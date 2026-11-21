// main/zombie_game.h
// 豌豆射手守草坪的游戏引擎:纯 C 逻辑,不含 ESP-IDF/LVGL/浮点,由
// tests/test_zombie_game.c 在主机上覆盖。坐标系与 UI 1:1 对应,单位像素。
//
// 玩法:竖屏顶视。花盆里的豌豆射手守着屏幕底边,只能左右移动;僵尸从上方往下压,
// 射手自动朝上吐豌豆。僵尸被打死就得分,分数每满一档就换下一关(共 10 关),
// 换关时亮横幅并在横幅期间停刷怪。天上还会掉下道具:接到就是射速翻倍 20 秒、
// 一次多吐一颗豌豆(整场最多 4 颗),或者一张克隆卡片。
// 克隆卡片当场在草坪底边种出 3 个克隆豌豆射手,各自吐豌豆打僵尸,一直打到这一条命
// 打完;代价是 10 秒后僵尸也学会克隆,在随后 20 秒里每只僵尸复制一个血量减半的分身。
// 僵尸漏到底边或撞到豌豆射手都扣 1 点血,10 点血扣光丢 1 条命,3 条命丢光结束。
// 分数打满第 10 关就是通关:游戏结束,按剩余命数发奖章,和 GAME OVER 一样等一次按键重开。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// ---- 场地与资源上限(固定池,无动态分配) ----
#define ZG_FIELD_W        240   // 与 BSP_LCD_W 一致
#define ZG_FIELD_H        250   // 屏幕扣掉 HUD 与底部提示后的可玩高度
// 僵尸池分两层:刷怪只按 ZG_SPAWN_CAP 走(难度曲线靠它),多出来的名额专门留给
// 克隆卡片的反噬——否则"僵尸也会复制"在后期一只都塞不进来,等于没有缺点。
#define ZG_POOL_ZOMBIES     8   // 同屏最多几只僵尸(含分身)
#define ZG_SPAWN_CAP        6   // 正常刷怪的同屏上限
#define ZG_POOL_BULLETS    14   // 同屏最多几颗子弹(克隆射手也要占弹道)
#define ZG_POOL_FX          8   // 同屏最多几个特效
#define ZG_POOL_ITEMS       2   // 同屏最多几个道具(多了读不过来,也接不完)

#define ZG_HP_MAX          10   // 一格血掉满 10 次才丢一条命
#define ZG_LIVES_MAX        3

// 坐标用 1/16 像素定点数:整数运算也能表达亚像素速度,慢速僵尸不会步进抖动。
#define ZG_POS_SHIFT        4
#define ZG_POS_TO_PX(v)     ((int16_t)((v) >> ZG_POS_SHIFT))
#define ZG_PX_TO_POS(px)    ((int32_t)(px) << ZG_POS_SHIFT)

// 精灵整体比初版放大约 1.5 倍:240x250 的场地上 16x26 的射手和 12x14 的僵尸
// 在手里那块小屏上几乎看不清在打谁。放大只动碰撞盒与画法,不动速度/血量。
#define ZG_PLAYER_W        24
#define ZG_PLAYER_H        38
#define ZG_PLAYER_SPEED   150   // 刚按下左右键时的移动速度,px/s
// 只有一个移动键可用,所以把"按住时长"当成第二个档位:同一个方向连续按满
// ZG_SPRINT_MS 就翻倍。点按仍能微调站位,长按用来赶路/救场。
#define ZG_SPRINT_MS      500
#define ZG_SPRINT_MUL       2
#define ZG_BULLET_W         4
#define ZG_BULLET_H        10
// 弹速不在这里定,而是每档枪各带一个 bullet_speed:开局那颗豌豆慢,升档才变快。

// ---- 节奏与判定门限 ----
// 这些数值同时被 UI(画倒计时/预警)和 host 测试(钉住手感)读取,所以放在头文件,
// 不在实现里再抄一份。
#define ZG_DT_MIN_MS        5   // 低于此值当作抖动丢弃,避免高速除小 dt 丢精度
#define ZG_DT_MAX_MS       60   // 补帧上限:再大就可能出现子弹一步跨过僵尸
#define ZG_READY_FIRST_MS 2000  // 开局的 GET READY
#define ZG_READY_MS      1500   // 丢命后的 GET READY
#define ZG_INVULN_HURT_MS 1200  // 挨一下之后的无敌:否则一群僵尸叠上来会一秒扣光 10 血
#define ZG_INVULN_RESPAWN_MS 2200  // 复活无敌比挨打无敌长,免得落地立刻被同一波带走
#define ZG_TELEGRAPH_MS   450   // 突袭僵尸现身预警
#define ZG_LEVEL_SCORE_STEP 1000  // 每 1000 分换一关:第一关 1000,第二关 2000……
#define ZG_LEVEL_MAX        10   // 一共 10 关;打完第 10 关就通关,不再往上涨
#define ZG_WIN_SCORE (ZG_LEVEL_SCORE_STEP * ZG_LEVEL_MAX)   // 通关分数线:10000
#define ZG_LEVELUP_MS     1800  // 升级横幅时长,这期间场上不再刷新的僵尸
#define ZG_LAWN_GROUPS       4   // 草坪色阶档数:第 1/4/7/10 关各换一次草地颜色
#define ZG_HOP_MS         300   // 跳跃僵尸腾空时长
#define ZG_HOP_WAIT_MS    800   // 落地蓄力时长
#define ZG_CRAWL_DIV        6   // 蓄力期间按 1/6 速度往前蹭,不是完全站死

// ---- 道具 ----
// 道具从屏幕上方飘下来,靠左右走位去接;漏了没有任何惩罚,接到才有效。
// 间隔随关卡收紧,所以越往后越是"顺手就能拿到强化",压力主要来自怪的数量。
#define ZG_ITEM_SIZE         18  // 碰撞盒与绘制尺寸一致(卡片按这个宽度竖着画)
#define ZG_ITEM_VY           55  // 下落速度 px/s:比最快的僵尸慢,追得上也接得住
#define ZG_ITEM_FIRST_MS   8000  // 开局第一件道具出现的时间
#define ZG_ITEM_STEP_MS   12000  // 之后每隔这么久掉一件
#define ZG_ITEM_MIN_MS    11000  // 难度修正后也不快过这个间隔
#define ZG_RAPID_MS      20000   // 射速翻倍持续 20 秒
#define ZG_RAPID_DIV         2   // 20 秒内开火间隔按 1/2 算
#define ZG_MAX_SHOTS         4   // 一次最多吐几颗豌豆(道具加成的硬顶)

// ---- 克隆卡片 ----
// 接到卡片:草坪底边当场长出 3 个克隆豌豆射手,各自独立吐豌豆,直到这一条命打完
// (丢命连带清掉,反噬也一起取消——不能让人挨了惩罚却没拿到好处)。
// 卡片的代价:接住 10 秒后僵尸学会同一招,在随后 20 秒里每只僵尸复制一个分身。
#define ZG_POOL_CLONES         3  // 一张卡片最多同时有几个克隆射手
#define ZG_CLONE_W            20
#define ZG_CLONE_H            30
#define ZG_CLONE_SLOT_STEP   100  // 三个克隆等间距钉在底边,第一个从这一列起
#define ZG_CLONE_SLOT_X0       8
#define ZG_CLONE_FIRE_MS    1100  // 单个克隆的开火间隔:3 个加起来约等于第二把枪
#define ZG_CLONE_SHOTS         1  // 克隆一次只吐一颗直上直下的豌豆
#define ZG_CLONE_DAMAGE        1  // 比本体低一档,升枪的收益仍然归本体
#define ZG_CLONE_SPEED      260   // 克隆的豌豆弹速 px/s
#define ZG_CLONE_STAGGER_MS 260  // 三个克隆错开相位,避免每 1.1 秒同步齐射
#define ZG_ZOMBIE_LEARN_MS 10000 // 接住卡片到僵尸学会克隆之间的空窗
#define ZG_ZOMBIE_CLONE_MS 20000 // 学会之后持续复制这么久
#define ZG_ZOMBIE_CLONE_STEP_MS 3000  // 复制窗口里每隔这么久放出一只分身
#define ZG_ZCLONE_HP_DIV        2     // 分身血量按母体剩余血量的一半算
#define ZG_ZCLONE_SCORE_DIV     2     // 分身分值同样减半:它不该比本体值钱

typedef enum {
    ZG_PHASE_READY = 0,   // 开局或刚丢命:短暂的 "GET READY",不生成也不判定
    ZG_PHASE_PLAY,        // 正常游玩
    ZG_PHASE_OVER,        // GAME OVER,等一次按键重开
    ZG_PHASE_WIN,         // 10 关全通关:恭喜 + 奖章,同样等一次按键重开
} zg_phase_t;

// 豌豆枪档位:跟着关卡走,换关就换枪,玩家不需要额外操作。
// 发数最多 4(和道具加成共用同一个顶),三个维度一起往前推:弹速 220→440、
// 间隔 900→300、伤害 1→4。第 1 关就是要"慢慢吐一颗、打两下才死"。
typedef enum {
    ZG_WPN_PEA = 0,       // 单发慢豌豆:第 1~2 关就这把,打的是"来几只好几只"
    ZG_WPN_SPLIT,         // 两发分左右:第一次有第二个方向(第 3 关)
    ZG_WPN_THREE,         // 三发扇形,单发两伤(第 5 关)
    ZG_WPN_RAPID,         // 三发且吐得极快(第 7 关)
    ZG_WPN_RAILGUN,       // 四发宽扇形,四伤可穿透(第 9 关)
    ZG_WPN_COUNT
} zg_weapon_t;

typedef struct {
    const char *name;      // 右上角 HUD 显示的武器名
    uint16_t interval_ms;  // 自动开火的间隔
    uint8_t shots;         // 一次开火打出的子弹数
    uint8_t spread_px;     // 相邻子弹的横向间隔(0 = 全部重合)
    uint8_t damage;        // 单发伤害
    uint8_t pierce;        // 单发能穿过的僵尸数(0 = 打中即消失)
    uint16_t bullet_speed; // 豌豆飞行速度 px/s,越低越像"慢慢吐出去"
    uint8_t first_level;   // 从第几关起手里是这一档
} zg_weapon_info_t;

// 道具类型。前两种是纯收益,第三种是"收益 + 延后到账的代价":卡片当场给 3 个克隆
// 射手,但 10 秒后僵尸也开始复制。玩家不需要认字,只要那 10 秒里看见草坪两侧多了自己。
typedef enum {
    ZG_ITEM_RAPID = 0,    // 射速翻倍,持续 ZG_RAPID_MS
    ZG_ITEM_PEA,          // 一次多吐一颗豌豆,最多加满 ZG_MAX_SHOTS 颗
    ZG_ITEM_CLONE,        // 克隆 3 个豌豆射手,反噬见 ZG_ZOMBIE_* 一组常量
    ZG_ITEM_COUNT
} zg_item_kind_t;

typedef struct {
    const char *name;      // 横幅与日志里标识用
    uint32_t color;        // UI 取色用:引擎不画,但两处必须认得出同一件道具
} zg_item_info_t;

typedef struct {
    int32_t x, y;                // 1/16 px,左上角
    uint16_t vy;                 // 下落速度 px/s
    bool alive;
    zg_item_kind_t kind;
} zg_item_t;

// 僵尸类型。四种的行为差别是玩法核心,不是外观皮肤。
// 登场顺序按"慢的大的先来,快的后来":大僵尸第 3 级,会跳的快僵尸第 5 级,
// 突袭僵尸第 7 级——先让人习惯"打不动但走得慢",再给要跟不上的速度。
typedef enum {
    ZG_Z_SMALL = 0,       // 小僵尸:标准速度,血薄,第 1 级就有
    ZG_Z_BIG,             // 大僵尸:血厚、最慢、占屏幕宽(第 3 级)
    ZG_Z_JUMPER,          // 跳跃僵尸:一段一段往下扑,腾空期间子弹打不到(第 5 级)
    ZG_Z_AMBUSH,          // 突然出现的僵尸:不在屏幕上方生成,直接在半空现身(第 7 级)
    ZG_Z_TYPE_COUNT
} zg_zombie_type_t;

typedef struct {
    const char *name;      // 日志与 host 测试里标识类型用
    uint8_t w, h;          // 碰撞盒,px
    uint8_t base_hp;       // 第 1 级血量;后续级别按 zg_zombie_hp() 叠加
    int16_t base_vy;       // 第 1 级下压速度,px/s(跳跃僵尸这里是腾空段速度)
    uint16_t score;        // 打死得分:一关要 1000 分,第 1 关正好是 10 只小僵尸
    uint8_t unlock_level;  // 从第几级开始会出现
} zg_zombie_info_t;

// 特效由引擎产生、UI 表现。ttl_ms 递减到 0 即槽位释放,所以 UI 掉帧也不会漏动画。
typedef enum {
    ZG_FX_MUZZLE = 0,     // 枪口火闪
    ZG_FX_HIT,            // 子弹命中
    ZG_FX_DEATH,          // 僵尸被打死
    ZG_FX_LEAK,           // 僵尸漏过底边
    ZG_FX_SPAWN,          // 僵尸现身前的预警
    ZG_FX_HURT,           // 豌豆射手挨了一下
    ZG_FX_GUNUP,          // 豌豆枪跟着关卡升档
    ZG_FX_PICKUP,         // 接到道具
    ZG_FX_CLONE,          // 卡片生效横幅:草坪上多了 3 株克隆射手(只出字,不画点)
    ZG_FX_ZCLONE,         // 一只分身落地时的那圈
    ZG_FX_ZLEARN,         // 反噬到账横幅:僵尸学会克隆了(只出字,不画点)
    ZG_FX_KIND_COUNT
} zg_fx_kind_t;

typedef struct {
    int32_t x, y;                // 1/16 px,左上角
    uint8_t w, h;
    int32_t vx, vy;              // px/s
    uint8_t hp;
    // 下面几个计时都用 uint16_t:蓄力 800ms、预警 450ms 都超出 uint8_t 的范围,
    // 用 uint8_t 存会被静默截断成几十毫秒,跳跃僵尸会变成每秒跳三次。
    uint16_t flash_ms;           // 命中高亮剩余时间
    uint16_t telegraph_ms;       // 现身预警剩余时间:此期间不伤人
    uint16_t hop_ms;             // 跳跃僵尸:本次腾空剩余时间
    uint16_t wait_ms;            // 跳跃僵尸:落地蓄力剩余时间
    bool airborne;               // 腾空:子弹直接穿过
    bool is_clone;               // 卡片反噬复制出来的分身:血与分都按母体减半
    bool cloned;                 // 已经产过分身,不再当母体(否则 20 秒里会滚成一片)
    bool alive;
    zg_zombie_type_t type;
} zg_zombie_t;

typedef struct {
    int32_t x, y;                // 1/16 px,左上角
    int32_t vx, vy;              // px/s
    uint8_t damage;
    uint8_t pierce_left;
    bool alive;
} zg_bullet_t;

typedef struct {
    int16_t x, y;                // px,特效中心
    uint16_t ttl_ms;
    uint16_t life_ms;            // 初始寿命,UI 用 ttl/life 算透明度
    zg_fx_kind_t kind;
    bool alive;
} zg_fx_t;

typedef struct {
    zg_phase_t phase;
    bool paused;                 // 冻结整局(含计时器),GAME OVER 时置不了
    uint32_t score;
    uint32_t best;
    bool best_dirty;             // 需要落盘的新纪录,由应用层消费后清零
    uint8_t hp;
    uint8_t lives;
    zg_weapon_t weapon;
    uint8_t level;
    uint16_t kills;
    uint32_t elapsed_ms;         // 本次游玩累计时间,只用于日志;关卡按分数推进

    int32_t player_x;            // 1/16 px,左上角
    int32_t player_y;
    uint16_t invuln_ms;          // 挨打后的短暂无敌
    uint16_t reload_ms;          // 下一次自动开火的剩余冷却
    uint16_t ready_ms;           // GET READY 剩余时间
    uint16_t levelup_ms;         // "LEVEL n" 横幅剩余时间,0 = 不在换关
    uint16_t spawn_ms;           // 下一只僵尸的生成倒计时
    uint16_t item_ms;            // 下一个道具的掉落倒计时
    uint16_t rapid_ms;           // 射速翻倍剩余时间,0 = 没吃到加速道具
    uint8_t bonus_shots;         // 道具累计的额外豌豆数,和档位发数合起来不超过 ZG_MAX_SHOTS
    // ---- 克隆卡片 ----
    bool clone_active;           // 这张卡片的一整段效果(己方克隆 + 僵尸反噬)还在走
    uint8_t clones;              // 场上有几个克隆豌豆射手
    uint16_t clone_reload_ms[ZG_POOL_CLONES];  // 各克隆的开火倒计时:错相位才不会齐射
    uint16_t learn_ms;           // 僵尸学会克隆还剩多久(接住卡片起 10 秒)
    uint16_t zclone_ms;          // 僵尸复制窗口剩余时间,0 = 当前不在复制
    uint16_t zclone_step_ms;     // 复制窗口内下一次放分身的时间
    int8_t move_dir;             // -1 左 / 0 停 / +1 右
    uint16_t hold_ms;            // 当前方向已连续按住多久,饱和在 ZG_SPRINT_MS
    bool key_left;
    bool key_right;
    uint32_t rng;

    zg_zombie_t zombies[ZG_POOL_ZOMBIES];
    zg_bullet_t bullets[ZG_POOL_BULLETS];
    zg_fx_t fx[ZG_POOL_FX];
    zg_item_t items[ZG_POOL_ITEMS];
} zg_game_t;

// ---- 参数表(测试与 UI 共用同一份数值,不在两边各抄一遍) ----
const zg_weapon_info_t *zg_weapon_info(zg_weapon_t weapon);
const zg_zombie_info_t *zg_zombie_info(zg_zombie_type_t type);
const zg_item_info_t *zg_item_info(zg_item_kind_t kind);

// 难度公式一律先把级别夹到 1..ZG_LEVEL_MAX。
int32_t zg_zombie_hp(zg_zombie_type_t type, uint8_t level);
int32_t zg_zombie_vy(zg_zombie_type_t type, uint8_t level);
uint16_t zg_spawn_interval_ms(uint8_t level);
// 同屏僵尸上限:第 1 关只给 2 只,后面逐关放开,到第 8 关起才是池子的 6 只。
uint8_t zg_max_alive(uint8_t level);
uint16_t zg_item_interval_ms(uint8_t level);
// 关卡按分数推进:第一关 0~999,第二关 1000~1999……第十关 9000~9999。
uint8_t zg_level_of_score(uint32_t score);

// 换关提示用:这一级第一次登场的是哪种僵尸,没有新种类时返回 NULL。
const zg_zombie_info_t *zg_unlock_at(uint8_t level);

// 草坪色阶分组:级别按它分成几档,UI 用它换草地底色,让"换关"不只有一行字。
// 返回 0..ZG_LAWN_GROUPS-1。
uint8_t zg_lawn_group(uint8_t level);

// 按关卡取武器档位。
zg_weapon_t zg_weapon_for_level(uint8_t level);
// 这一枪实际吐几颗豌豆:档位发数 + 道具加成,但不超过 ZG_MAX_SHOTS。
uint8_t zg_volley(const zg_game_t *game);
// 当前的实际开火间隔:吃到加速道具就是一半。
uint16_t zg_fire_interval_ms(const zg_game_t *game);

// ---- 生命周期 ----
void zg_init(zg_game_t *game, uint32_t seed, uint32_t best);
// GAME OVER 或通关后重开;其他状态下返回 false 且不改状态。
bool zg_restart(zg_game_t *game);
// 左右键按住状态。两个键同时按下按"停"处理:这块板的三键共用一路 ADC,
// 物理上本来就采不到同时按下,这里只是把状态定义清楚。
void zg_set_keys(zg_game_t *game, bool left, bool right);

// 暂停/继续。游玩和倒计时都能停;GAME OVER 与通关停不了(那已经不算一局)。
// 停住时 zg_step 一格都不推进,所以僵尸、子弹、倒计时全部原地不动。
void zg_set_paused(zg_game_t *game, bool paused);

// 推进一帧。dt_ms 由应用层的真实计时给出,引擎自己夹住范围,越界的补帧
// (调试串口停了一下、任务被抢占)不会让僵尸瞬移或子弹穿人。
void zg_step(zg_game_t *game, uint16_t dt_ms);

// ---- 查询 ----
bool zg_zombie_hits_player(const zg_game_t *game, const zg_zombie_t *zombie);
bool zg_bullet_hits_zombie(const zg_bullet_t *bullet, const zg_zombie_t *zombie);
bool zg_item_hits_player(const zg_game_t *game, const zg_item_t *item);
int zg_alive_count(const zg_game_t *game);
// 卡片反噬复制出来的分身有几只:非分身数量仍受 ZG_SPAWN_CAP 管,分身只吃池子余量。
int zg_clone_zombie_count(const zg_game_t *game);
// 第 idx 个克隆豌豆射手的左上角(px)。克隆种在底边固定三列,跟着场地走而不是跟人走,
// 所以想保住某一路就得自己站过去——UI 和引擎都从这一个函数取坐标。
int16_t zg_clone_x(uint8_t idx);
int16_t zg_clone_y(void);

// ---- 通关奖励 ----
// 打完 10 关必发奖章,等级只看还剩几条命:通关时一条没丢就是金,丢一条银,
// 剩下的都是铜。UI 用它决定奖章配色和"REWARD"那一行写什么。
typedef enum {
    ZG_MEDAL_NONE = 0,    // 还没通关:GAME OVER 不发奖章
    ZG_MEDAL_BRONZE,
    ZG_MEDAL_SILVER,
    ZG_MEDAL_GOLD,
    ZG_MEDAL_COUNT
} zg_medal_t;

zg_medal_t zg_medal_of(uint8_t lives);
const char *zg_medal_name(zg_medal_t medal);
