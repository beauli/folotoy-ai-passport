// tests/test_zombie_game.c
// 豌豆射手守草坪引擎的 host 测试:纯逻辑,不依赖 ESP-IDF/LVGL。
// 这里钉的是"手感数值 + 判定规则":几发打死一只、什么时候自动换枪、
// 漏怪和撞人各扣多少血、10 血 3 命怎么结账、跳跃/突袭僵尸的特权什么时候生效,
// 以及 40 关四个难度阶梯的难度曲线——开局必须弱,后面逐级变强、枪跟着一起变强,
// 阶梯起点血量/刷怪/抢卡概率一起跳档。另外钉住两件事:僵尸有概率抢吃掉下来的克隆
// 卡片,以及每跨过一关都要留下一份可落盘的进度(存档本身归应用层)。
// 这些一旦改错,设备上看起来只是"变难了一点",很难靠肉眼发现。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "zombie_game.h"

#define TICK_MS 40

// 精灵放大到 24 x 38 之后,射手占据底边 212~250 这一段。把"摆在枪口下挨打"的靶子
// 统一放在这条线以上,否则僵尸一出生就和射手重叠,测的变成撞人扣血而不是开枪。
#define RANGE_Y 170
#define RANGE_X 111   // 使 18px 宽的小僵尸正好横跨在 24px 宽的射手中线上

// 把生成器冻住:每次推进前把两个倒计时都拉满,单条规则测试就不会被随机刷出来的
// 僵尸或半路掉下来的道具干扰。
static void step_pure(zg_game_t *g, uint16_t dt)
{
    g->spawn_ms = 9999;
    g->item_ms = 9999;
    zg_step(g, dt);
    g->spawn_ms = 9999;
    g->item_ms = 9999;
}

// 摆一个道具,让人走着去接/站着让它漏掉。
static void place_item(zg_game_t *g, int slot, zg_item_kind_t kind, int x_px, int y_px)
{
    zg_item_t *it = &g->items[slot];
    memset(it, 0, sizeof(*it));
    it->x = ZG_PX_TO_POS(x_px);
    it->y = ZG_PX_TO_POS(y_px);
    it->vy = ZG_ITEM_VY;
    it->kind = kind;
    it->alive = true;
}

// 推进到 0 号槽位的道具消失(接到或落出底边),返回用了几个步进;超上限返回 -1。
static int step_item_gone(zg_game_t *g)
{
    for (int i = 0; i < 200; i++) {
        step_pure(g, TICK_MS);
        if (!g->items[0].alive) return i + 1;
    }
    return -1;
}

// 直接摆放一只僵尸,用来测单只僵尸的行为(引擎的状态就是普通数组,测试有权摆)。
static void place(zg_game_t *g, int slot, zg_zombie_type_t type, int x_px, int y_px,
                  int32_t vy, int32_t vx)
{
    const zg_zombie_info_t *info = zg_zombie_info(type);
    zg_zombie_t *z = &g->zombies[slot];
    memset(z, 0, sizeof(*z));
    z->x = ZG_PX_TO_POS(x_px);
    z->y = ZG_PX_TO_POS(y_px);
    z->w = info->w;
    z->h = info->h;
    z->vx = vx;
    z->vy = vy;
    z->hp = (uint8_t)zg_zombie_hp(type, g->level);
    z->type = type;
    z->alive = true;
}

// 跳过 GET READY,进入正常游玩。
static void to_play(zg_game_t *g)
{
    g->phase = ZG_PHASE_PLAY;
    g->ready_ms = 0;
    g->invuln_ms = 0;
    g->reload_ms = 0;
}

// 反复打死头顶的小僵尸,返回打死只数所需的推进次数;超上限返回 -1。
static int shoot_one_dead(zg_game_t *g, int slot)
{
    for (int i = 0; i < 400; i++) {
        step_pure(g, TICK_MS);
        if (!g->zombies[slot].alive) return i + 1;
    }
    return -1;
}

static void test_tables_are_consistent(void)
{
    // UI 直接打印武器名,空指针会把它打成 "(null)"。
    for (unsigned i = 0; i < ZG_WPN_COUNT; i++) {
        const zg_weapon_info_t *w = zg_weapon_info((zg_weapon_t)i);
        assert(w->name && w->name[0]);
        assert(w->interval_ms >= 100 && w->interval_ms <= 2000);
        assert(w->shots >= 1 && w->shots <= ZG_MAX_SHOTS);
        assert(w->damage >= 1);
        assert(w->bullet_speed >= 200 && w->bullet_speed <= 600);
        // 越往后弹速越快:开局那颗豌豆就是要慢慢吞吞,这是玩法而不是失误。
        if (i > 0) {
            assert(w->bullet_speed > zg_weapon_info((zg_weapon_t)(i - 1))->bullet_speed);
            // 任何一档都不该比开局那把更"卡",否则升级反而像降级。
            assert(w->interval_ms < zg_weapon_info(ZG_WPN_PEA)->interval_ms);
            // 档位按关卡排,所以起用关卡必须一关比一关晚。
            assert(w->first_level > zg_weapon_info((zg_weapon_t)(i - 1))->first_level);
        }
        // 磁轨枪是有意的例外:射速让位给穿透和 4 伤,不是数值漂移。
        if (i == ZG_WPN_RAILGUN) {
            assert(w->interval_ms > zg_weapon_info(ZG_WPN_RAPID)->interval_ms);
            assert(w->pierce >= 1 && w->damage >= 4);
        }
        // 多发必须是扇形,否则几颗豌豆全叠在同一条线上,算不上"多个方向"。
        if (w->shots > 1) assert(w->spread_px >= 4);
    }
    // 枪跟着关卡走:第一个阶梯 1/3/5/7/9 关各换一档,往后每个阶梯的起点再换一档。
    assert(zg_weapon_info(ZG_WPN_PEA)->first_level == 1);
    assert(zg_weapon_for_level(1) == ZG_WPN_PEA);
    assert(zg_weapon_for_level(2) == ZG_WPN_PEA);
    assert(zg_weapon_for_level(3) == ZG_WPN_SPLIT);
    assert(zg_weapon_for_level(4) == ZG_WPN_SPLIT);
    assert(zg_weapon_for_level(5) == ZG_WPN_THREE);
    assert(zg_weapon_for_level(6) == ZG_WPN_THREE);
    assert(zg_weapon_for_level(7) == ZG_WPN_RAPID);
    assert(zg_weapon_for_level(8) == ZG_WPN_RAPID);
    assert(zg_weapon_for_level(9) == ZG_WPN_RAILGUN);
    assert(zg_weapon_for_level(10) == ZG_WPN_RAILGUN);
    // 阶梯起点(11/21/31 关)各换一档:僵尸血量按 2/4/6 倍涨,枪不跟上就是打不动。
    assert(zg_weapon_for_level(11) == ZG_WPN_PLASMA);
    assert(zg_weapon_for_level(20) == ZG_WPN_PLASMA);
    assert(zg_weapon_for_level(21) == ZG_WPN_NOVA);
    assert(zg_weapon_for_level(30) == ZG_WPN_NOVA);
    assert(zg_weapon_for_level(31) == ZG_WPN_PULSAR);
    assert(zg_weapon_for_level(ZG_LEVEL_MAX) == ZG_WPN_PULSAR);
    // 越界传进来也不能放飞:第 0 关和第 99 关都得夹回首尾两档。
    assert(zg_weapon_for_level(0) == ZG_WPN_PEA);
    assert(zg_weapon_for_level(99) == ZG_WPN_PULSAR);
    // 往后的档位涨的是伤害与穿透,不是发数:4 发已经是弹池和读屏的顶。
    for (unsigned i = ZG_WPN_RAILGUN; i < ZG_WPN_COUNT; i++) {
        assert(zg_weapon_info((zg_weapon_t)i)->shots == ZG_MAX_SHOTS);
        assert(zg_weapon_info((zg_weapon_t)i)->damage >
               zg_weapon_info((zg_weapon_t)(i - 1))->damage);
        assert(zg_weapon_info((zg_weapon_t)i)->pierce >
               zg_weapon_info((zg_weapon_t)(i - 1))->pierce);
    }

    for (unsigned t = 0; t < ZG_Z_TYPE_COUNT; t++) {
        const zg_zombie_info_t *z = zg_zombie_info((zg_zombie_type_t)t);
        assert(z->name && z->name[0]);
        // 碰撞盒必须放得进场,否则生成时 x 区间会变成负数。
        assert(z->w >= 8 && z->w < ZG_FIELD_W);
        assert(z->h >= 8 && z->h < ZG_FIELD_H);
        assert(z->base_hp >= 2 && z->base_hp <= 12);   // "打两三下"下限,别退化成一发
        assert(z->base_vy > 0 && z->base_vy < 200);
        assert(z->unlock_level >= 1);
    }
    // 分值按"一关 1000 分"配:最便宜的一只也要值 100 分,否则一关要杀几十只,
    // 关卡推进就变成刷苦工而不是打得好。
    for (unsigned t = 0; t < ZG_Z_TYPE_COUNT; t++) {
        assert(zg_zombie_info((zg_zombie_type_t)t)->score >= 100);
        assert(zg_zombie_info((zg_zombie_type_t)t)->score <= ZG_LEVEL_SCORE_STEP / 2);
    }
    // 四种僵尸得按关卡逐个解锁,而且先是"大而慢"的,再是"快"的。
    // 阶梯:每 10 关一个倍数档,四个阶梯把 1/2/4/6 排满到第 40 关。
    assert(ZG_TIER_LEVELS == 10);
    assert(ZG_TIER_COUNT == 4);
    assert(ZG_LEVEL_MAX == ZG_TIER_LEVELS * ZG_TIER_COUNT);
    // 第 40 关不是终点:无尽模式没有通关线,关卡唯一的上限是防 uint8_t 回绕的那个数。
    assert(ZG_LEVEL_CEIL == 255);
    assert(ZG_LEVEL_CEIL > ZG_LEVEL_MAX);
    assert(zg_zombie_info(ZG_Z_SMALL)->unlock_level == 1);
    assert(zg_zombie_info(ZG_Z_BIG)->unlock_level == 3);
    assert(zg_zombie_info(ZG_Z_JUMPER)->unlock_level == 5);
    assert(zg_zombie_info(ZG_Z_AMBUSH)->unlock_level == 7);
    // 大僵尸比小僵尸慢:第一次出现"新的东西"是更耐打,而不是跑得更快。
    assert(zg_zombie_info(ZG_Z_BIG)->base_vy < zg_zombie_info(ZG_Z_SMALL)->base_vy);
    assert(zg_zombie_info(ZG_Z_JUMPER)->base_vy > zg_zombie_info(ZG_Z_SMALL)->base_vy);
    // 第 1 关就要弱:小僵尸 2 血、16 px/s 慢慢走、3.4 秒才来一只、场上最多 2 只。
    assert(zg_zombie_hp(ZG_Z_SMALL, 1) == 2);
    assert(zg_zombie_vy(ZG_Z_SMALL, 1) == 16);
    assert(zg_spawn_interval_ms(1) == 3400);
    assert(zg_max_alive(1) == 2);
    // "后面关卡慢慢变多":刷怪更密、同屏更多,而且两个方向都只能单调往前推。
    assert(zg_spawn_interval_ms(ZG_LEVEL_MAX) < zg_spawn_interval_ms(1));
    // 正常刷怪的上限吃 ZG_SPAWN_CAP,僵尸池尾部那几只名额是留给卡片反噬的分身的。
    // 如果哪天有人把两者合并,后期接卡片就再也塞不进分身了——缺点变成没有缺点。
    assert(ZG_POOL_ZOMBIES > ZG_SPAWN_CAP);
    assert(zg_max_alive(ZG_LEVEL_MAX) == ZG_SPAWN_CAP);
    for (uint8_t lv = 2; lv <= ZG_LEVEL_MAX; lv++) {
        assert(zg_spawn_interval_ms(lv) <= zg_spawn_interval_ms(lv - 1));
        assert(zg_max_alive(lv) >= zg_max_alive(lv - 1));
        assert(zg_max_alive(lv) <= ZG_SPAWN_CAP);
        assert(zg_item_interval_ms(lv) <= zg_item_interval_ms(lv - 1));
    }
    // 换关横幅要够长到能读完两行字,又不能长到吃掉大半关(一关最少也有几十秒)。
    assert(ZG_LEVELUP_MS >= 1200 && ZG_LEVELUP_MS <= 3000);
    // 道具:接得住的下落速度、说得出口的持续时间、以及"最多 4 颗"这个硬顶。
    assert(ZG_RAPID_MS == 20000);
    assert(ZG_RAPID_DIV == 2);   // 屏上写的是翻倍,不是 ×1.5
    assert(ZG_MAX_SHOTS == 4);
    assert(ZG_ITEM_VY > 20 && ZG_ITEM_VY < 120);
    assert(ZG_ITEM_SIZE >= 10 && ZG_ITEM_SIZE <= 20);
    for (unsigned k = 0; k < ZG_ITEM_COUNT; k++) {
        assert(zg_item_info((zg_item_kind_t)k)->name[0]);
        // 两种道具必须一眼分得开:配色撞了就只能靠运气认出接到的是哪个。
        for (unsigned j = 0; j < k; j++) {
            assert(zg_item_info((zg_item_kind_t)k)->color != zg_item_info((zg_item_kind_t)j)->color);
        }
    }
    // 精灵必须比初版大:240x250 的屏上 16x26 的射手几乎是一粒米。这几条下限就是
    // "人物要看得见"这条需求的凭据,哪天缩回去这里先红,而不是等玩家抱怨。
    assert(ZG_PLAYER_W >= 24 && ZG_PLAYER_H >= 38);
    assert(zg_zombie_info(ZG_Z_SMALL)->w >= 18 && zg_zombie_info(ZG_Z_SMALL)->h >= 21);
    assert(zg_zombie_info(ZG_Z_BIG)->w >= 30 && zg_zombie_info(ZG_Z_BIG)->h >= 30);
    assert(ZG_BULLET_W >= 4 && ZG_BULLET_H >= 10);
    assert(ZG_ITEM_SIZE >= 18);
    // 放大只该动尺寸,不该顺手把怪变硬:小僵尸第 1 关仍然是两下、16 px/s。
    assert(zg_zombie_hp(ZG_Z_SMALL, 1) == 2);
    assert(zg_zombie_vy(ZG_Z_SMALL, 1) == 16);

    // 克隆卡片:三株射手站得进场,反噬严格按"10 秒后学会、只持续 20 秒"到账。
    assert(ZG_POOL_CLONES == 3);
    assert(ZG_ZOMBIE_LEARN_MS == 10000);
    assert(ZG_ZOMBIE_CLONE_MS == 20000);
    assert(ZG_ZOMBIE_CLONE_STEP_MS > ZG_DT_MAX_MS);
    assert(ZG_CLONE_W >= 16 && ZG_CLONE_H >= 24 && ZG_CLONE_H < ZG_PLAYER_H);
    assert(ZG_CLONE_DAMAGE == 1);                              // 克隆不该比本体打得疼
    // 单个克隆不该比本体的枪还勤:卡片是"多几株帮手",不是把主武器换掉。
    assert(ZG_CLONE_FIRE_MS > zg_weapon_info(ZG_WPN_PEA)->interval_ms);
    assert(ZG_ZCLONE_HP_DIV >= 2 && ZG_ZCLONE_SCORE_DIV >= 2); // 分身必须是弱化版
    assert(zg_clone_x(0) >= 0);
    assert(zg_clone_x(ZG_POOL_CLONES - 1) + ZG_CLONE_W <= ZG_FIELD_W);  // 最右一株不出界
    assert(zg_clone_y() + ZG_CLONE_H == ZG_FIELD_H);           // 和本体一样贴着底边
    for (unsigned i = 1; i < ZG_POOL_CLONES; i++) {
        // 三株各自一列,而且不互相压:叠在一起就等于只有一株在打。
        assert(zg_clone_x(i) >= zg_clone_x(i - 1) + ZG_CLONE_W);
    }
    // 越界下标也不能给出场地外的坐标:UI 是按 0..ZG_POOL_CLONES-1 画的,写错要能兜住。
    assert(zg_clone_x(ZG_POOL_CLONES) <= ZG_FIELD_W - ZG_CLONE_W);

    // 屏上按键卡写的是 "2X SPEED",所以倍率必须是 2,改了倍率就要改文案。
    assert(ZG_SPRINT_MUL == 2);
    assert(ZG_SPRINT_MS > ZG_DT_MAX_MS);   // 门限至少比一帧长,否则第一步就快了
}

static void test_init_defaults(void)
{
    zg_game_t g;
    zg_init(&g, 12345, 777);
    assert(g.phase == ZG_PHASE_READY);
    assert(g.score == 0);
    assert(g.best == 777);              // 开机恢复的历史纪录不能被抹掉
    assert(!g.best_dirty);
    assert(g.hp == ZG_HP_MAX && ZG_HP_MAX == 10);
    assert(g.lives == ZG_LIVES_MAX && ZG_LIVES_MAX == 3);
    assert(g.weapon == ZG_WPN_PEA);
    assert(g.level == 1);
    assert(g.kills == 0);
    assert(g.move_dir == 0);
    assert(g.ready_ms > 0);
    // 出生点:屏幕底边正中,左右都能走到。
    assert(g.player_y == ZG_PX_TO_POS(ZG_FIELD_H - ZG_PLAYER_H));
    assert(g.player_x == ZG_PX_TO_POS((ZG_FIELD_W - ZG_PLAYER_W) / 2));
    for (unsigned i = 0; i < ZG_POOL_ZOMBIES; i++) assert(!g.zombies[i].alive);
    for (unsigned i = 0; i < ZG_POOL_BULLETS; i++) assert(!g.bullets[i].alive);
    for (unsigned i = 0; i < ZG_POOL_FX; i++) assert(!g.fx[i].alive);
    for (unsigned i = 0; i < ZG_POOL_ITEMS; i++) assert(!g.items[i].alive);
    // 道具相关的初始状态:第一件 8 秒后才掉,开局既没有加速也没有加成。
    assert(g.item_ms == ZG_ITEM_FIRST_MS);
    assert(g.rapid_ms == 0);
    assert(g.bonus_shots == 0);
    // 卡片相关的初始状态:没吃到卡片就不该有克隆射手,也不该有反噬倒计时。
    assert(!g.clone_active && g.clones == 0);
    assert(g.learn_ms == 0 && g.zclone_ms == 0 && g.zclone_step_ms == 0);
    for (unsigned i = 0; i < ZG_POOL_CLONES; i++) assert(g.clone_reload_ms[i] == 0);
    assert(zg_volley(&g) == zg_weapon_info(ZG_WPN_PEA)->shots);
    assert(zg_fire_interval_ms(&g) == zg_weapon_info(ZG_WPN_PEA)->interval_ms);

    // 种子 0 会让 xorshift 永远输出 0,僵尸全挤在同一列,所以要换成非零常量。
    zg_init(&g, 0, 0);
    assert(g.rng != 0);
}

static void test_movement_and_bounds(void)
{
    zg_game_t g;
    zg_init(&g, 1, 0);
    const int32_t right_limit = ZG_PX_TO_POS(ZG_FIELD_W - ZG_PLAYER_W);

    zg_set_keys(&g, false, true);      // 按住右
    assert(g.move_dir == 1);
    for (int i = 0; i < 200; i++) step_pure(&g, TICK_MS);
    assert(g.player_x == right_limit);   // 走到最右边就钉住,不会掉出屏幕

    zg_set_keys(&g, true, false);      // 按住左
    assert(g.move_dir == -1);
    for (int i = 0; i < 400; i++) step_pure(&g, TICK_MS);
    assert(g.player_x == 0);

    // 同按(这块板三键共用一路 ADC,物理上采不到)按"停"处理,位置不动。
    zg_set_keys(&g, true, true);
    const int32_t before = g.player_x;
    step_pure(&g, TICK_MS);
    assert(g.player_x == before);

    zg_set_keys(&g, false, false);
    assert(g.move_dir == 0);

    // 移动速度:150 px/s,40ms 一步正好 6 px。
    zg_set_keys(&g, false, true);
    step_pure(&g, TICK_MS);
    assert(g.player_x == ZG_PX_TO_POS(6));

    // GET READY 里就能走位,不然开局被闷在原地两秒。
    zg_init(&g, 2, 0);
    assert(g.phase == ZG_PHASE_READY);
    zg_set_keys(&g, false, true);
    step_pure(&g, TICK_MS);
    assert(g.player_x > 0);
}

// 只有左右两个移动键,所以"按住多久"就是第二个档位:慢档用来贴位,快档用来赶路。
static void test_holding_a_direction_doubles_speed(void)
{
    zg_game_t g;
    zg_init(&g, 17, 0);
    to_play(&g);
    g.reload_ms = 9999;                      // 本例只看走位,别让开火掺进来

    const int32_t x0 = g.player_x;
    zg_set_keys(&g, false, true);
    // 慢档 150px/s:40ms 一步 6px。攒满 0.5 秒要 13 步,前 12 步仍在慢档。
    for (int i = 0; i < 12; i++) step_pure(&g, TICK_MS);
    assert(g.hold_ms == 480);
    assert(ZG_POS_TO_PX(g.player_x - x0) == 72);

    step_pure(&g, TICK_MS);                  // 第 13 步跨过门限,计时饱和在 500
    assert(g.hold_ms == ZG_SPRINT_MS);
    const int32_t x1 = g.player_x;
    for (int i = 0; i < 2; i++) step_pure(&g, TICK_MS);   // 只走两步,别撞墙
    assert(ZG_POS_TO_PX(g.player_x - x1) == 2 * 12);      // 快档 300px/s,一步 12px

    // 一直按着不会掉回慢档,也不会把计时器加溢出。
    for (int i = 0; i < 2000; i++) step_pure(&g, TICK_MS);
    assert(g.hold_ms == ZG_SPRINT_MS);
    assert(g.player_x == ZG_PX_TO_POS(ZG_FIELD_W - ZG_PLAYER_W));

    // 松手一步就回到慢档:重新按下必须重新攒够 0.5 秒才加速。
    zg_set_keys(&g, false, false);
    step_pure(&g, TICK_MS);
    assert(g.hold_ms == 0);
    zg_set_keys(&g, true, false);                      // 反向重按:计时仍然从零起
    assert(g.hold_ms == 0 && g.move_dir == -1);
    step_pure(&g, TICK_MS);
    assert(ZG_POS_TO_PX(g.player_x) == ZG_FIELD_W - ZG_PLAYER_W - 6);   // 慢档 6px,不是 12

    // 一路往回蹭够久再换方向:换向同样清零,"往回蹭一下"不白拿双倍速度。
    for (int i = 0; i < 20; i++) step_pure(&g, TICK_MS);
    assert(g.hold_ms == ZG_SPRINT_MS);
    zg_set_keys(&g, false, true);
    assert(g.hold_ms == 0);
}

static void test_pause_freezes_the_whole_round(void)
{
    zg_game_t g;
    zg_init(&g, 18, 0);
    to_play(&g);
    // 一发还在半路的豌豆 + 一只正在下压的僵尸:摆在不同列,暂停前不会撞上。
    place(&g, 0, ZG_Z_SMALL, 200, 100, 40, 0);
    step_pure(&g, TICK_MS);
    const zg_bullet_t shot = g.bullets[0];
    assert(shot.alive);
    const int32_t zy = g.zombies[0].y;
    const uint32_t elapsed = g.elapsed_ms;
    const int32_t x = g.player_x;

    zg_set_paused(&g, true);
    assert(g.paused);
    zg_set_keys(&g, false, true);
    for (int i = 0; i < 50; i++) zg_step(&g, TICK_MS);
    assert(g.bullets[0].y == shot.y);            // 子弹不动
    assert(g.zombies[0].y == zy);                // 僵尸也不动,判定一起冻住
    assert(g.elapsed_ms == elapsed);             // 级别时间也不走,暂停不能用来"等升级"
    assert(g.player_x == x);                     // 按住左右也不算走位
    assert(g.spawn_ms > 0);                      // 生成倒计时一起冻住

    zg_set_paused(&g, false);
    step_pure(&g, TICK_MS);
    assert(g.bullets[0].y < shot.y);             // 解冻后照旧
    assert(g.zombies[0].y > zy);

    // GAME OVER 已经不算"正在一局":那里按暂停无效,重开一定会把它清掉。
    g.phase = ZG_PHASE_OVER;
    zg_set_paused(&g, true);
    assert(!g.paused);
    g.paused = true;                             // 假设暂停被带进了结束画面
    assert(zg_restart(&g));
    assert(!g.paused && g.phase == ZG_PHASE_READY);
}

static void test_dt_is_clamped(void)
{
    zg_game_t g;
    zg_init(&g, 3, 0);
    to_play(&g);
    // 太小的一帧直接忽略,不做"补个 1ms 结果把整帧位移摊薄"的事。
    const int32_t x = g.player_x;
    zg_set_keys(&g, false, true);
    zg_step(&g, (uint16_t)(ZG_DT_MIN_MS - 1));
    assert(g.player_x == x && g.elapsed_ms == 0);

    // 超大的一帧被夹到上限:串口打印卡了 2 秒也不该让僵尸瞬移穿人。
    zg_step(&g, 9000);
    assert(g.elapsed_ms == ZG_DT_MAX_MS);
    // 位移按夹过的 dt 算:150px/s * 60ms = 9px,而不是 2 秒的 300px。
    assert(g.player_x == x + ZG_PX_TO_POS(9));
    assert(g.level == 1);
}

static void test_gun_fires_itself(void)
{
    zg_game_t g;
    zg_init(&g, 4, 0);
    to_play(&g);

    // 第一帧就该出膛:玩家不需要按射击键。
    step_pure(&g, TICK_MS);
    int fired = 0;
    for (unsigned i = 0; i < ZG_POOL_BULLETS; i++) {
        if (g.bullets[i].alive) fired++;
    }
    assert(fired == 1);
    assert(g.fx[0].alive && g.fx[0].kind == ZG_FX_MUZZLE);

    // 子弹朝屏幕上方飞。
    const int32_t y = g.bullets[0].y;
    step_pure(&g, TICK_MS);
    assert(g.bullets[0].alive && g.bullets[0].y < y);

    // 飞出屏幕就回收,槽位不会越来越满(子弹尾还压在顶边上时允许仍然活着)。
    for (int i = 0; i < 120; i++) step_pure(&g, TICK_MS);
    for (unsigned i = 0; i < ZG_POOL_BULLETS; i++) {
        if (!g.bullets[i].alive) continue;
        assert(g.bullets[i].y + ZG_PX_TO_POS(ZG_BULLET_H) >= 0);
    }

    // 步枪 560ms 一发:40ms 内不会出现两发同刻出膛。
    zg_init(&g, 5, 0);
    to_play(&g);
    for (int i = 0; i < 2; i++) {
        step_pure(&g, TICK_MS);
        assert(g.bullets[0].alive || g.bullets[1].alive);
    }
    int live = 0;
    for (unsigned i = 0; i < ZG_POOL_BULLETS; i++) {
        if (g.bullets[i].alive) live++;
    }
    assert(live == 1);   // 第二发还在冷却里
}

static void test_small_zombie_dies_in_two_rifle_shots(void)
{
    zg_game_t g;
    zg_init(&g, 6, 0);
    to_play(&g);
    // 正对枪口、悬在头顶不动的一只小僵尸:血量就是"打两下"的 2。
    place(&g, 0, ZG_Z_SMALL, RANGE_X, RANGE_Y, 0, 0);
    assert(g.zombies[0].hp == 2);

    int hits = 0;
    uint16_t last_flash = 0;
    for (int i = 0; i < 400 && g.zombies[0].alive; i++) {
        step_pure(&g, TICK_MS);
        if (g.zombies[0].flash_ms > last_flash) hits++;
        last_flash = g.zombies[0].flash_ms;
    }
    assert(hits == 2);                       // 少一发或多一发都是数值漂移
    assert(!g.zombies[0].alive);
    assert(g.kills == 1);
    assert(g.score == zg_zombie_info(ZG_Z_SMALL)->score);
    assert(g.weapon == ZG_WPN_PEA);         // 10 分还不到换枪门槛
    assert(g.best == g.score);                    // HI 立刻跟着涨,屏幕上要看得见
    assert(!g.best_dirty);                        // 但不写盘:无尽局每十几秒就破一次纪录
    // 打死要有死亡特效,不然玩家不知道掉的是什么。
    bool death_fx = false;
    for (unsigned i = 0; i < ZG_POOL_FX; i++) {
        if (g.fx[i].kind == ZG_FX_DEATH) death_fx = true;
    }
    assert(death_fx);
}

static void test_scoring_advances_levels_and_guns(void)
{
    zg_game_t g;
    zg_init(&g, 7, 0);
    to_play(&g);
    zg_set_keys(&g, false, false);

    const struct {
        uint32_t score;
        uint8_t level;
        zg_weapon_t weapon;
    } want[] = {
        // 关卡只认分数:第一关 1000 分、第二关 2000 分……枪跟着关卡一起换档。
        { 2u * ZG_LEVEL_SCORE_STEP, 3, ZG_WPN_SPLIT },
        { 4u * ZG_LEVEL_SCORE_STEP, 5, ZG_WPN_THREE },
        { 6u * ZG_LEVEL_SCORE_STEP, 7, ZG_WPN_RAPID },
        { 8u * ZG_LEVEL_SCORE_STEP, 9, ZG_WPN_RAILGUN },
    };
    // 一只一只往枪口下摆小僵尸,打死到够分数为止,换关和换枪都必须自己发生。
    for (unsigned k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
        int guard = 0;
        // 小僵尸 100 分一只,所以一关是 10 只;到第 9 关累计已经打了 80 多只。
        while (g.score < want[k].score && guard++ < 200) {
            place(&g, 0, ZG_Z_SMALL, RANGE_X, RANGE_Y, 0, 0);
            assert(shoot_one_dead(&g, 0) >= 0);
        }
        assert(g.score >= want[k].score);
        assert(g.level == want[k].level);
        assert(g.weapon == want[k].weapon);
        assert(zg_level_of_score(g.score) == want[k].level);
    }
    // 已经拿到的枪不会因为丢血、漏怪而掉档。
    const uint8_t hp_before = g.hp;
    assert(hp_before == ZG_HP_MAX);
    place(&g, 1, ZG_Z_SMALL, 200, 240, 300, 0);
    for (int i = 0; i < 40; i++) step_pure(&g, TICK_MS);
    assert(g.weapon == ZG_WPN_RAILGUN);
    assert(g.hp < ZG_HP_MAX);            // 这只确实漏了,掉血但没掉枪
}

static void test_leak_through_the_line_costs_hp(void)
{
    zg_game_t g;
    zg_init(&g, 8, 0);
    to_play(&g);
    zg_set_keys(&g, true, false);          // 人在最左边,枪口打不到右边那只
    for (int i = 0; i < 40; i++) step_pure(&g, TICK_MS);
    assert(g.player_x == 0);

    place(&g, 0, ZG_Z_SMALL, 200, 230, 300, 0);
    const uint8_t hp_before = g.hp;
    int steps = 0;
    while (g.zombies[0].alive && steps++ < 60) step_pure(&g, TICK_MS);
    assert(!g.zombies[0].alive);
    assert(g.hp == (uint8_t)(hp_before - 1));   // 漏一只扣 1 点,不多扣
    assert(g.lives == ZG_LIVES_MAX);
    assert(g.kills == 0);                        // 漏掉的不算打死
    assert(g.score == 0);
    bool leak_fx = false;
    for (unsigned i = 0; i < ZG_POOL_FX; i++) {
        if (g.fx[i].kind == ZG_FX_LEAK) leak_fx = true;
    }
    assert(leak_fx);

    // 无敌期内漏怪只清怪,不重复扣血。
    place(&g, 1, ZG_Z_SMALL, 200, 230, 300, 0);
    const uint8_t hp_invuln = g.hp;
    while (g.zombies[1].alive) step_pure(&g, TICK_MS);
    assert(g.hp == hp_invuln);
}

static void test_contact_costs_hp_once_then_invuln(void)
{
    zg_game_t g;
    zg_init(&g, 9, 0);
    to_play(&g);
    zg_set_keys(&g, false, false);
    const int32_t px = g.player_x;

    place(&g, 0, ZG_Z_SMALL, ZG_POS_TO_PX(px), ZG_FIELD_H - ZG_PLAYER_H, 0, 0);
    assert(zg_zombie_hits_player(&g, &g.zombies[0]));
    step_pure(&g, TICK_MS);
    assert(g.hp == ZG_HP_MAX - 1);
    assert(!g.zombies[0].alive);          // 撞上的僵尸一定消失,不然会贴脸反复判定
    assert(g.invuln_ms > 0);
    assert(g.kills == 0);                 // 撞人不是打死,不给分

    // 无敌期内再撞只清怪。
    place(&g, 1, ZG_Z_BIG, ZG_POS_TO_PX(px), ZG_FIELD_H - ZG_PLAYER_H, 0, 0);
    step_pure(&g, TICK_MS);
    assert(g.hp == ZG_HP_MAX - 1);
    assert(!g.zombies[1].alive);

    // 无敌过期后恢复正常扣血。
    g.invuln_ms = 1;
    step_pure(&g, TICK_MS);
    assert(g.invuln_ms == 0);
    place(&g, 2, ZG_Z_SMALL, ZG_POS_TO_PX(px), ZG_FIELD_H - ZG_PLAYER_H, 0, 0);
    step_pure(&g, TICK_MS);
    assert(g.hp == ZG_HP_MAX - 2);
}

// 一直撞到掉命,返回丢命次数。
static int drive_hits(zg_game_t *g, int count)
{
    int lives_lost = 0;
    for (int i = 0; i < count; i++) {
        g->invuln_ms = 0;                 // 只测结算,不等无敌
        g->phase = ZG_PHASE_PLAY;
        g->ready_ms = 0;
        place(g, 0, ZG_Z_SMALL, ZG_POS_TO_PX(g->player_x), ZG_FIELD_H - ZG_PLAYER_H, 0, 0);
        const uint8_t lives = g->lives;
        step_pure(g, TICK_MS);
        if (g->lives < lives) lives_lost++;
    }
    return lives_lost;
}

static void test_ten_hp_three_lives_then_game_over(void)
{
    zg_game_t g;
    zg_init(&g, 10, 0);
    g.score = 500;                         // 攒点分,结束时要进纪录
    to_play(&g);

    assert(drive_hits(&g, ZG_HP_MAX - 1) == 0);
    assert(g.hp == 1 && g.lives == ZG_LIVES_MAX);

    // 第 10 下:丢 1 命,血量回满,清场,回到 GET READY,并给复活无敌。
    drive_hits(&g, 1);
    assert(g.lives == ZG_LIVES_MAX - 1);
    assert(g.hp == ZG_HP_MAX);
    assert(g.phase == ZG_PHASE_READY);
    assert(g.ready_ms > 0);
    assert(g.invuln_ms > 0);
    assert(g.score == 500);                // 丢命不扣已经拿到的分
    for (unsigned i = 0; i < ZG_POOL_ZOMBIES; i++) assert(!g.zombies[i].alive);

    // 复活后倒计时走完自动继续,不需要额外按键。
    for (int i = 0; i < 100; i++) step_pure(&g, TICK_MS);
    assert(g.phase == ZG_PHASE_PLAY);

    assert(drive_hits(&g, ZG_HP_MAX) == 1);   // 第二条命也走完
    assert(g.lives == 1);
    drive_hits(&g, ZG_HP_MAX);                // 第三条命走完 → 结束
    assert(g.lives == 0);
    assert(g.phase == ZG_PHASE_OVER);
    assert(g.hp == 0);
    assert(g.best == 500 && g.best_dirty);   // 结束时要落盘
    // 结束后再推帧不产生任何新判定。
    zg_step(&g, TICK_MS);
    assert(g.phase == ZG_PHASE_OVER && g.hp == 0);

    // 不在 GAME OVER 时,重开键不该把正在进行的一局抹掉。
    zg_game_t idle;
    zg_init(&idle, 11, 42);
    assert(!zg_restart(&idle));            // READY 倒计时里
    to_play(&idle);
    idle.score = 90;
    assert(!zg_restart(&idle));
    assert(idle.phase == ZG_PHASE_PLAY && idle.score == 90 && idle.best == 42);

    // GAME OVER 之后:左右键重开,分数归零、血与命回满、纪录保留。
    assert(zg_restart(&g));
    assert(g.phase == ZG_PHASE_READY && g.score == 0 && g.hp == ZG_HP_MAX);
    assert(g.lives == ZG_LIVES_MAX && g.level == 1 && g.kills == 0);
    assert(g.weapon == ZG_WPN_PEA);
    assert(!g.clone_active && g.clones == 0 && g.learn_ms == 0 && g.zclone_ms == 0);
    assert(g.best == 500);                 // 纪录不随重开丢失
    assert(g.best_dirty);                  // 还没写盘的纪录不能因为重开而丢
    for (unsigned i = 0; i < ZG_POOL_ZOMBIES; i++) assert(!g.zombies[i].alive);
}

static void test_jumper_is_only_shootable_on_the_ground(void)
{
    zg_game_t g;
    zg_init(&g, 12, 0);
    to_play(&g);

    zg_zombie_t z = {0};
    const zg_zombie_info_t *info = zg_zombie_info(ZG_Z_JUMPER);
    z = (zg_zombie_t){ .x = ZG_PX_TO_POS(100), .y = ZG_PX_TO_POS(100),
                       .w = info->w, .h = info->h, .hp = 4, .type = ZG_Z_JUMPER,
                       .alive = true, .airborne = true };
    zg_bullet_t b = { .x = ZG_PX_TO_POS(104), .y = ZG_PX_TO_POS(100),
                      .vx = 0, .vy = -zg_weapon_info(ZG_WPN_PEA)->bullet_speed,
                      .damage = 1, .alive = true };
    assert(zg_bullet_hits_zombie(&b, &z) == false);   // 腾空:子弹穿过
    z.airborne = false;
    assert(zg_bullet_hits_zombie(&b, &z) == true);    // 落地:能打

    // 跳跃僵尸会自己一段一段往下扑,而且腾空/落地交替出现。
    // 取 4.8 秒窗口:它平均每分钟只走 100 多 px,这段时间还碰不到底边,
    // 判定只考察运动方式,不掺杂掉血结算。
    place(&g, 0, ZG_Z_JUMPER, 100, 40, zg_zombie_vy(ZG_Z_JUMPER, 1), 0);
    g.zombies[0].hp = 200;                 // 不想让它死,只想看它的运动
    g.reload_ms = 9999;                    // 也别让子弹掺进来
    int hops = 0;
    int32_t last_y = g.zombies[0].y;
    bool was_air = false;
    for (int i = 0; i < 120; i++) {
        step_pure(&g, TICK_MS);
        assert(g.zombies[0].alive);
        if (g.zombies[0].airborne && !was_air) hops++;
        was_air = g.zombies[0].airborne;
        assert(g.zombies[0].y >= last_y);  // 只往下走,不会倒着飘回屏幕上方
        last_y = g.zombies[0].y;
    }
    assert(hops >= 2);                      // 4.8 秒里至少跳了两轮
    assert(ZG_POS_TO_PX(last_y) > 40 && ZG_POS_TO_PX(last_y) < 200);   // 在逼近,但还没到底
}

static void test_ambush_telegraphs_then_falls(void)
{
    zg_game_t g;
    zg_init(&g, 13, 0);
    to_play(&g);
    g.reload_ms = 9999;                    // 冻住枪,本例只考察突袭僵尸自己
    zg_set_keys(&g, false, false);

    // 突袭僵尸出现在半空而不是屏幕上方:预警期间原地不动,站在它身下也不掉血。
    place(&g, 0, ZG_Z_AMBUSH, 60, 120, 200, 0);
    g.zombies[0].telegraph_ms = ZG_TELEGRAPH_MS;
    const uint8_t hp_before = g.hp;
    const int32_t y_before = g.zombies[0].y;
    for (int i = 0; i < 5; i++) step_pure(&g, TICK_MS);
    assert(g.hp == hp_before);                     // 预警期不伤人
    assert(g.zombies[0].alive);
    assert(g.zombies[0].y == y_before);            // 预警期原地不动
    assert(g.zombies[0].telegraph_ms < ZG_TELEGRAPH_MS);

    // 预警走完就真的扑下来:先把玩家走到它正下方,落地后一定扣血。
    zg_set_keys(&g, true, false);
    for (int i = 0; i < 40 && g.zombies[0].telegraph_ms > 0; i++) step_pure(&g, TICK_MS);
    assert(g.zombies[0].telegraph_ms == 0);
    assert(g.zombies[0].y == y_before);            // 预警没走完就一格都不动
    assert(ZG_POS_TO_PX(g.player_x) <= 72);        // 已经站到它下面(60..74 与玩家重叠)
    for (int i = 0; i < 40 && g.hp == hp_before; i++) step_pure(&g, TICK_MS);
    assert(g.hp == (uint8_t)(hp_before - 1));       // 落地扑到人才第一次扣血
    assert(!g.zombies[0].alive);                    // 撞上的僵尸一定消失
}

static void test_difficulty_ramps_and_caps(void)
{
    // 关卡只认分数:第一关 0~999,第二关从 1000 起,第十关从 9000 起。
    assert(zg_level_of_score(0) == 1);
    assert(zg_level_of_score(ZG_LEVEL_SCORE_STEP - 1) == 1);
    assert(zg_level_of_score(ZG_LEVEL_SCORE_STEP) == 2);
    assert(zg_level_of_score((uint32_t)ZG_LEVEL_SCORE_STEP * ZG_LEVEL_MAX - 1u) == ZG_LEVEL_MAX);
    // 无尽段:40000 分是第 41 关的起点,不是终点线。
    assert(zg_level_of_score((uint32_t)ZG_LEVEL_SCORE_STEP * ZG_LEVEL_MAX) == ZG_LEVEL_MAX + 1);
    // 分数再高也只是关卡号,夹在 uint8_t 顶上是防它回绕成"第 1 关"。
    assert(zg_level_of_score(1000000u) == ZG_LEVEL_CEIL);
    for (uint8_t lv = 1; lv < ZG_LEVEL_MAX; lv++) {
        assert(zg_zombie_hp(ZG_Z_SMALL, (uint8_t)(lv + 1)) >= zg_zombie_hp(ZG_Z_SMALL, lv));
        assert(zg_zombie_vy(ZG_Z_SMALL, (uint8_t)(lv + 1)) > zg_zombie_vy(ZG_Z_SMALL, lv));
        assert(zg_zombie_vy(ZG_Z_BIG, (uint8_t)(lv + 1)) > zg_zombie_vy(ZG_Z_BIG, lv));
        assert(zg_spawn_interval_ms((uint8_t)(lv + 1)) <= zg_spawn_interval_ms(lv));
    }
    // 无尽段(第 41 关往上)还得继续变难,但只有血继续涨:速度早就顶在硬顶上,
    // 刷怪间隔也 floor 在 0.5 秒,不然"无尽"会退化成"一屏看不清"。
    for (uint8_t lv = ZG_LEVEL_MAX; lv < ZG_LEVEL_CEIL; lv++) {
        assert(zg_tier_of(lv) == ZG_TIER_COUNT - 1);       // 倍数停在最后一档
        assert(zg_zombie_hp(ZG_Z_SMALL, (uint8_t)(lv + 1)) >= zg_zombie_hp(ZG_Z_SMALL, lv));
        assert(zg_zombie_vy(ZG_Z_SMALL, (uint8_t)(lv + 1)) >= zg_zombie_vy(ZG_Z_SMALL, lv));
        assert(zg_zombie_vy(ZG_Z_SMALL, (uint8_t)(lv + 1)) <= ZG_ZOMBIE_VY_MAX);
        assert(zg_spawn_interval_ms(lv) >= ZG_SPAWN_MIN_MS);
    }
    assert(zg_zombie_hp(ZG_Z_SMALL, 60) > zg_zombie_hp(ZG_Z_SMALL, ZG_LEVEL_MAX));
    // 血最终夹在 uint8_t 之内:第 255 关小僵尸本来 312 血,不夹住会截成 56,
    // 那就成了"越往后越好打",和无尽的意思正好相反。
    assert(zg_zombie_hp(ZG_Z_SMALL, ZG_LEVEL_CEIL) == 255);
    assert(zg_zombie_hp(ZG_Z_BIG, ZG_LEVEL_CEIL) == 255);
    // 封顶:关卡越界也不涨,刷怪最密是硬底,再密就读不出人了。
    assert(zg_zombie_hp(ZG_Z_SMALL, ZG_LEVEL_MAX) > zg_zombie_hp(ZG_Z_SMALL, 1));
    assert(zg_spawn_interval_ms(99) >= ZG_SPAWN_MIN_MS);  // 最密 0.5 秒一只
    assert(zg_spawn_interval_ms(99) <= zg_spawn_interval_ms(1));
    assert(zg_max_alive(99) == ZG_SPAWN_CAP);
    assert(zg_item_interval_ms(99) >= ZG_ITEM_MIN_MS);

    zg_game_t g;
    zg_init(&g, 14, 0);
    to_play(&g);
    // 站着不动、一只都没打死:时间在走,但关卡不能自己往前推——现在是分数说话。
    for (int i = 0; i < 650; i++) step_pure(&g, TICK_MS);   // 26 秒
    assert(g.level == 1);
    assert(g.score == 0);
    assert(g.elapsed_ms == 26000u);
    assert(g.hp == ZG_HP_MAX);   // 场上没怪,推进本身不该带来掉血

    // 打死 9 只小僵尸还在第一关,第 10 只跨线:一关 1000 分、小僵尸 100 分。
    for (int i = 0; i < 9; i++) {
        place(&g, 0, ZG_Z_SMALL, RANGE_X, RANGE_Y, 0, 0);
        assert(shoot_one_dead(&g, 0) >= 0);
    }
    assert(g.score == 9u * ZG_LEVEL_SCORE_STEP / 10u && g.level == 1);
    place(&g, 0, ZG_Z_SMALL, RANGE_X, RANGE_Y, 0, 0);
    assert(shoot_one_dead(&g, 0) >= 0);
    assert(g.score >= ZG_LEVEL_SCORE_STEP && g.level == 2);
    assert(g.weapon == zg_weapon_for_level(2));
}

// 打通到第 level 关需要打死几只怪、按这一关的刷怪节奏大概要等多久:
// 只算"每只都打死最值钱的那种"是乐观下限,全按最便宜的小僵尸算是悲观上限。
// 关卡现在按分数推,所以整局时长必须由这两条夹住——哪一关要刷的数量失控,
// 或者整局拖到天荒地老,都是数值漂移,不是"这局打得不好"。
static void level_pace(uint32_t *seconds_min, uint32_t *seconds_max, uint32_t *kills_max)
{
    uint32_t lo = 0, hi = 0;
    uint32_t worst_kills = 0;
    for (uint8_t lv = 1; lv <= ZG_LEVEL_MAX; lv++) {
        uint32_t best = 0;
        for (unsigned t = 0; t < ZG_Z_TYPE_COUNT; t++) {
            const zg_zombie_info_t *z = zg_zombie_info((zg_zombie_type_t)t);
            if (z->unlock_level > lv) continue;
            if (z->score > best) best = z->score;
        }
        // 每关都要拿满 1000 分,最便宜的一只也要算进来(upper bound 用分值最低的类型)。
        const uint32_t kills_lo = (ZG_LEVEL_SCORE_STEP + best - 1) / best;
        const uint32_t kills_hi = (ZG_LEVEL_SCORE_STEP + zg_zombie_info(ZG_Z_SMALL)->score - 1) /
                                  zg_zombie_info(ZG_Z_SMALL)->score;
        if (kills_hi > worst_kills) worst_kills = kills_hi;
        lo += kills_lo * zg_spawn_interval_ms(lv) / 1000u;
        hi += kills_hi * zg_spawn_interval_ms(lv) / 1000u;
    }
    *seconds_min = lo;
    *seconds_max = hi;
    *kills_max = worst_kills;
}

// 打到某一级时"手上应该是哪把枪":枪跟着关卡走,所以这张表就是引擎那条规则的另一份写法。
// 两边对不上时,zg_weapon_for_level 会直接红,不用等玩家抱怨"怎么还不换枪"。
static const zg_weapon_t GUN_BY_LEVEL[ZG_LEVEL_MAX + 1] = {
    ZG_WPN_PEA,                                                      // 占位,下标从 1 起
    ZG_WPN_PEA, ZG_WPN_PEA, ZG_WPN_SPLIT, ZG_WPN_SPLIT, ZG_WPN_THREE,
    ZG_WPN_THREE, ZG_WPN_RAPID, ZG_WPN_RAPID, ZG_WPN_RAILGUN, ZG_WPN_RAILGUN,
    // 第二~第四阶梯:起点各换一档,后面九关沿用同一把。
    ZG_WPN_PLASMA, ZG_WPN_PLASMA, ZG_WPN_PLASMA, ZG_WPN_PLASMA, ZG_WPN_PLASMA,
    ZG_WPN_PLASMA, ZG_WPN_PLASMA, ZG_WPN_PLASMA, ZG_WPN_PLASMA, ZG_WPN_PLASMA,
    ZG_WPN_NOVA, ZG_WPN_NOVA, ZG_WPN_NOVA, ZG_WPN_NOVA, ZG_WPN_NOVA,
    ZG_WPN_NOVA, ZG_WPN_NOVA, ZG_WPN_NOVA, ZG_WPN_NOVA, ZG_WPN_NOVA,
    ZG_WPN_PULSAR, ZG_WPN_PULSAR, ZG_WPN_PULSAR, ZG_WPN_PULSAR, ZG_WPN_PULSAR,
    ZG_WPN_PULSAR, ZG_WPN_PULSAR, ZG_WPN_PULSAR, ZG_WPN_PULSAR, ZG_WPN_PULSAR,
};

// 每型僵尸允许的发数上限:小僵尸两发就倒,跳跃/突袭多给一发,大僵尸本来就是一堵墙。
// 关卡往上加而枪不跟着变强,这里就会红。
static const uint8_t HIT_BUDGET[ZG_Z_TYPE_COUNT] = { 2, 4, 3, 3 };   // SMALL BIG JUMPER AMBUSH

static void test_gun_scales_with_level(void)
{
    assert((size_t)(ZG_LEVEL_MAX + 1) == sizeof(GUN_BY_LEVEL) / sizeof(GUN_BY_LEVEL[0]));
    uint32_t pace_lo = 0, pace_hi = 0, kills = 0;
    level_pace(&pace_lo, &pace_hi, &kills);
    // 一关最多 10 只(第 1 关就是 10 只小僵尸),再多就是刷苦工而不是打法。
    assert(kills <= 10);
    // 整局通关的合理区间:打得顺利三分钟,磨到底七八分钟。四个阶梯把上限抬到原来的
    // 四倍多,正是"一会儿就结束"那条抱怨的回应;再往外就是关卡分数或刷怪节奏飘了。
    assert(pace_lo >= 150 && pace_lo <= 260);
    assert(pace_hi >= pace_lo && pace_hi <= 480);

    for (uint8_t lv = 1; lv <= ZG_LEVEL_MAX; lv++) {
        const zg_weapon_t want = GUN_BY_LEVEL[lv];
        // 这一关该拿到的枪,和引擎按关卡取枪的结果必须一字不差。
        assert(zg_weapon_for_level(lv) == want);
        const int32_t dmg = zg_weapon_info(want)->damage;
        assert(dmg >= 1);
        for (unsigned t = 0; t < ZG_Z_TYPE_COUNT; t++) {
            const zg_zombie_info_t *z = zg_zombie_info((zg_zombie_type_t)t);
            if (z->unlock_level > lv) continue;   // 这一级还不该见到它
            assert(zg_zombie_hp((zg_zombie_type_t)t, lv) <= (int32_t)HIT_BUDGET[t] * dmg);
        }
        if (lv > 1) assert((int)want >= (int)GUN_BY_LEVEL[lv - 1]);
    }
    assert(GUN_BY_LEVEL[1] == ZG_WPN_PEA);
    assert(GUN_BY_LEVEL[ZG_LEVEL_MAX] == ZG_WPN_PULSAR);
    // 第 1 关就是单发慢豌豆:开局那一关是留给练走位的,不该一上手就三发。
    assert(zg_weapon_info(ZG_WPN_PEA)->shots == 1);
    assert(zg_weapon_info(ZG_WPN_PEA)->first_level == 1);
}

// 换关必须看得见:横幅亮起来、这段时间不刷怪也不掉道具、走完自己落下,并且每一级
// 都有明确的"新僵尸/变强/封顶"提示词可用。
static void test_level_change_is_announced(void)
{
    zg_game_t g;
    zg_init(&g, 21, 0);
    to_play(&g);
    assert(g.level == 1 && g.levelup_ms == 0);

    // 把分数推到刚好跨过一级,只用一步换关;两个倒计时都设成正值期内到期,
    // 用来验证横幅期间既不刷怪也不掉道具。
    g.score = ZG_LEVEL_SCORE_STEP;
    g.spawn_ms = 20;
    g.item_ms = 20;
    zg_step(&g, TICK_MS);
    assert(g.level == 2);
    assert(g.levelup_ms == ZG_LEVELUP_MS);
    assert(zg_alive_count(&g) == 0);   // 倒计时已经归零,但横幅期间不登场
    assert(!g.items[0].alive && !g.items[1].alive);

    for (int i = 1; i < ZG_LEVELUP_MS / TICK_MS; i++) {
        zg_step(&g, TICK_MS);
        assert(g.levelup_ms == ZG_LEVELUP_MS - (uint16_t)(i * TICK_MS));
        assert(zg_alive_count(&g) == 0);
        assert(!g.items[0].alive && !g.items[1].alive);
    }
    // 横幅落下的那一帧,这一级的第一只僵尸才出现。
    zg_step(&g, TICK_MS);
    assert(g.levelup_ms == 0);
    assert(zg_alive_count(&g) == 1);

    // 跨过 40 关不是终点:第 41 关照样换关、照样亮横幅,倍数停在 ×6,局继续往下走。
    for (unsigned i = 0; i < ZG_POOL_ZOMBIES; i++) g.zombies[i].alive = false;
    g.reload_ms = 9999;   // 别让自动开火在这几帧里改动分数
    g.score = (uint32_t)ZG_LEVEL_MAX * ZG_LEVEL_SCORE_STEP;   // 40000 → 第 41 关
    g.level = ZG_LEVEL_MAX;
    g.levelup_ms = 0;
    g.progress_dirty = false;
    zg_step(&g, TICK_MS);
    assert(g.phase == ZG_PHASE_PLAY);              // 已经没有"打完"这一档了
    assert(g.level == ZG_LEVEL_MAX + 1 && g.levelup_ms == ZG_LEVELUP_MS);
    assert(zg_tier_mul(g.level) == 6);             // 无尽段停在最后一个阶梯
    assert(g.progress_dirty);                      // 换关照样招呼一次落盘

    // 新种类提示只在真的有新怪的那几级出现,UI 靠它决定第二行写什么。
    assert(zg_unlock_at(zg_zombie_info(ZG_Z_JUMPER)->unlock_level) ==
           zg_zombie_info(ZG_Z_JUMPER));
    assert(zg_unlock_at(zg_zombie_info(ZG_Z_BIG)->unlock_level) ==
           zg_zombie_info(ZG_Z_BIG));
    assert(zg_unlock_at(zg_zombie_info(ZG_Z_AMBUSH)->unlock_level) ==
           zg_zombie_info(ZG_Z_AMBUSH));
    assert(zg_unlock_at(2) == NULL);
    assert(zg_unlock_at(4) == NULL);
    assert(zg_unlock_at(6) == NULL);
    assert(zg_unlock_at(ZG_LEVEL_MAX) == NULL);   // 新怪只到第 7 关,后面全是"变强"
    assert(zg_unlock_at(ZG_LEVEL_MAX + 1) == NULL);

    // 草坪色阶:一个难度阶梯一档色,换档必须落在真的换关的那一级上,而且全程不越界。
    assert(zg_lawn_group(1) == 0 && zg_lawn_group(10) == 0);
    assert(zg_lawn_group(11) == 1);
    assert(zg_lawn_group(21) == 2);
    assert(zg_lawn_group(31) == 3);
    assert(zg_lawn_group(ZG_LEVEL_MAX) == ZG_LAWN_GROUPS - 1);
    // 无尽段不再换草皮:最后一档色一直用下去,UI 才不会跑到第 41 关就无组可取。
    assert(zg_lawn_group(ZG_LEVEL_MAX + 1) == ZG_LAWN_GROUPS - 1);
    for (uint8_t lv = 1; lv <= ZG_LEVEL_MAX; lv++) assert(zg_lawn_group(lv) < ZG_LAWN_GROUPS);
}

// 无尽:没有通关线。分数一路涨就是关卡一路涨,一局只由"三条命打空"结束。
// 这条测试钉住的是玩法结论——原来打完第 40 关就发奖章收工,整局被一条分数线判死。
static void test_endless_has_no_finish_line(void)
{
    zg_game_t g;
    zg_init(&g, 31, 0);
    to_play(&g);
    g.reload_ms = 9999;

    // 原来那条通关线的前一档:场上有怪、时间在走,但没有任何东西把它判成"结束"。
    g.score = (uint32_t)ZG_LEVEL_SCORE_STEP * ZG_LEVEL_MAX - 50u;
    g.level = zg_level_of_score(g.score);
    assert(g.level == ZG_LEVEL_MAX);
    place(&g, 0, ZG_Z_SMALL, 100, 60, 0, 0);   // 站住的怪:用来确认"场上还是照常"
    assert(zg_alive_count(&g) == 1);
    for (int i = 0; i < 100; i++) step_pure(&g, TICK_MS);
    assert(g.phase == ZG_PHASE_PLAY && g.level == ZG_LEVEL_MAX);
    assert(zg_alive_count(&g) == 1);   // 跨过 40 关不该把场上的怪抹掉

    // 跨过 40000 分:只是换一关,时间继续走,纪录跟着涨但不立刻招呼写盘。
    const uint32_t elapsed_before = g.elapsed_ms;
    g.progress_dirty = false;
    g.score = (uint32_t)ZG_LEVEL_SCORE_STEP * ZG_LEVEL_MAX;
    zg_step(&g, TICK_MS);
    assert(g.phase == ZG_PHASE_PLAY);
    assert(g.level == ZG_LEVEL_MAX + 1 && g.levelup_ms == ZG_LEVELUP_MS);
    assert(g.elapsed_ms == elapsed_before + TICK_MS);
    assert(g.best == 0 && !g.best_dirty);         // 这一帧没打死东西,就没有新纪录要写
    assert(g.progress_dirty);                     // 换关只招呼"最远关卡"那一半
    assert(zg_tier_mul(g.level) == 6);            // 无尽段还落在最后一个阶梯里

    // 进行中的局随时能暂停,左右键也不是重开键:只有打死才会离开 PLAY。
    zg_set_paused(&g, true);
    assert(g.paused);
    zg_set_paused(&g, false);
    assert(!g.paused);
    assert(!zg_restart(&g));
    assert(g.level == ZG_LEVEL_MAX + 1 && g.score == (uint32_t)ZG_LEVEL_SCORE_STEP * ZG_LEVEL_MAX);

    // 关卡号一路往上数,没有哪一档会停下来:再跨两关还是换关,不是结束。
    g.score = (uint32_t)ZG_LEVEL_SCORE_STEP * (ZG_LEVEL_MAX + 2u);
    zg_step(&g, TICK_MS);
    assert(g.phase == ZG_PHASE_PLAY && g.level == ZG_LEVEL_MAX + 3);
}

static void test_railgun_pierces_a_column(void)
{
    zg_game_t g;
    zg_init(&g, 15, 0);
    to_play(&g);
    // 枪跟着关卡走:要磁轨枪就得打到第 9 关,分数和级别必须一起摆正。
    g.score = 8u * ZG_LEVEL_SCORE_STEP;                              // 8000 → 第 9 关
    g.level = zg_level_of_score(g.score);
    g.weapon = zg_weapon_for_level(g.level);
    assert(g.level == 9 && g.weapon == ZG_WPN_RAILGUN);

    // 同一列上前后两只:磁轨枪一发穿透两只。
    place(&g, 0, ZG_Z_SMALL, RANGE_X, RANGE_Y, 0, 0);
    place(&g, 1, ZG_Z_SMALL, RANGE_X, RANGE_Y + 14, 0, 0);
    g.zombies[0].hp = 3;
    g.zombies[1].hp = 3;
    for (int i = 0; i < 200 && (g.zombies[0].alive || g.zombies[1].alive); i++) {
        step_pure(&g, TICK_MS);
    }
    assert(!g.zombies[0].alive && !g.zombies[1].alive);
    assert(g.kills == 2);
    assert(g.score >= 8u * ZG_LEVEL_SCORE_STEP +
                       2u * zg_zombie_info(ZG_Z_SMALL)->score);
}

// 道具是"靠走位拿的奖励":接住才生效,漏了不罚,加速有硬时限,豌豆数有硬顶。
// 这几条只要有一条写错,设备上就表现为"吃了道具没反应"或"吃一次就永久变强"。
static void test_items_are_caught_by_walking(void)
{
    zg_game_t g;
    zg_init(&g, 41, 0);
    to_play(&g);
    const uint16_t base_interval = zg_fire_interval_ms(&g);
    assert(base_interval == zg_weapon_info(g.weapon)->interval_ms);   // 没吃道具时不该打折

    // 接到加速:开火间隔当场减半,而且从接住那一刻起算 20 秒。
    place_item(&g, 0, ZG_ITEM_RAPID, ZG_POS_TO_PX(g.player_x), 60);
    assert(step_item_gone(&g) > 0);
    assert(g.rapid_ms == ZG_RAPID_MS);
    assert(zg_fire_interval_ms(&g) == base_interval / ZG_RAPID_DIV);

    // 20 秒一到自己退回原间隔,不需要谁去清它。
    for (int i = 0; i < ZG_RAPID_MS / TICK_MS - 1; i++) step_pure(&g, TICK_MS);
    assert(g.rapid_ms > 0 && zg_fire_interval_ms(&g) < base_interval);
    step_pure(&g, TICK_MS);
    assert(g.rapid_ms == 0 && zg_fire_interval_ms(&g) == base_interval);
    // 重复吃到只是重新计时,不会叠成 40 秒。
    place_item(&g, 0, ZG_ITEM_RAPID, ZG_POS_TO_PX(g.player_x), 60);
    assert(step_item_gone(&g) > 0);
    assert(g.rapid_ms == ZG_RAPID_MS);

    // 接到"多一颗豌豆":一次吐多发,道具加成和档位发数合起来不超过 4 颗。
    for (int n = 0; n < ZG_MAX_SHOTS + 2; n++) {
        place_item(&g, 0, ZG_ITEM_PEA, ZG_POS_TO_PX(g.player_x), 60);
        assert(step_item_gone(&g) > 0);
    }
    assert(zg_volley(&g) == ZG_MAX_SHOTS);
    // 加成是加在档位上的:第 1 关单发枪最多加 3 颗,再多接也不突破硬顶。
    assert(g.bonus_shots == ZG_MAX_SHOTS - zg_weapon_info(g.weapon)->shots);
    assert(g.bonus_shots < ZG_MAX_SHOTS);

    // 漏掉的道具只是消失:不扣分、不加血、什么都不改——奖励不该变成负担。
    const uint8_t hp_before = g.hp, bonus_before = g.bonus_shots;
    place_item(&g, 0, ZG_ITEM_PEA, 0, 60);   // 人站在中间,这颗落在最左边
    assert(step_item_gone(&g) > 0);
    assert(g.hp == hp_before && g.bonus_shots == bonus_before);
    assert(zg_volley(&g) == ZG_MAX_SHOTS);
    assert(!g.items[0].alive);
}

// 本体的枪也一起按住:9999ms 只够 250 帧,长窗口测试里要每帧续上,
// 否则测着测着本体自己把母体打死了,断言就变成在测运气。
static void step_frozen(zg_game_t *g)
{
    g->reload_ms = 9999;
    step_pure(g, TICK_MS);
    g->reload_ms = 9999;
}

// 克隆卡片的前半段:接住当场种下三株射手,各自从自己的槽位吐豌豆;
// 再吃一张只是补满位子和重排反噬,不会叠成六株。
static void test_clone_card_plants_three_shooters(void)
{
    zg_game_t g;
    zg_init(&g, 51, 0);
    to_play(&g);
    zg_set_keys(&g, false, false);

    place_item(&g, 0, ZG_ITEM_CLONE, ZG_POS_TO_PX(g.player_x), 60);
    assert(step_item_gone(&g) > 0);
    assert(g.clones == ZG_POOL_CLONES && g.clone_active);
    // 接到卡片那一帧,反噬是从满格起步的(同一帧的 step_card 已经跟着走了一格)。
    assert(g.learn_ms == ZG_ZOMBIE_LEARN_MS - TICK_MS);
    assert(g.zclone_ms == 0);
    // 接到卡片要有一条说得清的横幅,不然玩家只看见草坪上凭空多了三株植物。
    bool clone_fx = false;
    for (unsigned i = 0; i < ZG_POOL_FX; i++) {
        if (g.fx[i].kind == ZG_FX_CLONE) clone_fx = true;
    }
    assert(clone_fx);

    // 三株各打各的:弹道起点必须落在引擎那三个槽位的中线上,伤害比本体低一档。
    g.reload_ms = 9999;                       // 关掉本体,免得分不清哪颗豌豆是谁吐的
    for (unsigned i = 0; i < ZG_POOL_CLONES; i++) {
        g.clone_reload_ms[i] = 1;             // 挨个催出膛
        step_pure(&g, TICK_MS);
        const int16_t want_x = (int16_t)(zg_clone_x((uint8_t)i) + ZG_CLONE_W / 2 - ZG_BULLET_W / 2);
        int found = -1;
        for (unsigned j = 0; j < ZG_POOL_BULLETS; j++) {
            if (g.bullets[j].alive && ZG_POS_TO_PX(g.bullets[j].x) == want_x) found = (int)j;
        }
        assert(found >= 0);
        assert(g.bullets[found].vy == -ZG_CLONE_SPEED);
        assert(g.bullets[found].damage == ZG_CLONE_DAMAGE);
        assert(g.bullets[found].vx == 0);      // 克隆只直上直下,扇形是本体的事
        assert(g.bullets[found].pierce_left == 0);
    }

    // 同屏三株就够:再吃一张只把三个位子补满、反噬重新起算。
    for (int i = 0; i < 25; i++) step_frozen(&g);
    assert(g.learn_ms < ZG_ZOMBIE_LEARN_MS);
    place_item(&g, 0, ZG_ITEM_CLONE, ZG_POS_TO_PX(g.player_x), 60);
    assert(step_item_gone(&g) > 0);
    assert(g.clones == ZG_POOL_CLONES);        // 不是 6
    assert(g.learn_ms == ZG_ZOMBIE_LEARN_MS - TICK_MS);  // 第二张把反噬往后推,不叠成两段

    // 丢一条命把整段效果一起收掉:克隆射手没了,还没到账的反噬也不许再来。
    g.phase = ZG_PHASE_PLAY;
    g.hp = 1;
    g.invuln_ms = 0;
    place(&g, 5, ZG_Z_SMALL, ZG_POS_TO_PX(g.player_x), ZG_FIELD_H - ZG_PLAYER_H, 0, 0);
    step_pure(&g, TICK_MS);
    assert(g.lives == ZG_LIVES_MAX - 1);
    assert(g.clones == 0 && !g.clone_active);
    assert(g.learn_ms == 0 && g.zclone_ms == 0 && g.zclone_step_ms == 0);
    for (unsigned i = 0; i < ZG_POOL_CLONES; i++) assert(g.clone_reload_ms[i] == 0);

    // 射手死了就不该再有克隆的豌豆冒出来:三格开火倒计时已清,推进多久都不会再吐。
    g.phase = ZG_PHASE_PLAY;
    for (int i = 0; i < ZG_CLONE_FIRE_MS / TICK_MS + 10; i++) {
        step_frozen(&g);
        for (unsigned j = 0; j < ZG_POOL_BULLETS; j++) {
            if (!g.bullets[j].alive) continue;
            bool from_clone = false;
            for (unsigned k = 0; k < ZG_POOL_CLONES; k++) {
                if (ZG_POS_TO_PX(g.bullets[j].x) ==
                    zg_clone_x((uint8_t)k) + ZG_CLONE_W / 2 - ZG_BULLET_W / 2) from_clone = true;
            }
            assert(!from_clone);
        }
    }
}

// 克隆卡片的后半段,也就是这张卡片的"缺点":接住 10 秒之后僵尸才开始复制,
// 复制只持续 20 秒,每只母体一辈子只生一次,分身血和分都只有母体的一半。
static void test_zombie_backlash_is_late_and_limited(void)
{
    zg_game_t g;
    zg_init(&g, 52, 0);
    to_play(&g);
    zg_set_keys(&g, false, false);
    place_item(&g, 0, ZG_ITEM_CLONE, ZG_POS_TO_PX(g.player_x), 60);
    assert(step_item_gone(&g) > 0);
    g.reload_ms = 9999;

    // 母体放在三株克隆的弹道之外(16/116/216 三条竖线),这样场上谁在挨打只由测试决定。
    place(&g, 0, ZG_Z_SMALL, 60, 60, 0, 0);
    g.zombies[0].hp = 6;

    // 从"卡片到账"到"僵尸学会"之间的帧数必须正好是 10 秒。少一秒是白送,多一秒
    // 是把代价提前;接住卡片那一帧 step_card 已经跟着走掉一格,所以起点少一帧。
    int frames_to_learn = 0;
    while (g.zclone_ms == 0 && frames_to_learn < 1000) {
        step_frozen(&g);
        assert(!g.zombies[0].cloned);         // 10 秒没到之前一只分身都不许出
        assert(zg_clone_zombie_count(&g) == 0);
        frames_to_learn++;
    }
    assert(frames_to_learn == ZG_ZOMBIE_LEARN_MS / TICK_MS - 1);
    assert(g.learn_ms == 0);
    assert(g.zclone_ms == ZG_ZOMBIE_CLONE_MS);
    assert(zg_clone_zombie_count(&g) == 0);
    // 学会的那一刻要有警告横幅:偷偷开始复制等于没告诉玩家这张卡片有代价。
    bool learn_fx = false;
    for (unsigned i = 0; i < ZG_POOL_FX; i++) {
        if (g.fx[i].kind == ZG_FX_ZLEARN) learn_fx = true;
    }
    assert(learn_fx);

    step_frozen(&g);                          // 下一帧就放第一只,不多等一个 3 秒步长
    assert(g.zombies[0].cloned);              // 母体用掉了它这一辈子唯一的一次复制
    assert(zg_clone_zombie_count(&g) == 1);

    const zg_zombie_t *dup = &g.zombies[1];
    assert(dup->alive && dup->is_clone && dup->cloned);
    assert(dup->type == g.zombies[0].type);
    assert(dup->w == g.zombies[0].w && dup->h == g.zombies[0].h);
    assert(dup->vy == g.zombies[0].vy);
    assert(dup->hp == g.zombies[0].hp / ZG_ZCLONE_HP_DIV);   // 血按母体"剩余"的一半算
    // 分身是错开一只身位出来的,不是和母体重叠成同一只。
    assert(ZG_POS_TO_PX(dup->x) != ZG_POS_TO_PX(g.zombies[0].x));
    assert(dup->x >= 0 && dup->x <= ZG_PX_TO_POS(ZG_FIELD_W - dup->w));

    // 复制窗口里每隔 ZG_ZOMBIE_CLONE_STEP_MS 试一次,但只有那一只母体可以生:
    // 20 秒下来分身数必须一直钉在 1,不然这局就没法收尾了。
    int window_frames = 1;                    // 上面那一帧就是窗口的第一帧
    while (g.zclone_ms > 0 && window_frames < 1000) {
        step_frozen(&g);
        assert(zg_clone_zombie_count(&g) == 1);
        assert(g.zombies[1].alive);
        window_frames++;
    }
    assert(window_frames == ZG_ZOMBIE_CLONE_MS / TICK_MS);   // 20 秒整,一秒不多
    assert(g.zclone_ms == 0);
    step_frozen(&g);
    assert(!g.clone_active);                  // 空窗走完的那一帧把这一段账结清
    assert(g.clones == ZG_POOL_CLONES);       // 反噬到期,自己这边的克隆射手还在
    assert(g.zombies[1].alive);               // 已经生出来的分身不会随窗口一起消失

    // 分身分值减半:把克隆静音、只留本体的枪,免得说不清这一发是谁打的。
    g.clones = 0;
    g.reload_ms = 0;
    for (unsigned i = 0; i < ZG_POOL_ZOMBIES; i++) {
        if (i != 1) g.zombies[i].alive = false;
    }
    g.player_x = ZG_PX_TO_POS((ZG_FIELD_W - ZG_PLAYER_W) / 2);
    g.zombies[1].x = ZG_PX_TO_POS(RANGE_X);
    g.zombies[1].y = ZG_PX_TO_POS(RANGE_Y);
    g.zombies[1].hp = 1;
    const uint32_t score_before = g.score;
    assert(shoot_one_dead(&g, 1) >= 0);
    assert(g.score == score_before +
               zg_zombie_info(ZG_Z_SMALL)->score / ZG_ZCLONE_SCORE_DIV);
    assert(g.kills == 1);                     // 分身也算打死一只,只是不值那么多分
}

// 摆一张往下飘的克隆卡片,正下方一只站住的母体,看这一局是被抢(1)还是没抢中(0)。
// 抢卡只摇一次骰子,所以想要哪一种结果就换种子去挑:这里钉的是"摇完之后的规则",
// 不是运气本身。返回 -1 表示这一局没走到判定(不该发生)。
static int card_grab_try(uint32_t seed, uint8_t level)
{
    zg_game_t g;
    zg_init(&g, seed, 0);
    to_play(&g);
    g.level = level;
    g.score = (uint32_t)(level - 1u) * ZG_LEVEL_SCORE_STEP;
    g.reload_ms = 9999;                       // 本体别开枪,免得母体先被打死
    place(&g, 0, ZG_Z_SMALL, 100, 60, 0, 0);
    place_item(&g, 0, ZG_ITEM_CLONE, 100, 40);
    for (int i = 0; i < 10; i++) {
        g.reload_ms = 9999;
        step_pure(&g, TICK_MS);
        if (g.zombies[0].carded) return 1;
        if (g.items[0].dodged) return 0;      // 摇过一次没中:卡片继续往下飘
    }
    return -1;
}

// 僵尸也能吃掉下来的克隆卡片:抢中当场多一只分身,并把场上僵尸的复制窗口一起打开,
// 而玩家一份好处也拿不到;没抢中的那张照常飘下来,走过去仍然接得到。
static void test_zombie_steals_a_falling_card(void)
{
    // 先认"这是概率":同样 100 个种子,既不能全抢到也不能一个都抢不到。
    int hits_lo = 0, hits_hi = 0;
    uint32_t hit_seed = 0, miss_seed = 0;
    for (uint32_t s = 1; s <= 100; s++) {
        const int r = card_grab_try(s, 1);
        assert(r >= 0);
        if (r == 1) { hits_lo++; if (!hit_seed) hit_seed = s; }
        else if (!miss_seed) miss_seed = s;
        if (card_grab_try(s, ZG_LEVEL_MAX) == 1) hits_hi++;
    }
    assert(hits_lo >= 5 && hits_lo <= 60);
    assert(hit_seed && miss_seed);
    // 抢卡概率跟着难度阶梯往上给:最后一个阶梯(六成)必须明显比第一阶梯(两成)抢得多。
    assert(hits_hi > hits_lo);

    // 抢中的那一局:卡片没了、母体标上"抢过卡"、当场多一只弱化分身、窗口按短的那档打开。
    zg_game_t g;
    zg_init(&g, hit_seed, 0);
    to_play(&g);
    g.reload_ms = 9999;
    place(&g, 0, ZG_Z_SMALL, 100, 60, 0, 0);
    const uint8_t mother_hp = g.zombies[0].hp;
    place_item(&g, 0, ZG_ITEM_CLONE, 100, 40);
    for (int i = 0; i < 10 && !g.zombies[0].carded; i++) {
        g.reload_ms = 9999;
        step_pure(&g, TICK_MS);
    }
    assert(g.zombies[0].carded);
    assert(!g.items[0].alive);                // 被吃掉的卡片不再是玩家的
    assert(g.clones == 0 && !g.clone_active);  // 好处归僵尸,玩家这边一株克隆都没有
    assert(g.learn_ms == 0);                   // 也没有"10 秒后学会"那笔账要记
    assert(zg_clone_zombie_count(&g) == 1);
    assert(g.zombies[1].is_clone && g.zombies[1].alive);
    assert(g.zombies[1].hp == mother_hp / ZG_ZCLONE_HP_DIV);   // 分身是弱化版
    // 窗口时长用僵尸那一张的(ZG_ZEAT_CLONE_MS),比玩家那张 20 秒短:抢到不等于全盘失控。
    // 同一帧的窗口计时已经走掉一格,所以这里按"刚到账"的那一小段区间认。
    assert(g.zclone_ms <= ZG_ZEAT_CLONE_MS && g.zclone_ms > ZG_ZEAT_CLONE_MS - 3 * TICK_MS);
    assert(g.zclone_ms < ZG_ZOMBIE_CLONE_MS);
    bool zeat_fx = false;
    for (unsigned i = 0; i < ZG_POOL_FX; i++) {
        if (g.fx[i].kind == ZG_FX_ZEAT) zeat_fx = true;
    }
    assert(zeat_fx);                           // UI 靠这条写出"卡片被抢了"

    // 母体生过就不再生,窗口关掉之后也不再补:整局只多这一只,不会滚成一片。
    for (int i = 0; i < ZG_ZEAT_CLONE_MS / TICK_MS + 5; i++) {
        g.reload_ms = 9999;
        step_pure(&g, TICK_MS);
    }
    assert(g.zclone_ms == 0);
    assert(zg_clone_zombie_count(&g) == 1);

    // 没抢中的那张:判定只摇一次,之后它继续往下飘,站着不动就能接住。
    zg_init(&g, miss_seed, 0);
    to_play(&g);
    g.reload_ms = 9999;
    place(&g, 0, ZG_Z_SMALL, 100, 60, 0, 0);
    place_item(&g, 0, ZG_ITEM_CLONE, 100, 40);
    for (int i = 0; i < 10 && !g.items[0].dodged; i++) {
        g.reload_ms = 9999;
        step_pure(&g, TICK_MS);
    }
    assert(g.items[0].dodged && g.items[0].alive);
    assert(!g.zombies[0].carded && g.zclone_ms == 0 && zg_clone_zombie_count(&g) == 0);
    for (int i = 0; i < 200 && g.items[0].alive; i++) {
        g.reload_ms = 9999;
        step_pure(&g, TICK_MS);
    }
    assert(!g.items[0].alive);
    assert(g.clones == ZG_POOL_CLONES && g.clone_active);   // 接到照旧种下三株
}

// 第 10 关不是天花板:每 ZG_TIER_LEVELS 关一个难度阶梯,血量/刷怪/抢卡按 1/2/4/6 跳档,
// 速度单独用一张更缓的表并夹在硬顶之下,枪在阶梯起点跟着换。
static void test_difficulty_tiers_stack_up(void)
{
    assert(ZG_TIER_LEVELS == 10 && ZG_TIER_COUNT == 4);
    assert(ZG_LEVEL_MAX == ZG_TIER_LEVELS * ZG_TIER_COUNT);

    // 阶梯归属:第 1/11/21/31 关各是一个阶梯的起点,第 40 关仍在最后一档里。
    assert(zg_tier_of(1) == 0 && zg_tier_of(10) == 0);
    assert(zg_tier_of(11) == 1 && zg_tier_of(20) == 1);
    assert(zg_tier_of(21) == 2 && zg_tier_of(30) == 2);
    assert(zg_tier_of(31) == 3 && zg_tier_of(ZG_LEVEL_MAX) == ZG_TIER_COUNT - 1);
    // 第 0 关是越界下沿,夹到第一档;99 关则是无尽段的一关,落在最后一档。
    assert(zg_tier_of(0) == 0 && zg_tier_of(99) == ZG_TIER_COUNT - 1);
    assert(zg_tier_mul(1) == 1 && zg_tier_mul(11) == 2);
    assert(zg_tier_mul(21) == 4 && zg_tier_mul(31) == 6);   // UI 直接打印这个数
    assert(zg_tier_is_head(11) && zg_tier_is_head(21) && zg_tier_is_head(31));
    assert(!zg_tier_is_head(1) && !zg_tier_is_head(10) && !zg_tier_is_head(15));

    // 血量在换阶梯那一关明显往上跳:1→2、2→4 两跳至少翻倍,4→6 那一跳至少 1.5 倍。
    // 跳幅比倍数本身略高一点是正常的——阶梯内每 5 关还多给 1 点基础血。
    assert(zg_zombie_hp(ZG_Z_SMALL, 11) >= 2 * zg_zombie_hp(ZG_Z_SMALL, 10));
    assert(zg_zombie_hp(ZG_Z_BIG, 21) >= 2 * zg_zombie_hp(ZG_Z_BIG, 20));
    assert(2 * zg_zombie_hp(ZG_Z_SMALL, 31) >= 3 * zg_zombie_hp(ZG_Z_SMALL, 30));
    // 无尽段还在继续变厚:第 99 关比第 40 关更硬,直到血被 uint8_t 夹住为止。
    // 旧的"第 40 关就是天花板"在这里会红——那正是"一局太短"的根因,不该回来。
    assert(zg_zombie_hp(ZG_Z_SMALL, 99) > zg_zombie_hp(ZG_Z_SMALL, ZG_LEVEL_MAX));
    assert(zg_zombie_hp(ZG_Z_SMALL, ZG_LEVEL_CEIL) == 255);

    // 速度同样在涨,但用的是另一张更缓的表:六倍血量已经是一堵墙,再乘六倍速度
    // 就是一屏到底,那不叫难叫看不见。这条是"故意偏离 6 倍"的凭据。
    assert(zg_tier_vy_mul(1) == 100 && zg_tier_vy_mul(11) == 150);
    assert(zg_tier_vy_mul(21) == 200 && zg_tier_vy_mul(31) == 250);
    assert(zg_zombie_vy(ZG_Z_SMALL, 11) > zg_zombie_vy(ZG_Z_SMALL, 10));
    for (uint8_t lv = 1; lv <= ZG_LEVEL_MAX; lv++) {
        assert(zg_zombie_vy(ZG_Z_SMALL, lv) <= ZG_ZOMBIE_VY_MAX);
        assert(zg_zombie_vy(ZG_Z_AMBUSH, lv) <= ZG_ZOMBIE_VY_MAX);
    }

    // 刷怪跟着阶梯压密,但压到硬底就停:越往后是"更厚 + 更多",不是糊满一屏。
    assert(zg_spawn_interval_ms(11) < zg_spawn_interval_ms(10));
    assert(zg_spawn_interval_ms(21) == ZG_SPAWN_MIN_MS);
    assert(zg_spawn_interval_ms(ZG_LEVEL_MAX) == ZG_SPAWN_MIN_MS);
    assert(ZG_SPAWN_MIN_MS >= 500);

    // 枪跟着阶梯涨伤害与穿透,克隆豌豆射手也一样:否则后期三株克隆一回合磨不掉一格血。
    assert(zg_clone_damage(1) == ZG_CLONE_DAMAGE);
    assert(zg_clone_damage(11) == ZG_CLONE_DAMAGE * 2);
    assert(zg_clone_damage(ZG_LEVEL_MAX) == ZG_CLONE_DAMAGE * 6);

    // 抢卡概率跟着阶梯往上给,而且永远是个能兑现的百分数。
    assert(zg_zombie_eat_chance_pct(1) == 20);
    assert(zg_zombie_eat_chance_pct(11) == 30);
    assert(zg_zombie_eat_chance_pct(21) == 50);
    assert(zg_zombie_eat_chance_pct(ZG_LEVEL_MAX) == 60);
    for (uint8_t lv = 2; lv <= ZG_LEVEL_MAX; lv++) {
        assert(zg_zombie_eat_chance_pct(lv) >= zg_zombie_eat_chance_pct(lv - 1));
        assert(zg_zombie_eat_chance_pct(lv) < 100);   // 不是必抢:走位接卡一直有意义
    }
}

// 每跨过一关都要留一份能落盘的进度:引擎只标"待存档",写 NVS 归应用层。
static void test_progress_is_marked_every_level(void)
{
    zg_game_t g;
    zg_init(&g, 7, 0);
    to_play(&g);
    assert(!g.progress_dirty);                    // 刚开局没什么要写的
    g.score = ZG_LEVEL_SCORE_STEP;                // 刚好跨过第二关
    zg_step(&g, TICK_MS);
    assert(g.level == 2 && g.progress_dirty);
    g.progress_dirty = false;                     // 应用层写盘之后清标记
    // 同一关里再怎么涨分都不该重复写:Flash 的擦写次数不是无限的。
    g.score = 2u * ZG_LEVEL_SCORE_STEP - 1u;
    zg_step(&g, TICK_MS);
    assert(g.level == 2 && !g.progress_dirty);
    // 再跨一关又标一次,枪跟着换到那一档。
    g.score = 2u * ZG_LEVEL_SCORE_STEP;
    zg_step(&g, TICK_MS);
    assert(g.level == 3 && g.progress_dirty);
    assert(g.weapon == zg_weapon_for_level(3));

    // 无尽段没有"最后一格":第 40 关之后每跨一关照样标一次,待遇完全一样。
    g.progress_dirty = false;
    g.score = (uint32_t)ZG_LEVEL_SCORE_STEP * ZG_LEVEL_MAX;
    zg_step(&g, TICK_MS);
    assert(g.phase == ZG_PHASE_PLAY && g.level == ZG_LEVEL_MAX + 1 && g.progress_dirty);

    // 重开不写进度:纪录里的"最远关卡"是纪录,新局的第 1 关不该把它抹掉。
    // 打死那条路由 test_ten_hp_three_lives_then_game_over 覆盖,这里只关心重开的账。
    g.progress_dirty = false;
    g.phase = ZG_PHASE_OVER;
    assert(zg_restart(&g));
    assert(!g.progress_dirty && g.score == 0 && g.level == 1);
}

// 开机永远从第一关起:读回来的纪录只当比较基准,不当断点。
// 这条替掉了旧的"从存档分数接着打"—— 无尽模式没有位置可以续,能续的只有纪录。
static void test_boot_starts_at_level_one(void)
{
    zg_game_t g;
    zg_init(&g, 9, 12345u);                        // 历史最高分 12345
    assert(g.phase == ZG_PHASE_READY);
    assert(g.score == 0 && g.level == 1);          // 分数再高也不把位置回放回来
    assert(g.best == 12345u);                      // 纪录留着做 HI 显示和破纪录比较
    assert(g.weapon == ZG_WPN_PEA);                // 枪跟着关卡走:第 1 关就是最慢那颗豌豆
    assert(g.hp == ZG_HP_MAX && g.lives == ZG_LIVES_MAX);
    assert(!g.best_dirty && !g.progress_dirty);    // 读回来的东西不用再写一遍
    assert(g.spawn_ms == zg_spawn_interval_ms(1));
    assert(g.item_ms == ZG_ITEM_FIRST_MS);

    // 打死之后重开是同一件事:回到第一关,纪录一个字都不动。
    g.phase = ZG_PHASE_OVER;
    assert(zg_restart(&g));
    assert(g.level == 1 && g.score == 0 && g.best == 12345u);
    assert(!g.progress_dirty && !g.best_dirty);
}

static void test_spawn_pool_and_bounds(void)
{
    zg_game_t g;
    zg_init(&g, 16, 0);
    to_play(&g);
    // 开局倒计时之后完全不干预,看引擎自己会不会越界、会不会漏出还没解锁的怪。
    bool saw_spawn = false;
    bool saw_item = false;
    for (int i = 0; i < 3000; i++) {   // 最多 2 分钟
        zg_step(&g, TICK_MS);
        assert(g.player_x >= 0 && g.player_x <= ZG_PX_TO_POS(ZG_FIELD_W - ZG_PLAYER_W));
        assert(g.hp <= ZG_HP_MAX);
        assert(g.lives <= ZG_LIVES_MAX);
        assert(g.level == zg_level_of_score(g.score));       // 关卡只跟分数挂钩
        assert(g.level >= 1 && g.level <= ZG_LEVEL_MAX);
        assert(g.weapon == zg_weapon_for_level(g.level));    // 换枪只跟关卡挂钩
        assert(g.bonus_shots <= ZG_MAX_SHOTS);
        int alive = 0;
        for (unsigned j = 0; j < ZG_POOL_ZOMBIES; j++) {
            const zg_zombie_t *z = &g.zombies[j];
            if (!z->alive) continue;
            alive++;
            saw_spawn = true;
            assert(z->hp >= 1);                     // 活着的僵尸血量一定还没被打空
            assert(z->x >= 0 && z->x <= ZG_PX_TO_POS(ZG_FIELD_W - z->w));   // 反弹生效,不出界
            assert(z->y >= -ZG_PX_TO_POS(z->h));                             // 从上方走进来
            assert(z->y < ZG_PX_TO_POS(ZG_FIELD_H + 8));                      // 漏过就回收
            assert((unsigned)z->type < ZG_Z_TYPE_COUNT);
            assert(z->w == zg_zombie_info(z->type)->w);
            assert(z->h == zg_zombie_info(z->type)->h);
            // 没到解锁级别就不该出现这种怪。
            assert(g.level >= zg_zombie_info(z->type)->unlock_level);
            // 预警中的突袭僵尸不移动、也不伤人。
            if (z->telegraph_ms > 0) assert(z->type == ZG_Z_AMBUSH);
            // 分身自己绝不再当母体:每一只复制出来的怪都必须带着"已经生过"的标记,
            // 否则 20 秒窗口里会变成指数增长。
            if (z->is_clone) assert(z->cloned);
        }
        assert(alive <= ZG_POOL_ZOMBIES);
        // 同屏数量必须守住这一级的上限,不然第 1 关就不再是"要少"。这条按"非分身"算:
        // 卡片反噬复制出来的分身只吃僵尸池尾部那几只余量,不该把上限一起顶掉,
        // 否则第 1 关接了卡片就能刷出一整屏怪。
        assert(alive - zg_clone_zombie_count(&g) <= zg_max_alive(g.level));
        assert(zg_alive_count(&g) == alive);
        for (unsigned j = 0; j < ZG_POOL_BULLETS; j++) {
            if (!g.bullets[j].alive) continue;
            assert(g.bullets[j].y <= ZG_PX_TO_POS(ZG_FIELD_H));
            assert(g.bullets[j].vy < 0);            // 子弹只朝上飞
        }
        for (unsigned j = 0; j < ZG_POOL_ITEMS; j++) {
            const zg_item_t *it = &g.items[j];
            if (!it->alive) continue;
            saw_item = true;
            assert((unsigned)it->kind < ZG_ITEM_COUNT);
            assert(it->x >= 0 && it->x <= ZG_PX_TO_POS(ZG_FIELD_W - ZG_ITEM_SIZE));
            assert(it->y < ZG_PX_TO_POS(ZG_FIELD_H + ZG_ITEM_SIZE));   // 出底边就消失
            assert(it->vy == ZG_ITEM_VY);
        }
        for (unsigned j = 0; j < ZG_POOL_FX; j++) {
            if (!g.fx[j].alive) continue;
            assert(g.fx[j].ttl_ms <= g.fx[j].life_ms && g.fx[j].ttl_ms > 0);
            assert((unsigned)g.fx[j].kind < ZG_FX_KIND_COUNT);
        }
        if (g.phase == ZG_PHASE_OVER) break;
    }
    assert(saw_spawn);                       // 站着不动也一定有怪刷出来
    assert(saw_item);                        // 也一定见过道具掉下来
    assert(g.phase != ZG_PHASE_OVER || g.lives == 0);
    // 站着不动又基本打不中,几分钟内一定会掉血或拿到分。
    assert(g.elapsed_ms > 0);
    assert(g.hp < ZG_HP_MAX || g.kills > 0 || g.lives < ZG_LIVES_MAX);
    assert(g.best >= g.score);               // 实时刷新的纪录不会落后于当前分
}

// 观测指纹:只比可变状态,不比结构体尾部填充。
static uint32_t checksum(const zg_game_t *g)
{
    uint32_t h = 2166136261u;
    const uint32_t fields[] = {
        (uint32_t)g->phase, (uint32_t)g->paused, g->score, g->best, (uint32_t)g->best_dirty,
        g->hp, g->lives, (uint32_t)g->weapon, g->level, g->kills, g->elapsed_ms,
        (uint32_t)g->player_x, (uint32_t)g->invuln_ms, (uint32_t)g->reload_ms,
        (uint32_t)g->ready_ms, (uint32_t)g->levelup_ms, (uint32_t)g->spawn_ms,
        (uint32_t)g->item_ms, (uint32_t)g->rapid_ms, (uint32_t)g->bonus_shots,
        (uint32_t)g->clone_active, (uint32_t)g->clones, (uint32_t)g->learn_ms,
        (uint32_t)g->zclone_ms, (uint32_t)g->clone_reload_ms[0],
        (uint32_t)g->zclone_step_ms, (uint32_t)g->progress_dirty,
        (uint32_t)g->move_dir, (uint32_t)g->hold_ms, g->rng,
    };
    for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        h = (h ^ fields[i]) * 16777619u;
    }
    for (unsigned i = 0; i < ZG_POOL_ZOMBIES; i++) {
        const zg_zombie_t *z = &g->zombies[i];
        h = (h ^ (uint32_t)z->alive ^ ((uint32_t)z->x << 3) ^ ((uint32_t)z->y << 5) ^
             ((uint32_t)z->hp << 7) ^ ((uint32_t)z->type << 11) ^
             ((uint32_t)z->airborne << 13) ^ ((uint32_t)z->telegraph_ms << 17) ^
             ((uint32_t)z->is_clone << 23) ^ ((uint32_t)z->cloned << 24) ^
             ((uint32_t)z->carded << 25)) * 16777619u;
    }
    for (unsigned i = 0; i < ZG_POOL_BULLETS; i++) {
        const zg_bullet_t *b = &g->bullets[i];
        h = (h ^ (uint32_t)b->alive ^ ((uint32_t)b->x << 3) ^ ((uint32_t)b->y << 5)) * 16777619u;
    }
    // 道具也要进指纹:否则"同一局两边接到的是不是同一件"根本没被测到。
    for (unsigned i = 0; i < ZG_POOL_ITEMS; i++) {
        const zg_item_t *it = &g->items[i];
        h = (h ^ (uint32_t)it->alive ^ ((uint32_t)it->x << 3) ^ ((uint32_t)it->y << 5) ^
             ((uint32_t)it->kind << 9) ^ ((uint32_t)it->dodged << 11)) * 16777619u;
    }
    return h;
}

static void test_same_seed_same_round(void)
{
    zg_game_t a;
    zg_game_t b;
    zg_init(&a, 0xC0FFEE, 0);
    zg_init(&b, 0xC0FFEE, 0);
    for (int i = 0; i < 4000; i++) {
        // 脚本化输入:两边做完全一样的走位和一样的帧长序列,结果必须一模一样。
        const bool left = (i / 25) % 4 == 1;
        const bool right = (i / 25) % 4 == 3;
        const uint16_t dt = (i % 7 == 0) ? (uint16_t)60 : TICK_MS;
        zg_set_keys(&a, left, right);
        zg_set_keys(&b, left, right);
        zg_step(&a, dt);
        zg_step(&b, dt);
        if (a.phase == ZG_PHASE_OVER && b.phase == ZG_PHASE_OVER) break;
    }
    assert(a.phase == b.phase);
    assert(a.score == b.score && a.kills == b.kills);
    assert(a.level == b.level && a.elapsed_ms == b.elapsed_ms);
    assert(a.player_x == b.player_x);
    assert(a.hp == b.hp && a.lives == b.lives);
    assert(checksum(&a) == checksum(&b));

    // 换个种子就该走出不同的一局(至少僵尸位置不同)。
    zg_init(&a, 1, 0);
    zg_init(&b, 2, 0);
    for (int i = 0; i < 800; i++) {
        zg_step(&a, TICK_MS);
        zg_step(&b, TICK_MS);
    }
    assert(checksum(&a) != checksum(&b));
}

int main(void)
{
    test_tables_are_consistent();
    test_init_defaults();
    test_movement_and_bounds();
    test_holding_a_direction_doubles_speed();
    test_pause_freezes_the_whole_round();
    test_dt_is_clamped();
    test_gun_fires_itself();
    test_small_zombie_dies_in_two_rifle_shots();
    test_scoring_advances_levels_and_guns();
    test_leak_through_the_line_costs_hp();
    test_contact_costs_hp_once_then_invuln();
    test_ten_hp_three_lives_then_game_over();
    test_jumper_is_only_shootable_on_the_ground();
    test_ambush_telegraphs_then_falls();
    test_difficulty_ramps_and_caps();
    test_gun_scales_with_level();
    test_level_change_is_announced();
    test_items_are_caught_by_walking();
    test_endless_has_no_finish_line();
    test_railgun_pierces_a_column();
    test_clone_card_plants_three_shooters();
    test_zombie_backlash_is_late_and_limited();
    test_zombie_steals_a_falling_card();
    test_difficulty_tiers_stack_up();
    test_progress_is_marked_every_level();
    test_boot_starts_at_level_one();
    test_spawn_pool_and_bounds();
    test_same_seed_same_round();
    puts("zombie shooter engine tests: PASS");
    return 0;
}
