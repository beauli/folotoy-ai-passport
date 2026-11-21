// main/app_zombie.c
// 豌豆射手守草坪的应用层:自绘游戏 UI(不使用参考示例的测试菜单与 ui_pixel 壳)、
// 游戏任务持有引擎并负责落盘,LVGL 定时器只负责渲染,按键回调只投递事件。
//
// 操作只有两个键:UP 键 = 往左,DOWN 键 = 往右,按住才走。射手自己吐豌豆,
// OK 键不参与玩法。GAME OVER 后按任意方向键重开。
#include "app_zombie.h"

#include "bsp_battery.h"
#include "bsp_display.h"
#include "bsp_pins.h"       // BSP_BTN_MV_TABLE:按键兜底核对用的是同一份分压窗口
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "zombie_game.h"
#include "zombie_store.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "z_app";

// ---- 版式(240 x 320,圆角遮罩半径 30,内容都避开四角) ----
// 场地与引擎坐标 1:1:实体都是场地的子对象,所以引擎给的 px 直接就是对象坐标,
// 超出场地的部分由 LVGL 裁剪 —— 僵尸从屏幕上方"走进来"就是靠这条裁剪实现的。
#define UI_EDGE_X        14   // 文字左边距,圆角遮罩在这一列以外已经不吃像素
#define UI_LINE1_Y       8
#define UI_LINE2_Y       22
#define UI_FIELD_Y       36   // 场地顶边:HUD 两行(每行 16px)之后
#define UI_FIELD_X       0
#define UI_HP_X          14
#define UI_HP_Y          23
#define UI_HP_W          60
#define UI_HP_H          5
#define UI_LIVES_X       80
#define UI_FOOTER_Y      292
#define UI_TEXT_W        212  // 整行文字标签的宽度,超出用省略号收掉,不许折行

#define UI_TICK_MS       40   // 引擎与渲染共用同一拍,免得两边各自算时间对不上

#define ZG_TASK_STACK    4096
#define ZG_KEY_QUEUE_LEN 16

// ---- 配色(白天草坪守线,植物大战僵尸那套底色) ----
#define COL_BG        0x14260E      // HUD 与提示条:压暗的草垄
#define COL_FIELD     0x3F7A32      // 草坪
#define COL_FIELD_ALT 0x37702B      // 草坪竖条,读出"这一垄/那一垄"
#define COL_FRAME     0x2B4A22
#define COL_TEXT      0xF2F7DF
#define COL_DIM       0xB2C497
#define COL_GOLD      0xFFD928
#define COL_MEDAL_DK  0xC8981C      // 奖章中心那一圈压深的金
#define COL_MEDAL_SILVER 0xC9D2DA   // 通关奖章另外两档:只剩 2 条命是银
#define COL_MEDAL_BRONZE 0xC0783C   // 丢过两条以上的人拿铜,配色故意暗一档
#define COL_POT       0xC0693A      // 花盆
#define COL_POT_RIM   0x9C4F27
#define COL_STALK     0x2F6B22      // 茎与叶背:比草坪深一档,免得糊在草里
#define COL_LEAF      0x6FBF3A
#define COL_HEAD      0x86CC3F      // 豌豆射手的头
#define COL_SNOUT     0x5EA232      // 炮口那一截
#define COL_PEA       0xB9E86A      // 豌豆
#define COL_HP        0xE4523B
#define COL_HP_BG     0x2A1A12
#define COL_LINE      0xE3D9A0      // 底边防线:一块压平的石板路
#define COL_SPAWN     0xD9B25A      // 突袭僵尸现身的那圈土
#define COL_EYE       0x24331C      // 僵尸眼窝

// 草坪色阶:每档对应一次换关(1/4/7/10 级),草越往后越暗,和"上午→傍晚"一个走向。
// 索引是 zg_lawn_group(level),第一档必须等于上面的 COL_FIELD / COL_FIELD_ALT。
static const uint32_t LAWN_BASE[ZG_LAWN_GROUPS] = { 0x3F7A32, 0x347127, 0x2A671F, 0x215C18 };
static const uint32_t LAWN_ALT[ZG_LAWN_GROUPS]  = { 0x37702B, 0x2C6620, 0x235C19, 0x1A5114 };

// 四种僵尸各配一对颜色,一眼能分清该先打谁:普通 / 铁桶 / 路障 / 矿工。
static const uint32_t Z_HEAD[ZG_Z_TYPE_COUNT] = { 0xAFC98A, 0xB6C0C7, 0xE08A2E, 0x8C6A3F };
static const uint32_t Z_BODY[ZG_Z_TYPE_COUNT] = { 0x4E6472, 0x3D4E5C, 0x556B78, 0x6B4A8C };
#define Z_ARM 0xAFC98A              // 手臂一律是皮肤的浅色,和破衣服分开才看得出是两只手

typedef enum {
    KEY_MSG_PRESS = 0,
    KEY_MSG_RELEASE,
    KEY_MSG_LONG,      // 只有 OK 键会投这个:长按才算"要暂停",免得误触
} key_msg_kind_t;

typedef struct {
    uint8_t btn;   // bsp_btn_t
    uint8_t ev;    // key_msg_kind_t
} key_msg_t;

// 每只僵尸/每个特效一组对象,槽位固定,只改可见性与坐标。
typedef struct {
    lv_obj_t *box;
    lv_obj_t *head;     // 僵尸头/桶/路障
    lv_obj_t *eye;      // 眼窝:一块暗色,免得 12px 宽的头里塞两只眼睛糊成一片
    lv_obj_t *torso;
    lv_obj_t *arm_l;    // 双臂朝前(即朝玩家方向)伸着,这是植物大战僵尸僵尸的剪影
    lv_obj_t *arm_r;
    int16_t look;     // 上一次上屏的外观签名
    bool shown;
} zview_t;

typedef struct {
    lv_obj_t *obj;
    int16_t look;     //  kind * 4 + 是否换过尺寸
    bool shown;
} fxview_t;

// 道具一组:外壳 + 中心记号。两种道具颜色差得开,但色弱或偏色屏上还要靠
// 中心那一块的形状区分,所以记号也按 kind 换配色。
typedef struct {
    lv_obj_t *box;
    lv_obj_t *mark;
    int8_t kind;      // 上一次上屏的道具种类,-1 = 还没画过
    bool shown;
} itemview_t;

static zg_game_t s_game;
static uint32_t s_best_at_boot;      // 本次开机时的纪录快照,用来判断"这局正在破纪录"
static bool s_nvs_ok = true;

static lv_obj_t *s_scr;
static lv_obj_t *s_field;
static lv_obj_t *s_rec_score;
static lv_obj_t *s_battery;
static lv_obj_t *s_hp_bg;
static lv_obj_t *s_hp_fill;
static lv_obj_t *s_lives;
static lv_obj_t *s_gun_label;
static lv_obj_t *s_footer;
static lv_obj_t *s_help_label;
static lv_obj_t *s_ready_label;
static lv_obj_t *s_over_label;
static lv_obj_t *s_over_score;
static lv_obj_t *s_banner;
static lv_obj_t *s_lawn_a;
static lv_obj_t *s_lawn_b;
static lv_obj_t *s_level_bar;
static lv_obj_t *s_level_label;
static lv_obj_t *s_level_sub;
static lv_obj_t *s_win_label;
static lv_obj_t *s_win_sub;
static lv_obj_t *s_medal;
static lv_obj_t *s_medal_disc;   // 金/银/铜只换这一圈的颜色
static lv_obj_t *s_medal_label;
static lv_obj_t *s_player;
static lv_obj_t *s_leaf_l;
static lv_obj_t *s_leaf_r;
static zview_t s_zview[ZG_POOL_ZOMBIES];
static lv_obj_t *s_bullet_view[ZG_POOL_BULLETS];
static fxview_t s_fx_view[ZG_POOL_FX];
static itemview_t s_item_view[ZG_POOL_ITEMS];
// 换关横幅与草坪色阶都只在"变了那一帧"改样式,所以各自留一份上一次的状态。
static bool s_level_bar_on;
static uint8_t s_level_shown;
static uint8_t s_lawn_shown = 0xFF;
static bool s_win_logged;   // 通关日志只打一次,重开时清掉

static lv_timer_t *s_timer;
static QueueHandle_t s_keys;
static SemaphoreHandle_t s_data_lock;
static SemaphoreHandle_t s_stopped;
static TaskHandle_t s_task;
static volatile bool s_stopping;
static uint8_t s_frame;              // 渲染计数,只用于走路与闪烁的节奏
// 电量是一次 I2C 读,不能在渲染帧里 25Hz 反复问:缓存约 2 秒,渲染只取缓存。
#define UI_SOC_REFRESH_FRAMES 50
static int s_soc_cache = -1;
static uint8_t s_soc_frame;

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

static bool data_lock(int timeout_ms)
{
    return s_data_lock && xSemaphoreTake(s_data_lock, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

static void data_unlock(void)
{
    if (s_data_lock) xSemaphoreGive(s_data_lock);
}

// ---- 小对象工具 ----

static lv_obj_t *add_box(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                         uint32_t color, int32_t radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    // 关滚动而不是清样式:和本仓库已上屏验证过的画法一致。
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    return obj;
}

static lv_obj_t *add_label(lv_obj_t *parent, int32_t x, int32_t y, const lv_font_t *font,
                           uint32_t color, lv_text_align_t align, int32_t width)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, "");
    lv_obj_set_style_pad_all(label, 0, 0);
    lv_obj_set_style_border_width(label, 0, 0);
    if (width > 0) {
        // 定宽 + 省略号:定宽就不折行,行与行不会互相压字。
        lv_obj_set_width(label, width);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    }
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, x, y);
    return label;
}

static void set_visible(lv_obj_t *obj, bool visible)
{
    if (!obj) return;
    if (visible) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

// 透明容器:只用来把几个部件捆在一起整体移动,自身不画东西。
static lv_obj_t *add_group(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *group = lv_obj_create(parent);
    lv_obj_remove_flag(group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(group, w, h);
    lv_obj_set_style_bg_opa(group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(group, 0, 0);
    lv_obj_set_style_pad_all(group, 0, 0);
    return group;
}

// ---- 豌豆射手(16 x 26 容器,炮口朝上对着压下来的僵尸) ----
static void build_player(void)
{
    s_player = add_group(s_field, ZG_PLAYER_W, ZG_PLAYER_H);
    add_box(s_player, 7, 11, 2, 8, COL_STALK, 0);           // 茎
    s_leaf_l = add_box(s_player, 1, 13, 6, 3, COL_LEAF, 2); // 两片叶:移动时错开摆动
    s_leaf_r = add_box(s_player, 9, 11, 6, 3, COL_LEAF, 2);
    add_box(s_player, 4, 19, 8, 7, COL_POT, 2);             // 花盆
    add_box(s_player, 3, 18, 10, 3, COL_POT_RIM, 1);        // 盆沿
    add_box(s_player, 3, 3, 10, 10, COL_HEAD, LV_RADIUS_CIRCLE);   // 脑袋
    add_box(s_player, 6, 0, 4, 5, COL_SNOUT, 2);            // 炮口:子弹判定盒正上方
}

// ---- 僵尸槽位 ----
static void build_zombies(void)
{
    for (unsigned i = 0; i < ZG_POOL_ZOMBIES; i++) {
        zview_t *v = &s_zview[i];
        v->box = add_group(s_field, 12, 14);
        lv_obj_set_style_border_color(v->box, lv_color_hex(0x1A2A12), 0);
        lv_obj_set_style_border_opa(v->box, LV_OPA_50, 0);
        v->torso = add_box(v->box, 1, 6, 10, 6, Z_BODY[0], 2);
        v->arm_l = add_box(v->box, 0, 8, 3, 5, Z_ARM, 1);
        v->arm_r = add_box(v->box, 9, 8, 3, 5, Z_ARM, 1);
        v->head = add_box(v->box, 3, 0, 7, 7, Z_HEAD[0], LV_RADIUS_CIRCLE);
        v->eye = add_box(v->box, 4, 2, 2, 2, COL_EYE, 1);
        v->look = -1;
        v->shown = false;
        set_visible(v->box, false);
    }
}

static void build_bullets(void)
{
    for (unsigned i = 0; i < ZG_POOL_BULLETS; i++) {
        // 画成一颗圆豌豆:判定盒仍是 3 x 8,视觉比判定略宽一点不影响手感。
        s_bullet_view[i] = add_box(s_field, 0, 0, 5, 5, COL_PEA, LV_RADIUS_CIRCLE);
        set_visible(s_bullet_view[i], false);
    }
}

static void build_fx(void)
{
    for (unsigned i = 0; i < ZG_POOL_FX; i++) {
        fxview_t *v = &s_fx_view[i];
        v->obj = add_box(s_field, 0, 0, 6, 6, COL_GOLD, LV_RADIUS_CIRCLE);
        v->look = -1;
        v->shown = false;
        set_visible(v->obj, false);
    }
}

// 道具:一个圆角盒 + 中心一块小方块。中心那块横着画是"多一颗豌豆"(和炮口同向),
// 竖着画是"吐得更快",两个剪影不认字也分得开。
static void build_items(void)
{
    for (unsigned i = 0; i < ZG_POOL_ITEMS; i++) {
        itemview_t *v = &s_item_view[i];
        v->box = add_box(s_field, 0, 0, ZG_ITEM_SIZE, ZG_ITEM_SIZE, COL_GOLD, 4);
        v->mark = add_box(v->box, 3, 5, 8, 4, COL_BG, 1);
        v->kind = -1;
        v->shown = false;
        set_visible(v->box, false);
    }
}

static void ui_build(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);

    // HUD 第一行:本局分数 + 历史纪录 + 右上角电量。
    s_rec_score = add_label(s_scr, UI_EDGE_X, UI_LINE1_Y, &lv_font_montserrat_14,
                            COL_TEXT, LV_TEXT_ALIGN_LEFT, UI_TEXT_W - 40);
    s_battery = add_label(s_scr, 0, UI_LINE1_Y, &lv_font_montserrat_14, COL_DIM,
                          LV_TEXT_ALIGN_RIGHT, 40);
    lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, -UI_EDGE_X, UI_LINE1_Y);

    // HUD 第二行:血量条 + 剩余命数 + 当前枪械档位。
    s_hp_bg = add_box(s_scr, UI_HP_X, UI_HP_Y, UI_HP_W, UI_HP_H, COL_HP_BG, 2);
    s_hp_fill = add_box(s_hp_bg, 0, 0, UI_HP_W, UI_HP_H, COL_HP, 2);
    s_lives = add_label(s_scr, UI_LIVES_X, UI_HP_Y - 2, &lv_font_montserrat_14, COL_TEXT,
                        LV_TEXT_ALIGN_LEFT, 40);
    s_gun_label = add_label(s_scr, 0, UI_HP_Y - 2, &lv_font_montserrat_14, COL_GOLD,
                            LV_TEXT_ALIGN_RIGHT, 130);
    lv_obj_align(s_gun_label, LV_ALIGN_TOP_RIGHT, -UI_EDGE_X, UI_HP_Y - 2);

    // 场地:引擎坐标原点。底边一条防线,漏过这条线就扣血。
    s_field = add_box(s_scr, UI_FIELD_X, UI_FIELD_Y, ZG_FIELD_W, ZG_FIELD_H, COL_FIELD, 0);
    // 两条暗垄把草坪分成三块:僵尸是顺着垄道往下压的,有垄道才读得出"草坪"。
    s_lawn_a = add_box(s_field, 60, 0, 60, ZG_FIELD_H, COL_FIELD_ALT, 0);
    s_lawn_b = add_box(s_field, 180, 0, 60, ZG_FIELD_H, COL_FIELD_ALT, 0);
    // 防线画在垄道之上,不然会被垄道压掉一截。
    add_box(s_field, 0, ZG_FIELD_H - 2, ZG_FIELD_W, 2, COL_LINE, 0);
    // 场地四周留一条很暗的框,免得实体贴到圆角遮罩上时看不出边界。
    lv_obj_set_style_border_width(s_field, 1, 0);
    lv_obj_set_style_border_color(s_field, lv_color_hex(COL_FRAME), 0);

    build_player();
    build_zombies();
    build_bullets();
    build_fx();
    build_items();

    // 覆盖层:开局/复活倒计时、结束画面、升枪横幅。
    s_ready_label = add_label(s_field, 0, 90, &lv_font_montserrat_20, COL_TEXT,
                              LV_TEXT_ALIGN_CENTER, ZG_FIELD_W);
    lv_obj_align(s_ready_label, LV_ALIGN_TOP_MID, 0, 90);
    // 升枪横幅挪到场地中下部:上半屏现在归换关横幅,两层同时亮时不互相盖字。
    s_banner = add_label(s_field, 0, 140, &lv_font_montserrat_20, COL_GOLD,
                         LV_TEXT_ALIGN_CENTER, ZG_FIELD_W);
    lv_obj_align(s_banner, LV_ALIGN_TOP_MID, 0, 140);
    s_over_label = add_label(s_field, 0, 70, &lv_font_montserrat_20, COL_HP,
                             LV_TEXT_ALIGN_CENTER, ZG_FIELD_W);
    lv_obj_align(s_over_label, LV_ALIGN_TOP_MID, 0, 70);
    s_over_score = add_label(s_field, 0, 100, &lv_font_montserrat_14, COL_TEXT,
                             LV_TEXT_ALIGN_CENTER, ZG_FIELD_W);
    lv_obj_align(s_over_score, LV_ALIGN_TOP_MID, 0, 100);
    // 按键说明卡:内容一辈子不变,所以建的时候写一次,之后只切可见性。
    // 摆在 SCORE 那行下面,开局/暂停/结束三种"场上没东西"的画面都放得下。
    s_help_label = add_label(s_field, 0, 122, &lv_font_montserrat_14, COL_TEXT,
                             LV_TEXT_ALIGN_CENTER, ZG_FIELD_W);
    lv_label_set_text(s_help_label,
                      "UP / DOWN   MOVE\n"
                      "KEEP HOLD   2X SPEED\n"
                      "OK HOLD     PAUSE\n"
                      "OK TAP      RESUME\n"
                      "GIFTS FALL  WALK UNDER");
    lv_obj_align(s_help_label, LV_ALIGN_TOP_MID, 0, 122);
    // 换关横幅:一条压暗的横带 + 级别 + 这一级新登场的那只僵尸。引擎在这 1.8 秒里
    // 不会刷出新怪,所以这条横幅一定读得完,不会被扑下来的僵尸打断。
    s_level_bar = add_box(s_field, 0, 56, ZG_FIELD_W, 52, COL_BG, 4);
    lv_obj_set_style_bg_opa(s_level_bar, LV_OPA_80, 0);
    s_level_label = add_label(s_level_bar, 0, 1, &lv_font_montserrat_20, COL_GOLD,
                              LV_TEXT_ALIGN_CENTER, ZG_FIELD_W);
    lv_obj_align(s_level_label, LV_ALIGN_TOP_MID, 0, 1);
    s_level_sub = add_label(s_level_bar, 0, 32, &lv_font_montserrat_14, COL_TEXT,
                            LV_TEXT_ALIGN_CENTER, ZG_FIELD_W);
    lv_obj_align(s_level_sub, LV_ALIGN_TOP_MID, 0, 32);
    // 通关画面:恭喜 + 一行说明 + 分数,再下面是一枚奖章(缎带 + 金盘 + 压深中心),
    // 奖章下面一行写清楚拿到的是哪一档、凭什么。
    s_win_label = add_label(s_field, 0, 52, &lv_font_montserrat_20, COL_GOLD,
                            LV_TEXT_ALIGN_CENTER, ZG_FIELD_W);
    lv_obj_align(s_win_label, LV_ALIGN_TOP_MID, 0, 52);
    s_win_sub = add_label(s_field, 0, 78, &lv_font_montserrat_14, COL_TEXT,
                          LV_TEXT_ALIGN_CENTER, ZG_FIELD_W);
    lv_obj_align(s_win_sub, LV_ALIGN_TOP_MID, 0, 78);
    s_medal = add_box(s_field, 100, 120, 40, 48, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_medal, LV_OPA_TRANSP, 0);
    add_box(s_medal, 13, 0, 14, 18, COL_HP, 1);            // 缎带
    s_medal_disc = add_box(s_medal, 5, 14, 30, 30, COL_GOLD, LV_RADIUS_CIRCLE);   // 金盘
    add_box(s_medal, 14, 23, 12, 12, COL_MEDAL_DK, LV_RADIUS_CIRCLE);
    s_medal_label = add_label(s_field, 0, 176, &lv_font_montserrat_14, COL_GOLD,
                              LV_TEXT_ALIGN_CENTER, ZG_FIELD_W);
    lv_obj_align(s_medal_label, LV_ALIGN_TOP_MID, 0, 176);
    set_visible(s_ready_label, false);
    set_visible(s_banner, false);
    set_visible(s_help_label, false);
    set_visible(s_over_label, false);
    set_visible(s_over_score, false);
    set_visible(s_level_bar, false);
    set_visible(s_win_label, false);
    set_visible(s_win_sub, false);
    set_visible(s_medal, false);
    set_visible(s_medal_label, false);

    s_footer = add_label(s_scr, UI_EDGE_X, UI_FOOTER_Y, &lv_font_montserrat_14, COL_DIM,
                         LV_TEXT_ALIGN_CENTER, UI_TEXT_W);
    lv_screen_load(s_scr);
}

// ---- 渲染 ----

// 外观签名:只有它变了才重设尺寸与配色,每帧都改样式会把渲染压成橡皮图章。
static int16_t zombie_look(const zg_zombie_t *z)
{
    return (int16_t)((int)z->type * 16 + (z->airborne ? 8 : 0) +
                     (z->telegraph_ms > 0 ? 4 : 0) + (z->flash_ms > 0 ? 2 : 0));
}

static void zview_apply(zview_t *v, const zg_zombie_t *z)
{
    const bool air = z->airborne;
    const int32_t w = air ? z->w + 6 : z->w;      // 腾空时放大,读出"扑过来"
    const int32_t h = air ? z->h + 6 : z->h;
    const bool ghost = (z->telegraph_ms > 0);     // 突袭僵尸现身的一瞬是半透明的
    const lv_opa_t opa = ghost ? LV_OPA_50 : LV_OPA_COVER;
    const bool hit = (z->flash_ms > 0);
    // 头一律是"帽色"(普通僵尸是皮肤,铁桶是铁皮,路障是橙色路障),被打中就整只泛白。
    const uint32_t head_col = hit ? 0xFFFFFF : Z_HEAD[z->type];
    const uint32_t shirt_col = hit ? 0xFFFFFF : Z_BODY[z->type];
    const uint32_t arm_col = hit ? 0xFFFFFF : Z_ARM;
    // 大头:植物大战僵尸的僵尸脑袋占身体一半以上,小身板反而更像那批建模。
    const int32_t head = (w * 3) / 5;
    const int32_t arm_w = (w >= 16) ? 4 : 3;
    const int32_t arm_h = (h >= 20) ? 7 : 5;
    const int32_t eye = (head >= 8) ? 3 : 2;

    lv_obj_set_size(v->box, w, h);
    lv_obj_set_pos(v->head, (w - head) / 2, 0);
    lv_obj_set_size(v->head, head, head);
    lv_obj_set_style_bg_color(v->head, lv_color_hex(head_col), 0);
    lv_obj_set_style_bg_opa(v->head, opa, 0);
    // 眼窝只点一颗,压在头的左侧:12px 宽的头里放两只眼睛会糊成一条暗带。
    lv_obj_set_pos(v->eye, (w - head) / 2 + 1, head / 3);
    lv_obj_set_size(v->eye, eye, eye);
    lv_obj_set_style_bg_opa(v->eye, opa, 0);
    lv_obj_set_pos(v->torso, 1, head);
    lv_obj_set_size(v->torso, w - 2, (h - head > 3) ? h - head - 1 : h - head);
    lv_obj_set_style_bg_color(v->torso, lv_color_hex(shirt_col), 0);
    lv_obj_set_style_bg_opa(v->torso, opa, 0);
    // 双臂朝前(即朝玩家、朝屏幕下方)伸着,贴在身体下缘两侧 —— 这是那批僵尸的剪影。
    lv_obj_set_pos(v->arm_l, 0, h - arm_h);
    lv_obj_set_size(v->arm_l, arm_w, arm_h);
    lv_obj_set_style_bg_color(v->arm_l, lv_color_hex(arm_col), 0);
    lv_obj_set_style_bg_opa(v->arm_l, opa, 0);
    lv_obj_set_pos(v->arm_r, w - arm_w, h - arm_h);
    lv_obj_set_size(v->arm_r, arm_w, arm_h);
    lv_obj_set_style_bg_color(v->arm_r, lv_color_hex(arm_col), 0);
    lv_obj_set_style_bg_opa(v->arm_r, opa, 0);
    lv_obj_set_style_border_width(v->box, air ? 1 : 0, 0);
}

static void render_zombies(void)
{
    for (unsigned i = 0; i < ZG_POOL_ZOMBIES; i++) {
        zview_t *v = &s_zview[i];
        const zg_zombie_t *z = &s_game.zombies[i];
        if (!z->alive) {
            if (v->shown) {
                set_visible(v->box, false);
                v->shown = false;
                v->look = -1;
            }
            continue;
        }
        // 腾空时左上角外扩 3px,才能保持中心不动。
        const int32_t air = z->airborne ? 3 : 0;
        lv_obj_set_x(v->box, ZG_POS_TO_PX(z->x) - air);
        lv_obj_set_y(v->box, ZG_POS_TO_PX(z->y) - air);
        const int16_t look = zombie_look(z);
        if (look != v->look) {
            zview_apply(v, z);
            v->look = look;
        }
        if (!v->shown) {
            set_visible(v->box, true);
            v->shown = true;
        }
    }
}

static void render_bullets(void)
{
    for (unsigned i = 0; i < ZG_POOL_BULLETS; i++) {
        const zg_bullet_t *b = &s_game.bullets[i];
        lv_obj_t *obj = s_bullet_view[i];
        if (!b->alive) {
            if (obj) set_visible(obj, false);
            continue;
        }
        lv_obj_set_x(obj, ZG_POS_TO_PX(b->x) - 1);
        lv_obj_set_y(obj, ZG_POS_TO_PX(b->y) + 1);
        set_visible(obj, true);
    }
}

// 道具:只在种类变化时改配色与形状,每帧只挪位置。
static void render_items(void)
{
    for (unsigned i = 0; i < ZG_POOL_ITEMS; i++) {
        itemview_t *v = &s_item_view[i];
        const zg_item_t *it = &s_game.items[i];
        if (!it->alive) {
            if (v->shown) {
                set_visible(v->box, false);
                v->shown = false;
                v->kind = -1;
            }
            continue;
        }
        lv_obj_set_x(v->box, ZG_POS_TO_PX(it->x));
        lv_obj_set_y(v->box, ZG_POS_TO_PX(it->y));
        if ((int8_t)it->kind != v->kind) {
            const bool rapid = (it->kind == ZG_ITEM_RAPID);
            v->kind = (int8_t)it->kind;
            lv_obj_set_style_bg_color(v->box, lv_color_hex(zg_item_info(it->kind)->color), 0);
            // 加速:竖着一道(往下冲的意思);加豆:横着一道(一次多吐一颗)。
            lv_obj_set_pos(v->mark, rapid ? 5 : 3, rapid ? 3 : 5);
            lv_obj_set_size(v->mark, rapid ? 4 : 8, rapid ? 8 : 4);
        }
        if (!v->shown) {
            set_visible(v->box, true);
            v->shown = true;
        }
    }
}

// 特效按 ttl 算透明度和大小:引擎负责寿命,UI 掉一帧也不会漏动画。
static void render_fx(void)
{
    bool banner = false;
    for (unsigned i = 0; i < ZG_POOL_FX; i++) {
        fxview_t *v = &s_fx_view[i];
        const zg_fx_t *fx = &s_game.fx[i];
        if (!fx->alive) {
            if (v->shown) {
                set_visible(v->obj, false);
                v->shown = false;
                v->look = -1;
            }
            continue;
        }
        const uint32_t percent = (uint32_t)(fx->ttl_ms * 100u / (fx->life_ms ? fx->life_ms : 1));
        const int32_t fade = (int32_t)(255u * percent / 100u);
        int32_t size = 6;
        uint32_t color = COL_GOLD;
        bool ring = false;

        switch (fx->kind) {
        case ZG_FX_MUZZLE:
            size = 6; color = 0xD9F58A; break;      // 炮口那一下:亮绿的豆光
        case ZG_FX_HIT:
            size = 5; color = 0xFFFFFF; break;
        case ZG_FX_DEATH:
            // 僵尸被打散是一撮灰绿的粉,在草坪上要用浅色才看得见。
            size = 6 + (int32_t)(12 * percent / 100); color = 0xC9CE9E; break;
        case ZG_FX_LEAK:
            size = 4; color = COL_HP; break;
        case ZG_FX_SPAWN:
            size = 10 + (int32_t)(14 * (100 - percent) / 100); color = COL_SPAWN;
            ring = true; break;
        case ZG_FX_HURT:
            size = 8; color = COL_HP; break;
        case ZG_FX_GUNUP:
            banner = true; break;
        case ZG_FX_PICKUP:
            // 接到道具:一圈往外扩的白环,和"打死僵尸的灰绿粉"区分开。
            size = 8 + (int32_t)(16 * (100 - percent) / 100); color = 0xFFFFFF;
            ring = true; break;
        default:
            break;
        }

        if (fx->kind == ZG_FX_LEAK) {
            // 漏怪沿整条防线闪一下,和"撞到人身上的红点"要区分开。
            lv_obj_set_pos(v->obj, 0, ZG_FIELD_H - 6);
            lv_obj_set_size(v->obj, ZG_FIELD_W, 4);
            lv_obj_set_style_radius(v->obj, 0, 0);
        } else if (fx->kind == ZG_FX_HURT) {
            // 挨打:在豌豆射手位置画一圈红,比整屏变红更容易看出是谁动的手。
            lv_obj_set_pos(v->obj, ZG_POS_TO_PX(s_game.player_x) - 4,
                           ZG_POS_TO_PX(s_game.player_y) - 4);
            lv_obj_set_size(v->obj, ZG_PLAYER_W + 8, ZG_PLAYER_H + 8);
            lv_obj_set_style_radius(v->obj, LV_RADIUS_CIRCLE, 0);
        } else {
            // 引擎的特效坐标就是 px(中心点),不再过一遍定点转换。
            const int32_t cx = fx->x;
            lv_obj_set_pos(v->obj, cx - size / 2, (int32_t)fx->y - size / 2);
            lv_obj_set_size(v->obj, size, size);
            lv_obj_set_style_radius(v->obj, ring ? LV_RADIUS_CIRCLE : (size / 3), 0);
        }
        lv_obj_set_style_bg_color(v->obj, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(v->obj, (lv_opa_t)(fade > 255 ? 255 : fade), 0);
        lv_obj_set_style_border_width(v->obj, ring ? 2 : 0, 0);
        lv_obj_set_style_border_color(v->obj, lv_color_hex(color), 0);
        lv_obj_set_style_border_opa(v->obj, (lv_opa_t)(fade > 255 ? 255 : fade), 0);
        set_visible(v->obj, fx->kind != ZG_FX_GUNUP);
        v->shown = fx->kind != ZG_FX_GUNUP;
        v->look = (int16_t)fx->kind;
    }

    if (banner) {
        lv_label_set_text_fmt(s_banner, "%s UP!", zg_weapon_info(s_game.weapon)->name);
        set_visible(s_banner, true);
    } else {
        set_visible(s_banner, false);
    }
}

// 换关的两处可见变化:每一级都亮横幅,第 4/7/10 级再换一档草坪颜色。
static void render_level(void)
{
    const uint8_t group = zg_lawn_group(s_game.level);
    if (group != s_lawn_shown) {
        s_lawn_shown = group;
        lv_obj_set_style_bg_color(s_field, lv_color_hex(LAWN_BASE[group]), 0);
        lv_obj_set_style_bg_color(s_lawn_a, lv_color_hex(LAWN_ALT[group]), 0);
        lv_obj_set_style_bg_color(s_lawn_b, lv_color_hex(LAWN_ALT[group]), 0);
    }

    // 横幅只在游玩中亮:倒计时和结束画面那块位置已经有别的字在用,
    // 暂停时也把它收起来,免得 PAUSED 被横带盖掉半截。
    const bool show = (s_game.phase == ZG_PHASE_PLAY && s_game.levelup_ms > 0 &&
                       !s_game.paused);
    if (!show) {
        if (s_level_bar_on) {
            s_level_bar_on = false;
            s_level_shown = 0;
            set_visible(s_level_bar, false);
        }
        return;
    }
    // 同一级别只写一次字,后面几十帧只保持可见:改文本会让 LVGL 重排整个标签。
    if (s_level_shown == s_game.level) return;
    s_level_bar_on = true;
    s_level_shown = s_game.level;
    const zg_zombie_info_t *nz = zg_unlock_at(s_game.level);
    lv_label_set_text_fmt(s_level_label, "LEVEL %u", (unsigned)s_game.level);
    if (nz) {
        lv_label_set_text_fmt(s_level_sub, "NEW: %s", nz->name);
    } else if (s_game.level >= ZG_LEVEL_MAX) {
        lv_label_set_text(s_level_sub, "MAX LEVEL");
    } else {
        lv_label_set_text(s_level_sub, "TOUGHER NOW");
    }
    set_visible(s_level_bar, true);
}

static void render_hud(void)
{
    // 一行写完分数与纪录:两段各自定宽会互相挤,拼一条反而稳定。
    if (s_game.score > s_best_at_boot && s_game.phase == ZG_PHASE_PLAY) {
        lv_label_set_text_fmt(s_rec_score, "SC %lu  NEW HI %lu",
                              (unsigned long)s_game.score, (unsigned long)s_game.best);
        lv_obj_set_style_text_color(s_rec_score, lv_color_hex(COL_GOLD), 0);
    } else {
        lv_label_set_text_fmt(s_rec_score, "SC %lu  HI %lu",
                              (unsigned long)s_game.score, (unsigned long)s_game.best);
        lv_obj_set_style_text_color(s_rec_score, lv_color_hex(COL_TEXT), 0);
    }

    // 血条按 10 格等分,少一格就短 6px,不用数字也能一眼看清。
    lv_obj_set_width(s_hp_fill, (int32_t)s_game.hp * (UI_HP_W / ZG_HP_MAX));
    lv_label_set_text_fmt(s_lives, "x%u", (unsigned)s_game.lives);

    // 级别写成分母形式(共 10 级),后面是这一级该拿到的枪档。
    const zg_weapon_info_t *w = zg_weapon_info(s_game.weapon);
    lv_label_set_text_fmt(s_gun_label, "L%u/%u %s", (unsigned)s_game.level,
                          (unsigned)ZG_LEVEL_MAX, w->name);

    const int soc = s_soc_cache;
    lv_label_set_text_fmt(s_battery, (soc < 0) ? "--" : "%d%%", soc);
}

static void render_player(void)
{
    lv_obj_set_x(s_player, ZG_POS_TO_PX(s_game.player_x));
    lv_obj_set_y(s_player, ZG_POS_TO_PX(s_game.player_y));
    // 无敌期每 4 帧隐一次,既说明"现在撞不死",也不遮怪。
    const bool blink = s_game.invuln_ms > 0 && (s_frame % 6) < 3;
    set_visible(s_player, !blink);
    // 植物不迈步:移动时把两片叶子上下错开一点,读成"端着花盆在挪"。
    // 加速档把摆动周期减半,不认字也看得出现在是双倍速度;暂停时叶子也不许动。
    const uint32_t period = (s_game.hold_ms >= ZG_SPRINT_MS) ? 2u : 4u;
    const bool walk = !s_game.paused && s_game.move_dir != 0 &&
                      ((s_frame % period) < period / 2);
    lv_obj_set_y(s_leaf_l, walk ? 12 : 13);
    lv_obj_set_y(s_leaf_r, walk ? 12 : 11);
}

static void render_overlays(void)
{
    // 先全部收起,再按当前画面把要露出来的那几个亮出去:这些覆盖层两两互斥,
    // 每个 case 各写一遍可见性迟早会漏一个,留下上一屏的字压在下一屏上。
    set_visible(s_ready_label, false);
    set_visible(s_over_label, false);
    set_visible(s_over_score, false);
    set_visible(s_win_label, false);
    set_visible(s_win_sub, false);
    set_visible(s_medal, false);
    set_visible(s_medal_label, false);

    // 按键卡只在这几屏出现:开局、暂停、结束——场上没有东西要看,才读得完。
    bool help = false;
    switch (s_game.phase) {
    case ZG_PHASE_READY:
        // 倒计时也归暂停管:冻结时若还写着 GET READY,会看不出手上的键已经把时间停了。
        lv_label_set_text(s_ready_label, s_game.paused ? "PAUSED" : "GET READY");
        set_visible(s_ready_label, true);
        help = true;
        lv_label_set_text(s_footer, s_game.paused ? "TAP OK TO RESUME"
                           : (s_nvs_ok ? "" : "NO STORAGE - SCORE NOT SAVED"));
        break;
    case ZG_PHASE_PLAY:
        if (s_game.paused) {
            lv_label_set_text(s_ready_label, "PAUSED");
            set_visible(s_ready_label, true);
            help = true;
            lv_label_set_text(s_footer, "TAP OK TO RESUME");
        } else if (s_game.rapid_ms > 0 && s_game.bonus_shots > 0) {
            lv_label_set_text_fmt(s_footer, "FAST %us  PEAS x%u",
                                  (unsigned)((s_game.rapid_ms + 999u) / 1000u),
                                  (unsigned)zg_volley(&s_game));
        } else if (s_game.rapid_ms > 0) {
            lv_label_set_text_fmt(s_footer, "FAST %us",
                                  (unsigned)((s_game.rapid_ms + 999u) / 1000u));
        } else if (s_game.bonus_shots > 0) {
            lv_label_set_text_fmt(s_footer, "PEAS x%u", (unsigned)zg_volley(&s_game));
        } else {
            lv_label_set_text(s_footer, "UP/DOWN MOVE  OK PAUSE");
        }
        break;
    case ZG_PHASE_OVER:
        lv_label_set_text(s_over_label, "GAME OVER");
        lv_label_set_text_fmt(s_over_score, "SCORE %lu  KILLS %u",
                              (unsigned long)s_game.score, (unsigned)s_game.kills);
        set_visible(s_over_label, true);
        set_visible(s_over_score, true);
        help = true;
        lv_label_set_text(s_footer, "PRESS LEFT OR RIGHT TO RETRY");
        break;
    case ZG_PHASE_WIN:
    default: {
        // 奖励按还剩几条命定档:通关本身就够难,档位再高说明"一路没挨穿"。
        const zg_medal_t medal = zg_medal_of(s_game.lives);
        const uint32_t disc = (medal == ZG_MEDAL_GOLD) ? COL_GOLD :
                              (medal == ZG_MEDAL_SILVER) ? COL_MEDAL_SILVER : COL_MEDAL_BRONZE;
        lv_obj_set_style_bg_color(s_medal_disc, lv_color_hex(disc), 0);
        lv_label_set_text(s_win_label, "CONGRATULATIONS!");
        lv_label_set_text_fmt(s_win_sub, "ALL %u LEVELS CLEARED", (unsigned)ZG_LEVEL_MAX);
        lv_label_set_text_fmt(s_over_score, "SCORE %lu  KILLS %u",
                              (unsigned long)s_game.score, (unsigned)s_game.kills);
        lv_label_set_text_fmt(s_medal_label, "%s MEDAL  %u LIVES LEFT",
                              zg_medal_name(medal), (unsigned)s_game.lives);
        set_visible(s_win_label, true);
        set_visible(s_win_sub, true);
        set_visible(s_over_score, true);
        set_visible(s_medal, true);
        set_visible(s_medal_label, true);
        lv_label_set_text(s_footer, "PRESS LEFT OR RIGHT TO REPLAY");
        break;
    }
    }
    set_visible(s_help_label, help);
}

// 画一帧。调用方必须已经持有 LVGL 上下文(定时器跑在 LVGL 任务上,
// app_zombie_start 里则是先 lock 再画):esp_lvgl_port 的互斥量不可重入,
// 在这里再加一次锁会直接死锁。
static void render_locked(void)
{
    if (!s_scr) return;
    // 电量是总线上的一次读:放在数据锁之前,并且约 2 秒才问一次,
    // 免得游戏任务为了拿锁多等一趟 I2C。
    if (s_soc_frame == 0) {
        s_soc_cache = bsp_battery_soc();
        s_soc_frame = UI_SOC_REFRESH_FRAMES;
    } else {
        s_soc_frame--;
    }
    if (!data_lock(120)) return;
    s_frame++;
    render_hud();
    render_player();
    render_zombies();
    render_bullets();
    render_items();
    render_fx();
    render_overlays();
    render_level();
    data_unlock();
}

static void update_ui(lv_timer_t *timer)
{
    (void)timer;
    render_locked();
}

// ---- 按键 ----

// 事件是唯一的操作来源,但队列满时可能丢一次 RELEASE,那样人会一直朝一个方向走。
// 所以每帧再拿真实分压核对一次"还按住吗",只用来撤销,不用来新增。
static bool mv_holds(int mv, bsp_btn_t btn)
{
    static const uint16_t windows[BSP_BTN_COUNT][2] = BSP_BTN_MV_TABLE;
    if (mv < 0) return false;
    const uint16_t v = (uint16_t)mv;
    return v >= windows[btn][0] && v < windows[btn][1];
}

static void resync_keys(bool *left, bool *right)
{
    if (!*left && !*right) return;          // 没人按住时不必碰 ADC
    const int mv = bsp_button_read_mv();
    if (mv < 0) return;                     // 读不到就维持现状,不做猜测
    if (*left && !mv_holds(mv, BSP_BTN_UP)) *left = false;
    if (*right && !mv_holds(mv, BSP_BTN_DOWN)) *right = false;
}

// 跑在共享 esp_timer 任务上:只投递,不碰 LVGL,也不碰引擎。
void app_zombie_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    // OK 只管暂停:按下(含已经暂停时的恢复按)投 PRESS,按住到门限投 LONG。
    // 移动键不用 LONG,所以这里分两支各收自己关心的事件。
    const bool is_ok = (btn == BSP_BTN_OK);
    if (is_ok) {
        if (ev != BSP_BTN_PRESS && ev != BSP_BTN_LONG) return;
    } else if (ev != BSP_BTN_PRESS && ev != BSP_BTN_RELEASE) {
        return;
    }
    uint8_t kind;
    if (ev == BSP_BTN_PRESS) kind = KEY_MSG_PRESS;
    else if (ev == BSP_BTN_RELEASE) kind = KEY_MSG_RELEASE;
    else kind = KEY_MSG_LONG;
    const key_msg_t msg = { .btn = (uint8_t)btn, .ev = kind };
    if (s_keys) (void)xQueueSend(s_keys, &msg, 0);
}

// ---- 游戏任务 ----

static void game_task(void *arg)
{
    (void)arg;
    bool left = false;
    bool right = false;
    int64_t last = now_us();

    while (!s_stopping) {
        bool want_restart = false;
        bool want_pause = false;
        bool want_resume = false;
        key_msg_t msg;
        while (xQueueReceive(s_keys, &msg, 0) == pdTRUE) {
            const bool press = (msg.ev == KEY_MSG_PRESS);
            if (msg.btn == BSP_BTN_UP) {
                left = press;
                if (press) want_restart = true;
            } else if (msg.btn == BSP_BTN_DOWN) {
                right = press;
                if (press) want_restart = true;
            } else if (msg.btn == BSP_BTN_OK) {
                // 长按才进暂停(误触的代价太高),恢复用一次普通按下就够。
                if (msg.ev == KEY_MSG_LONG) want_pause = true;
                else if (press) want_resume = true;
            }
        }
        resync_keys(&left, &right);

        const int64_t now = now_us();
        const uint16_t dt = (uint16_t)((now - last) / 1000);
        last = now;
        uint32_t save_best = 0;
        bool need_save = false;

        if (data_lock(200)) {
            if (want_restart) {
                // 只有 GAME OVER / 通关时左右键才是重开;游玩中它仍然是移动,不吞按键。
                if (zg_restart(&s_game)) s_win_logged = false;
            }
            if (want_pause) zg_set_paused(&s_game, !s_game.paused);
            if (want_resume) zg_set_paused(&s_game, false);
            zg_set_keys(&s_game, left, right);
            zg_step(&s_game, dt);
            if (s_game.phase == ZG_PHASE_WIN && !s_win_logged) {
                // 通关只此一次,留一行日志:上板验收时这是唯一的客观凭据。
                s_win_logged = true;
                ESP_LOGI(TAG, "通关!%lus 打完 %u 关,分数 %lu,打死 %u 只,奖章 %s(剩 %u 命)",
                         (unsigned long)(s_game.elapsed_ms / 1000u), (unsigned)ZG_LEVEL_MAX,
                         (unsigned long)s_game.score, (unsigned)s_game.kills,
                         zg_medal_name(zg_medal_of(s_game.lives)), (unsigned)s_game.lives);
            }
            if (s_game.best_dirty) {
                save_best = s_game.best;
                need_save = true;
                s_game.best_dirty = false;   // 落盘在锁外做,标记已经接手
            }
            data_unlock();
        }
        // Flash 写不进 LVGL 任务,也不在按键回调里:一局最多走这一次。
        if (need_save && zombie_store_save_best(save_best)) {
            // 纪录已经落盘,下一局的"破纪录"高亮要跟新纪录比,否则一直显示 NEW HI。
            s_best_at_boot = save_best;
        } else if (need_save) {
            s_nvs_ok = false;
        }
        vTaskDelay(pdMS_TO_TICKS(UI_TICK_MS));
    }
    if (s_stopped) xSemaphoreGive(s_stopped);
}

// ---- 生命周期 ----

esp_err_t app_zombie_start(void)
{
    if (s_scr) return ESP_ERR_INVALID_STATE;

    uint32_t best = 0;
    s_nvs_ok = zombie_store_init(&best);
    s_best_at_boot = best;
    // 种子用启动时刻:每次开机都刷出不同的一局,host 侧的可复现性由
    // zombie_game 的种子参数保证(测试里传固定种子)。
    zg_init(&s_game, (uint32_t)now_us(), best);
    s_win_logged = false;

    s_keys = xQueueCreate(ZG_KEY_QUEUE_LEN, sizeof(key_msg_t));
    s_data_lock = xSemaphoreCreateMutex();
    s_stopped = xSemaphoreCreateBinary();
    if (!s_keys || !s_data_lock || !s_stopped) goto fail_services;

    if (!bsp_lvgl_lock(1000)) goto fail_services;
    ui_build();
    s_timer = lv_timer_create(update_ui, UI_TICK_MS, NULL);
    render_locked();                    // 先画一帧,不用等定时器第一个周期
    // 界面一次要建几十个对象,LVGL 那个静态池够不够只有在这块板上才看得出来,
    // 所以把水位打出来:池子满了 lv_obj_create 返回 NULL,那就不是掉帧而是直接崩。
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "LVGL 堆:已用 %u / 峰值 %u / 共 %u 字节",
             (unsigned)(mon.total_size - mon.free_size), (unsigned)mon.max_used,
             (unsigned)mon.total_size);
    bsp_lvgl_unlock();

    if (xTaskCreate(game_task, "zombie_game", ZG_TASK_STACK, NULL, 6, &s_task) != pdPASS) {
        s_task = NULL;
        goto fail_page;
    }
    ESP_LOGI(TAG, "游戏就绪:UP=左 DOWN=右(按住 0.5 秒翻倍),OK 长按暂停,每 %ld 分换关共 %d 关,%ld 分通关,历史最高分 %lu",
             (long)ZG_LEVEL_SCORE_STEP, ZG_LEVEL_MAX, (long)ZG_WIN_SCORE,
             (unsigned long)best);
    return ESP_OK;

fail_page:
    if (bsp_lvgl_lock(500)) {
        if (s_timer) {
            lv_timer_delete(s_timer);
            s_timer = NULL;
        }
        if (s_scr) {
            lv_obj_delete(s_scr);
            s_scr = NULL;
        }
        bsp_lvgl_unlock();
    }
fail_services:
    if (s_keys) { vQueueDelete(s_keys); s_keys = NULL; }
    if (s_data_lock) { vSemaphoreDelete(s_data_lock); s_data_lock = NULL; }
    if (s_stopped) { vSemaphoreDelete(s_stopped); s_stopped = NULL; }
    s_stopping = false;
    return ESP_ERR_NO_MEM;
}

esp_err_t app_zombie_stop(void)
{
    if (!s_task) return ESP_OK;

    s_stopping = true;
    // 页面删除前必须先让生产者退出:定时器与 UI 对象不能再被游戏任务碰到。
    if (xSemaphoreTake(s_stopped, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGE(TAG, "游戏任务未在时限内退出,保留页面与引擎状态");
        return ESP_ERR_TIMEOUT;
    }
    s_task = NULL;
    s_stopping = false;

    if (bsp_lvgl_lock(500)) {
        if (s_timer) {
            lv_timer_delete(s_timer);
            s_timer = NULL;
        }
        if (s_scr) {
            lv_obj_delete(s_scr);
            s_scr = NULL;
        }
        bsp_lvgl_unlock();
    }
    if (s_keys) { vQueueDelete(s_keys); s_keys = NULL; }
    if (s_data_lock) { vSemaphoreDelete(s_data_lock); s_data_lock = NULL; }
    if (s_stopped) { vSemaphoreDelete(s_stopped); s_stopped = NULL; }
    return ESP_OK;
}
