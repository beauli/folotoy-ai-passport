// tests/test_zombie_game.c
// 豌豆射手守草坪引擎的 host 测试:纯逻辑,不依赖 ESP-IDF/LVGL。
// 这里钉的是"手感数值 + 判定规则":几发打死一只、什么时候自动换枪、
// 漏怪和撞人各扣多少血、10 血 3 命怎么结账、跳跃/突袭僵尸的特权什么时候生效,
// 以及 10 个级别的难度曲线——开局必须弱,后面逐级变强,枪要跟着一起变强。
// 这些一旦改错,设备上看起来只是"变难了一点",很难靠肉眼发现。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "zombie_game.h"

#define TICK_MS 40

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
    // 枪跟着关卡走:第 1/3/5/7/9 关各换一档,换关一定能感到枪也变了。
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
    assert(zg_weapon_for_level(ZG_LEVEL_MAX) == ZG_WPN_RAILGUN);
    // 越界传进来也不能放飞:第 0 关和第 99 关都得夹回首尾两档。
    assert(zg_weapon_for_level(0) == ZG_WPN_PEA);
    assert(zg_weapon_for_level(99) == ZG_WPN_RAILGUN);

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
    assert(ZG_LEVEL_MAX == 10);
    assert(ZG_WIN_SCORE == (uint32_t)ZG_LEVEL_SCORE_STEP * ZG_LEVEL_MAX);
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
    assert(zg_max_alive(ZG_LEVEL_MAX) == ZG_POOL_ZOMBIES);
    for (uint8_t lv = 2; lv <= ZG_LEVEL_MAX; lv++) {
        assert(zg_spawn_interval_ms(lv) <= zg_spawn_interval_ms(lv - 1));
        assert(zg_max_alive(lv) >= zg_max_alive(lv - 1));
        assert(zg_max_alive(lv) <= ZG_POOL_ZOMBIES);
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
    place(&g, 0, ZG_Z_SMALL, 115, 200, 0, 0);
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
    assert(g.best == g.score && g.best_dirty);   // 破了 0 分纪录,标记待落盘
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
            place(&g, 0, ZG_Z_SMALL, 115, 200, 0, 0);
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
    assert(zg_level_of_score(ZG_WIN_SCORE - 1) == ZG_LEVEL_MAX);
    assert(zg_level_of_score(1000000u) == ZG_LEVEL_MAX);
    for (uint8_t lv = 1; lv < ZG_LEVEL_MAX; lv++) {
        assert(zg_zombie_hp(ZG_Z_SMALL, (uint8_t)(lv + 1)) >= zg_zombie_hp(ZG_Z_SMALL, lv));
        assert(zg_zombie_vy(ZG_Z_SMALL, (uint8_t)(lv + 1)) > zg_zombie_vy(ZG_Z_SMALL, lv));
        assert(zg_zombie_vy(ZG_Z_BIG, (uint8_t)(lv + 1)) > zg_zombie_vy(ZG_Z_BIG, lv));
        assert(zg_spawn_interval_ms((uint8_t)(lv + 1)) <= zg_spawn_interval_ms(lv));
    }
    // 封顶:第 10 关以后不再无限涨,否则后期数值会失控。
    assert(zg_zombie_hp(ZG_Z_SMALL, ZG_LEVEL_MAX) > zg_zombie_hp(ZG_Z_SMALL, 1));
    assert(zg_spawn_interval_ms(99) >= 1000);  // 最密也是一秒多一只,不会刷成一片
    assert(zg_spawn_interval_ms(99) <= zg_spawn_interval_ms(1));
    assert(zg_max_alive(99) == ZG_POOL_ZOMBIES);
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
        place(&g, 0, ZG_Z_SMALL, 115, 200, 0, 0);
        assert(shoot_one_dead(&g, 0) >= 0);
    }
    assert(g.score == 9u * ZG_LEVEL_SCORE_STEP / 10u && g.level == 1);
    place(&g, 0, ZG_Z_SMALL, 115, 200, 0, 0);
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
    // 整局通关的合理区间:乐观两分多,悲观四分多。超出这个框,说明关卡分数或
    // 刷怪节奏有一头飘了——上板时表现为"怎么都打不完"或"一会儿就通关"。
    assert(pace_lo >= 90 && pace_lo <= 240);
    assert(pace_hi >= pace_lo && pace_hi <= 300);

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
    assert(GUN_BY_LEVEL[ZG_LEVEL_MAX] == ZG_WPN_RAILGUN);
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

    // 封顶:打到第 10 关只亮一次横幅,之后分数再怎么涨都不换关。
    for (unsigned i = 0; i < ZG_POOL_ZOMBIES; i++) g.zombies[i].alive = false;
    g.score = (uint32_t)(ZG_LEVEL_MAX - 1u) * ZG_LEVEL_SCORE_STEP;
    zg_step(&g, TICK_MS);
    assert(g.level == ZG_LEVEL_MAX && g.levelup_ms == ZG_LEVELUP_MS);
    for (int i = 0; i < 200; i++) step_pure(&g, TICK_MS);
    assert(g.level == ZG_LEVEL_MAX && g.levelup_ms == 0);
    assert(zg_alive_count(&g) == 0);   // 倒计时冻住,场上不会自己冒出怪

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
    assert(zg_unlock_at(ZG_LEVEL_MAX) == NULL);   // 第 10 级没有新怪,只有"封顶"

    // 草坪色阶:换档必须落在真的会换关的级别上,而且全程不越界。
    assert(zg_lawn_group(1) == 0 && zg_lawn_group(3) == 0);
    assert(zg_lawn_group(4) == 1);
    assert(zg_lawn_group(7) == 2);
    assert(zg_lawn_group(ZG_LEVEL_MAX) == ZG_LAWN_GROUPS - 1);
    for (uint8_t lv = 1; lv <= ZG_LEVEL_MAX; lv++) assert(zg_lawn_group(lv) < ZG_LAWN_GROUPS);
}

static void test_clearing_all_levels_wins(void)
{
    zg_game_t g;
    zg_init(&g, 31, 0);
    to_play(&g);
    g.reload_ms = 9999;

    // 第 10 关:分数已经在 9000~9999 这一档。场上照常有怪,但时间走得再久也不该结束。
    // 这一条专门钉住"通关看积分而不是看时间"——旧的按时间规则会在这里红。
    g.elapsed_ms = 600000u;
    g.score = ZG_WIN_SCORE - 50u;
    g.level = zg_level_of_score(g.score);
    assert(g.level == ZG_LEVEL_MAX);
    place(&g, 0, ZG_Z_SMALL, 100, 60, 0, 0);   // 站住的怪:只用来确认"场上有怪但还没通关"
    assert(zg_alive_count(&g) == 1);
    for (int i = 0; i < 100; i++) step_pure(&g, TICK_MS);
    assert(g.phase == ZG_PHASE_PLAY && g.level == ZG_LEVEL_MAX);
    assert(zg_alive_count(&g) == 1);

    // 把分数推到通关线,只用一步跨过。
    const uint32_t elapsed_before = g.elapsed_ms;
    g.score = ZG_WIN_SCORE;
    zg_step(&g, TICK_MS);
    assert(g.phase == ZG_PHASE_WIN);
    assert(g.level == ZG_LEVEL_MAX);
    assert(g.levelup_ms == 0);
    assert(zg_alive_count(&g) == 0);                       // 恭喜画面不留怪
    assert(!g.items[0].alive && !g.items[1].alive);        // 道具也一起清掉
    assert(g.hp == ZG_HP_MAX);                             // 通关不该顺手扣血
    assert(g.best == ZG_WIN_SCORE && g.best_dirty);        // 通关也要把纪录落盘

    // 终局:时间停走、暂停停不了、只有左右键重开能走。
    assert(g.elapsed_ms == elapsed_before + TICK_MS);
    for (int i = 0; i < 100; i++) step_pure(&g, TICK_MS);
    assert(g.phase == ZG_PHASE_WIN && g.elapsed_ms == elapsed_before + TICK_MS);
    assert(g.score == ZG_WIN_SCORE);
    zg_set_paused(&g, true);
    assert(!g.paused);                        // 恭喜画面暂停不了,和 GAME OVER 一样
    zg_set_paused(&g, false);
    assert(!g.paused);
    assert(zg_restart(&g));
    assert(g.phase == ZG_PHASE_READY && g.level == 1 && g.best_dirty);

    // 奖章只看通关时还剩几条命:一条没丢是金,丢一条银,其余铜。
    assert(zg_medal_of(ZG_LIVES_MAX) == ZG_MEDAL_GOLD);
    assert(zg_medal_of(2) == ZG_MEDAL_SILVER);
    assert(zg_medal_of(1) == ZG_MEDAL_BRONZE);
    assert(zg_medal_of(0) == ZG_MEDAL_BRONZE);
    // UI 直接打印档位名,任何一档都不能是空串。
    for (int m = 0; m < ZG_MEDAL_COUNT; m++) {
        const char *name = zg_medal_name((zg_medal_t)m);
        assert(name && name[0]);
    }
    assert(zg_medal_name(ZG_MEDAL_NONE) && strcmp(zg_medal_name(ZG_MEDAL_NONE), "NONE") == 0);
}

static void test_railgun_pierces_a_column(void)
{
    zg_game_t g;
    zg_init(&g, 15, 0);
    to_play(&g);
    // 枪跟着关卡走:要磁轨枪就得打到第 9 关,分数和级别必须一起摆正。
    g.score = (uint32_t)(ZG_LEVEL_MAX - 2u) * ZG_LEVEL_SCORE_STEP;   // 8000 → 第 9 关
    g.level = zg_level_of_score(g.score);
    g.weapon = zg_weapon_for_level(g.level);
    assert(g.level == 9 && g.weapon == ZG_WPN_RAILGUN);

    // 同一列上前后两只:磁轨枪一发穿透两只。
    place(&g, 0, ZG_Z_SMALL, 115, 200, 0, 0);
    place(&g, 1, ZG_Z_SMALL, 115, 210, 0, 0);
    g.zombies[0].hp = 3;
    g.zombies[1].hp = 3;
    for (int i = 0; i < 200 && (g.zombies[0].alive || g.zombies[1].alive); i++) {
        step_pure(&g, TICK_MS);
    }
    assert(!g.zombies[0].alive && !g.zombies[1].alive);
    assert(g.kills == 2);
    assert(g.score >= (uint32_t)(ZG_LEVEL_MAX - 2u) * ZG_LEVEL_SCORE_STEP +
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
        }
        assert(alive <= ZG_POOL_ZOMBIES);
        // 同屏数量必须守住这一级的上限,不然第 1 关就不再是"要少"。
        assert(alive <= zg_max_alive(g.level));
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
        (uint32_t)g->move_dir, (uint32_t)g->hold_ms, g->rng,
    };
    for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        h = (h ^ fields[i]) * 16777619u;
    }
    for (unsigned i = 0; i < ZG_POOL_ZOMBIES; i++) {
        const zg_zombie_t *z = &g->zombies[i];
        h = (h ^ (uint32_t)z->alive ^ ((uint32_t)z->x << 3) ^ ((uint32_t)z->y << 5) ^
             ((uint32_t)z->hp << 7) ^ ((uint32_t)z->type << 11) ^
             ((uint32_t)z->airborne << 13) ^ ((uint32_t)z->telegraph_ms << 17)) * 16777619u;
    }
    for (unsigned i = 0; i < ZG_POOL_BULLETS; i++) {
        const zg_bullet_t *b = &g->bullets[i];
        h = (h ^ (uint32_t)b->alive ^ ((uint32_t)b->x << 3) ^ ((uint32_t)b->y << 5)) * 16777619u;
    }
    // 道具也要进指纹:否则"同一局两边接到的是不是同一件"根本没被测到。
    for (unsigned i = 0; i < ZG_POOL_ITEMS; i++) {
        const zg_item_t *it = &g->items[i];
        h = (h ^ (uint32_t)it->alive ^ ((uint32_t)it->x << 3) ^ ((uint32_t)it->y << 5) ^
             ((uint32_t)it->kind << 9)) * 16777619u;
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
    test_clearing_all_levels_wins();
    test_railgun_pierces_a_column();
    test_spawn_pool_and_bounds();
    test_same_seed_same_round();
    puts("zombie shooter engine tests: PASS");
    return 0;
}
