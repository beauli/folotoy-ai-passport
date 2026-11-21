// main/zombie_game.c
// 游戏引擎实现:定点数运算,固定对象池,无动态内存、无浮点、无 ESP 依赖。
// 数值都集中在下面的参数表里,调手感只改这里,UI 与测试读同一份表。
#include "zombie_game.h"

#include <string.h>

// 节奏与判定门限(ZG_DT_*、ZG_READY_*、ZG_TELEGRAPH_MS、ZG_LEVEL_*、ZG_HOP_* 等)
// 放在 zombie_game.h:UI 要按同一套时间画预警和倒计时,host 测试也要钉住这些数值,
// 抄两份迟早会对不上。
#define ZG_SPREAD_RATE      4   // 霰弹每偏 1 px 弹道再横向 4 px/s

// ---- 特效寿命(UI 只按 ttl 画,不在自己这边计时间) ----
#define FX_TTL_MUZZLE    90
#define FX_TTL_HIT      120
#define FX_TTL_DEATH    260
#define FX_TTL_LEAK     300
#define FX_TTL_SPAWN    260
#define FX_TTL_HURT     350
#define FX_TTL_GUNUP 1200
#define FX_TTL_PICKUP   400

// 档位跟着关卡走:第 1/3/5/7/9 关各换一档,所以"换关"一定同时看得见怪变多、
// 枪变强,不会只有一行字。发数最多 4(和道具加成共用 ZG_MAX_SHOTS 这个顶):
// 三个维度一起往前推——弹速 220→440、间隔 900→300、伤害 1→4。
// 开局那颗豌豆就是要"慢慢吐、打两下才死",难的地方留给走位。
static const zg_weapon_info_t WPN[ZG_WPN_COUNT] = {
    //  名称       间隔  发数 扇距 伤害 穿透 弹速  起用关卡
    { "PEA",      900,   1,   0,  1,  0,  220,    1 },
    { "SPLIT",    720,   2,  10,  1,  0,  270,    3 },
    { "THREE",    520,   3,  12,  2,  0,  320,    5 },
    { "RAPID",    300,   3,  10,  2,  0,  380,    7 },
    { "RAILGUN",  420,   4,  12,  4,  2,  440,    9 },
};

// 血量与速度都从"第 1 关几乎没威胁"起步:小僵尸 2 血、16 px/s,从屏幕上方走完
// 250px 要十几秒,足够先看清局面再抬手。
// jumper 的 base_vy 是腾空段速度:它一跳一跳地往下压,蓄力段按 1/6 速度蹭过来,
// 平均推进速度和小僵尸相当,但腾空那 0.3 秒子弹打不到,所以更难缠。
// 分值按"一关 1000 分"配:第 1 关只出小僵尸,正好 10 只换下一关;往后单只更值钱,
// 所以关卡越打越快,而压力靠同屏数量(zg_max_alive)和刷怪间隔往上加。
static const zg_zombie_info_t ZBN[ZG_Z_TYPE_COUNT] = {
    //  名称       宽  高  基础血 基础速度 得分 解锁关卡
    { "SMALL",  12, 14,     2,      16, 100,  1 },
    { "BIG",    22, 24,     4,       9, 250,  3 },
    { "JUMPER", 12, 16,     3,      40, 200,  5 },
    { "AMBUSH", 14, 14,     2,      28, 300,  7 },
};

// 道具配色放在引擎里,是因为 host 测试和 UI 要认同一件东西:测试钉住"接到的是哪件",
// UI 按同一个 kind 取色,两边不会各写一套。
static const zg_item_info_t ITN[ZG_ITEM_COUNT] = {
    { "FAST",   0x6FD8FF },   // 射速翻倍:冷色,和豌豆的绿分开
    { "PEA+",   0xB9E86A },   // 多一颗豌豆:跟豌豆同色,一眼认得出是什么
};

// 只在 C3 上跑不出问题,但 host 测试要能复现同一局,所以自带一个可播种的 xorshift。
static uint32_t rnd_next(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

// [0, span) 的整数随机数。
static int32_t rnd_below(uint32_t *state, int32_t span)
{
    if (span <= 0) return 0;
    return (int32_t)(rnd_next(state) % (uint32_t)span);
}

static int32_t clamp32(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// 定点位移:pos(1/16 px) += speed(px/s) * dt(ms)。dt 已被夹在 60 内,
// 最大速度 260 px/s,乘积不超过 int32 范围。
static int32_t advance(int32_t pos, int32_t speed, uint16_t dt)
{
    return pos + (speed * (1 << ZG_POS_SHIFT) * (int32_t)dt) / 1000;
}

// 倒计时统一走这里,免得每处都写一遍"减到 0 就停"。
static uint16_t tick_down(uint16_t value, uint16_t dt)
{
    return (value > dt) ? (uint16_t)(value - dt) : 0;
}

const zg_weapon_info_t *zg_weapon_info(zg_weapon_t weapon)
{
    return &WPN[(unsigned)weapon < ZG_WPN_COUNT ? (unsigned)weapon : 0];
}

const zg_zombie_info_t *zg_zombie_info(zg_zombie_type_t type)
{
    return &ZBN[(unsigned)type < ZG_Z_TYPE_COUNT ? (unsigned)type : 0];
}

const zg_item_info_t *zg_item_info(zg_item_kind_t kind)
{
    return &ITN[(unsigned)kind < ZG_ITEM_COUNT ? (unsigned)kind : 0];
}

uint8_t zg_level_of_score(uint32_t score)
{
    const uint32_t level = score / ZG_LEVEL_SCORE_STEP + 1u;
    return (uint8_t)(level > ZG_LEVEL_MAX ? ZG_LEVEL_MAX : level);
}

// 难度公式一律先把级别夹到 1..ZG_LEVEL_MAX:外面传进来越界也不会把数值放飞。
static int32_t level_cap(uint8_t level)
{
    return clamp32(level, 1, ZG_LEVEL_MAX);
}

int32_t zg_zombie_hp(zg_zombie_type_t type, uint8_t level)
{
    // 每 4 关才加 1 点血:第 10 关小僵尸 4 血,而那时手里最少是三伤的枪,一发多就倒。
    return zg_zombie_info(type)->base_hp + (level_cap(level) - 1) / 4;
}

int32_t zg_zombie_vy(zg_zombie_type_t type, uint8_t level)
{
    return zg_zombie_info(type)->base_vy + (level_cap(level) - 1);   // 第 10 关只比第 1 关快 9 px/s
}

uint16_t zg_spawn_interval_ms(uint8_t level)
{
    // 第 1 关 3.4 秒一只,逐级降到第 10 关 1.24 秒:"后面慢慢变多"首先就是这里变快。
    const int32_t interval = 3400 - (level_cap(level) - 1) * 240;
    return (uint16_t)(interval < 1200 ? 1200 : interval);
}

uint8_t zg_max_alive(uint8_t level)
{
    // 第 1 关场上最多 2 只,之后每关放开一只,第 6 关起吃满池子的 6 只。
    // 只调刷怪间隔不够:间隔短了但上限还是 2,玩家会觉得"打不完"而不是"来得多"。
    const int32_t alive = 1 + level_cap(level);
    return (uint8_t)(alive > ZG_POOL_ZOMBIES ? ZG_POOL_ZOMBIES : alive);
}

uint16_t zg_item_interval_ms(uint8_t level)
{
    // 道具越往后越密,但最快也只到 11 秒一件:强化不该变成每关定量的白送。
    const int32_t interval = ZG_ITEM_STEP_MS - (level_cap(level) - 1) * 400;
    return (uint16_t)(interval < ZG_ITEM_MIN_MS ? ZG_ITEM_MIN_MS : interval);
}

const zg_zombie_info_t *zg_unlock_at(uint8_t level)
{
    for (unsigned t = 0; t < ZG_Z_TYPE_COUNT; t++) {
        if (ZBN[t].unlock_level == level_cap((uint8_t)level)) return &ZBN[t];
    }
    return NULL;
}

uint8_t zg_lawn_group(uint8_t level)
{
    // 1~3 / 4~6 / 7~9 / 10:四档色阶,换档正好落在新僵尸登场的那几关上。
    const int32_t group = (level_cap(level) - 1) / 3;
    return (uint8_t)(group > ZG_LAWN_GROUPS - 1 ? ZG_LAWN_GROUPS - 1 : group);
}

zg_weapon_t zg_weapon_for_level(uint8_t level)
{
    zg_weapon_t best = ZG_WPN_PEA;
    const int32_t lv = level_cap(level);
    for (unsigned i = 1; i < ZG_WPN_COUNT; i++) {
        if (lv >= WPN[i].first_level) best = (zg_weapon_t)i;
    }
    return best;
}

uint8_t zg_volley(const zg_game_t *game)
{
    // 档位发数 + 道具加成,但硬顶是 ZG_MAX_SHOTS:第 9 关的磁轨枪本来就已经 4 发,
    // 再吃道具也不会把弹池吃空。
    const int32_t shots = (int32_t)zg_weapon_info(game->weapon)->shots + game->bonus_shots;
    return (uint8_t)(shots > ZG_MAX_SHOTS ? ZG_MAX_SHOTS : shots);
}

uint16_t zg_fire_interval_ms(const zg_game_t *game)
{
    const uint16_t base = zg_weapon_info(game->weapon)->interval_ms;
    return (game->rapid_ms > 0) ? (uint16_t)(base / ZG_RAPID_DIV) : base;
}

zg_medal_t zg_medal_of(uint8_t lives)
{
    // 命数是唯一标准:通关时手里还有 3 条命就是全程没丢过命。
    if (lives >= 3) return ZG_MEDAL_GOLD;
    if (lives == 2) return ZG_MEDAL_SILVER;
    return ZG_MEDAL_BRONZE;
}

const char *zg_medal_name(zg_medal_t medal)
{
    switch (medal) {
    case ZG_MEDAL_GOLD:   return "GOLD";
    case ZG_MEDAL_SILVER: return "SILVER";
    case ZG_MEDAL_BRONZE: return "BRONZE";
    case ZG_MEDAL_NONE:
    default:              return "NONE";
    }
}

// ---- 对象池 ----

static zg_fx_t *fx_slot(zg_game_t *g)
{
    for (unsigned i = 0; i < ZG_POOL_FX; i++) {
        if (!g->fx[i].alive) return &g->fx[i];
    }
    return NULL;   // 池满就丢一个特效:宁可少个动画,也不让游戏停下来
}

static void fx_add(zg_game_t *g, zg_fx_kind_t kind, int16_t x, int16_t y, uint16_t life)
{
    zg_fx_t *slot = fx_slot(g);
    if (!slot) return;
    *slot = (zg_fx_t){ .x = x, .y = y, .ttl_ms = life, .life_ms = life,
                       .kind = kind, .alive = true };
}

// 僵尸中心点,特效和命中判定都用它。
static int16_t zcx(const zg_zombie_t *z)
{
    return (int16_t)(ZG_POS_TO_PX(z->x) + z->w / 2);
}

static int16_t zcy(const zg_zombie_t *z)
{
    return (int16_t)(ZG_POS_TO_PX(z->y) + z->h / 2);
}

int zg_alive_count(const zg_game_t *g)
{
    int n = 0;
    for (unsigned i = 0; i < ZG_POOL_ZOMBIES; i++) {
        if (g->zombies[i].alive) n++;
    }
    return n;
}

// ---- 判定 ----

static bool overlap(int32_t ax, int32_t aw, int32_t ay, int32_t ah,
                    int32_t bx, int32_t bw, int32_t by, int32_t bh)
{
    // 半开区间相交:边对边贴在一起不算撞上,免得站在一起的两只僵尸互相判碰撞。
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

bool zg_zombie_hits_player(const zg_game_t *g, const zg_zombie_t *z)
{
    if (!z || !z->alive || z->telegraph_ms > 0) return false;
    return overlap(g->player_x, ZG_PX_TO_POS(ZG_PLAYER_W), g->player_y,
                   ZG_PX_TO_POS(ZG_PLAYER_H), z->x, ZG_PX_TO_POS(z->w), z->y,
                   ZG_PX_TO_POS(z->h));
}

bool zg_bullet_hits_zombie(const zg_bullet_t *b, const zg_zombie_t *z)
{
    if (!b || !z || !b->alive || !z->alive) return false;
    // 腾空中的跳跃僵尸打不到:这是它唯一的规避手段,不然它和普通僵尸没区别。
    if (z->airborne) return false;
    return overlap(b->x, ZG_PX_TO_POS(ZG_BULLET_W), b->y, ZG_PX_TO_POS(ZG_BULLET_H),
                   z->x, ZG_PX_TO_POS(z->w), z->y, ZG_PX_TO_POS(z->h));
}

// 道具不看僵尸那套命中:它只跟"人有没有走到下面",子弹永远打不到它。
bool zg_item_hits_player(const zg_game_t *g, const zg_item_t *it)
{
    if (!it || !it->alive) return false;
    return overlap(g->player_x, ZG_PX_TO_POS(ZG_PLAYER_W), g->player_y,
                   ZG_PX_TO_POS(ZG_PLAYER_H), it->x, ZG_PX_TO_POS(ZG_ITEM_SIZE),
                   it->y, ZG_PX_TO_POS(ZG_ITEM_SIZE));
}

// ---- 生命与结算 ----

static void clear_field(zg_game_t *g)
{
    for (unsigned i = 0; i < ZG_POOL_ZOMBIES; i++) g->zombies[i].alive = false;
    for (unsigned i = 0; i < ZG_POOL_BULLETS; i++) g->bullets[i].alive = false;
    for (unsigned i = 0; i < ZG_POOL_ITEMS; i++) g->items[i].alive = false;
}

static void commit_best(zg_game_t *g)
{
    if (g->score <= g->best) return;
    g->best = g->score;
    g->best_dirty = true;   // 落盘交给应用层:引擎不碰 NVS
}

static void lose_life(zg_game_t *g)
{
    g->lives = (g->lives > 0) ? (uint8_t)(g->lives - 1) : 0;
    clear_field(g);
    if (g->lives == 0) {
        g->phase = ZG_PHASE_OVER;
        commit_best(g);
        return;
    }
    g->hp = ZG_HP_MAX;
    g->phase = ZG_PHASE_READY;
    g->ready_ms = ZG_READY_MS;
    g->invuln_ms = ZG_INVULN_RESPAWN_MS;
    g->reload_ms = 0;
    g->levelup_ms = 0;   // 换关横幅不该跟着人一起进 GET READY,那是另一件事
}

// 僵尸碰到豌豆射手或漏过底边都算"防线被突破"。撞上的僵尸一定消失,不然
// 无敌期会被人贴在身上反复判定;打死不给分,得分只来自枪。
static void break_through(zg_game_t *g, zg_zombie_t *z, bool leak)
{
    const int16_t x = z ? zcx(z) : (int16_t)(ZG_POS_TO_PX(g->player_x) + ZG_PLAYER_W / 2);
    const int16_t y = leak ? (int16_t)(ZG_FIELD_H - 4) : zcy(z);
    fx_add(g, leak ? ZG_FX_LEAK : ZG_FX_HURT, x, y, leak ? FX_TTL_LEAK : FX_TTL_HURT);
    if (z) z->alive = false;

    if (g->invuln_ms > 0) return;   // 无敌期只清怪
    g->hp = (g->hp > 0) ? (uint8_t)(g->hp - 1) : 0;
    g->invuln_ms = ZG_INVULN_HURT_MS;   // 挨一下之后给一段无敌,不然叠在一起的僵尸会连扣
    if (g->hp == 0) lose_life(g);       // lose_life 会把它换成更长的复活无敌
}

// ---- 生成 ----

// 已经解锁的类型都进抽奖池。级别越高越偏向难缠的类型:第 6 关起给难缠类型补权重。
static void spawn_zombie(zg_game_t *g)
{
    // 同屏数量上限按关卡放:第 1 关最多 2 只,"要少"是这么来的,不是靠刷得慢。
    if (zg_alive_count(g) >= zg_max_alive(g->level)) return;

    zg_zombie_t *slot = NULL;
    for (unsigned i = 0; i < ZG_POOL_ZOMBIES; i++) {
        if (!g->zombies[i].alive) { slot = &g->zombies[i]; break; }
    }
    if (!slot) return;   // 池子已满:宁可少刷一只,也不让难度失控

    // 权重表:第 1 级只有小僵尸,3/5/7 级各加入一种;7 级起四种齐全且概率趋于拉平。
    uint32_t weights[ZG_Z_TYPE_COUNT];
    uint32_t total = 0;
    for (unsigned t = 0; t < ZG_Z_TYPE_COUNT; t++) {
        if (ZBN[t].unlock_level > g->level) { weights[t] = 0; continue; }
        weights[t] = 8u - t * 2u + (g->level >= 7 ? t : 0);
        total += weights[t];
    }
    if (total == 0) { weights[0] = 1; total = 1; }   // 理论不会发生,兜底不除零

    uint32_t pick = (uint32_t)rnd_below(&g->rng, (int32_t)total);
    zg_zombie_type_t type = ZG_Z_SMALL;
    for (unsigned t = 0; t < ZG_Z_TYPE_COUNT; t++) {
        if (pick < weights[t]) { type = (zg_zombie_type_t)t; break; }
        pick -= weights[t];
    }

    const zg_zombie_info_t *info = zg_zombie_info(type);
    // 生成范围按"像素"算再换算成定点数:直接在定点数上取模会多乘一次 16,
    // 僵尸会出生在场外,看着就是凭空冒出来又立刻被弹回边。
    const int32_t span_px = ZG_FIELD_W - info->w;
    *slot = (zg_zombie_t){
        .x = ZG_PX_TO_POS(rnd_below(&g->rng, span_px)),
        .y = -ZG_PX_TO_POS(info->h),          // 从屏幕上方外走进来
        .w = info->w,
        .h = info->h,
        .vx = 0,
        .vy = zg_zombie_vy(type, g->level),
        .hp = (uint8_t)zg_zombie_hp(type, g->level),
        .telegraph_ms = 0,
        .hop_ms = 0,
        .wait_ms = (type == ZG_Z_JUMPER) ? ZG_HOP_WAIT_MS : 0,
        .airborne = false,
        .alive = true,
        .type = type,
    };

    if (type == ZG_Z_AMBUSH) {
        // 突袭僵尸不走屏幕上方:直接在场中段现身,先亮一下预警再扑下来。
        slot->y = ZG_PX_TO_POS(60 + rnd_below(&g->rng, 70));
        slot->telegraph_ms = ZG_TELEGRAPH_MS;
        slot->vx = (rnd_below(&g->rng, 2) == 0) ? -18 : 18;
        fx_add(g, ZG_FX_SPAWN, zcx(slot), zcy(slot), FX_TTL_SPAWN);
    } else if (type == ZG_Z_JUMPER) {
        slot->vx = (rnd_below(&g->rng, 2) == 0) ? -10 : 10;
    } else if (type == ZG_Z_SMALL) {
        slot->vx = (rnd_below(&g->rng, 2) == 0) ? -8 : 8;
    }
}

// ---- 开火 ----

static void fire(zg_game_t *g)
{
    const zg_weapon_info_t *w = zg_weapon_info(g->weapon);
    // 发数 = 档位 + 道具加成,所以吃到 PEA+ 之后每一枪都多一颗,扇形也更宽。
    const uint8_t shots = zg_volley(g);
    const int32_t muzzle_x = g->player_x + ZG_PX_TO_POS(ZG_PLAYER_W / 2) - ZG_PX_TO_POS(ZG_BULLET_W / 2);
    const int32_t muzzle_y = g->player_y - ZG_PX_TO_POS(2);

    fx_add(g, ZG_FX_MUZZLE, ZG_POS_TO_PX(g->player_x) + ZG_PLAYER_W / 2,
           ZG_POS_TO_PX(g->player_y) - 4, FX_TTL_MUZZLE);

    for (uint8_t i = 0; i < shots; i++) {
        zg_bullet_t *slot = NULL;
        for (unsigned j = 0; j < ZG_POOL_BULLETS; j++) {
            if (!g->bullets[j].alive) { slot = &g->bullets[j]; break; }
        }
        if (!slot) return;   // 弹池满了就少打一发,不排队:排队会让节奏和显示不一致

        // 扇形按 (2i - shots + 1)/2 取偏移,奇偶发数都对称居中:两发是 -5/+5,
        // 三发是 -12/0/+12,四发是 -18/-6/+6/+18。
        // 横向速度按偏移给出,所以"多发"同时就是"多个方向"。
        const int32_t offset_px = ((2 * (int32_t)i - (int32_t)shots + 1) * w->spread_px) / 2;
        *slot = (zg_bullet_t){
            .x = muzzle_x + ZG_PX_TO_POS(offset_px),
            .y = muzzle_y,
            .vx = offset_px * ZG_SPREAD_RATE,
            .vy = -w->bullet_speed,
            .damage = w->damage,
            .pierce_left = w->pierce,
            .alive = true,
        };
    }
}

// ---- 逐步推进 ----

static void step_fx(zg_game_t *g, uint16_t dt)
{
    for (unsigned i = 0; i < ZG_POOL_FX; i++) {
        zg_fx_t *fx = &g->fx[i];
        if (!fx->alive) continue;
        fx->ttl_ms = tick_down(fx->ttl_ms, dt);
        if (fx->ttl_ms == 0) fx->alive = false;
    }
}

static void step_player(zg_game_t *g, uint16_t dt)
{
    g->invuln_ms = tick_down(g->invuln_ms, dt);
    // 按住时长只需要判断"够不够 0.5 秒",所以饱和在门限:既不溢出,也看得出已到档。
    if (g->move_dir == 0) g->hold_ms = 0;
    else if (g->hold_ms < ZG_SPRINT_MS) {
        g->hold_ms = (uint16_t)clamp32((int32_t)g->hold_ms + dt, 0, ZG_SPRINT_MS);
    }
    const int32_t speed = (g->hold_ms >= ZG_SPRINT_MS)
                        ? ZG_PLAYER_SPEED * ZG_SPRINT_MUL : ZG_PLAYER_SPEED;
    const int32_t limit = ZG_PX_TO_POS(ZG_FIELD_W - ZG_PLAYER_W);
    g->player_x = clamp32(g->player_x +
                          (g->move_dir * speed * (1 << ZG_POS_SHIFT) * (int32_t)dt) / 1000,
                          0, limit);
}

static void step_gun(zg_game_t *g, uint16_t dt)
{
    // 加速道具的计时跟着枪走:暂停时 zg_step 整帧不进来,所以 20 秒不会被暂停吃掉。
    g->rapid_ms = tick_down(g->rapid_ms, dt);
    g->reload_ms = tick_down(g->reload_ms, dt);
    if (g->reload_ms > 0) return;
    fire(g);
    g->reload_ms = zg_fire_interval_ms(g);
}

static void step_bullets(zg_game_t *g, uint16_t dt)
{
    for (unsigned i = 0; i < ZG_POOL_BULLETS; i++) {
        zg_bullet_t *b = &g->bullets[i];
        if (!b->alive) continue;
        b->x = advance(b->x, b->vx, dt);
        b->y = advance(b->y, b->vy, dt);
        // 出了屏幕或飘出场地左右边界就回收,槽位立刻能给下一发用。
        if (b->y + ZG_PX_TO_POS(ZG_BULLET_H) < 0 || b->x < 0 ||
            b->x > ZG_PX_TO_POS(ZG_FIELD_W)) {
            b->alive = false;
        }
    }
}

static void step_zombies(zg_game_t *g, uint16_t dt)
{
    for (unsigned i = 0; i < ZG_POOL_ZOMBIES; i++) {
        zg_zombie_t *z = &g->zombies[i];
        if (!z->alive) continue;

        z->flash_ms = tick_down(z->flash_ms, dt);

        if (z->telegraph_ms > 0) {
            // 预警期间原地不动(但可以被先手打掉),到点才真的扑下来。
            z->telegraph_ms = tick_down(z->telegraph_ms, dt);
            continue;
        }

        int32_t vy = z->vy;
        if (z->type == ZG_Z_JUMPER) {
            if (z->airborne) {
                vy = z->vy;                       // 腾空段:快,且子弹打不到
                z->hop_ms = tick_down(z->hop_ms, dt);
                if (z->hop_ms == 0) {
                    z->airborne = false;
                    z->wait_ms = ZG_HOP_WAIT_MS;
                }
            } else {
                vy = z->vy / ZG_CRAWL_DIV;        // 蓄力段:慢,这段时间才是能打的窗口
                z->wait_ms = tick_down(z->wait_ms, dt);
                if (z->wait_ms == 0) {
                    z->airborne = true;
                    z->hop_ms = ZG_HOP_MS;
                }
            }
        }

        z->y = advance(z->y, vy, dt);
        z->x = advance(z->x, z->vx, dt);
        const int32_t limit = ZG_PX_TO_POS(ZG_FIELD_W - z->w);
        if (z->x <= 0) { z->x = 0; z->vx = -z->vx; }          // 撞墙反弹
        else if (z->x >= limit) { z->x = limit; z->vx = -z->vx; }

        if (z->y >= ZG_PX_TO_POS(ZG_FIELD_H)) {
            break_through(g, z, true);   // 漏过底边
            continue;
        }
        if (zg_zombie_hits_player(g, z)) {
            break_through(g, z, false);  // 直接撞上豌豆射手
        }
    }
}

static void shoot_zombies(zg_game_t *g)
{
    for (unsigned i = 0; i < ZG_POOL_BULLETS; i++) {
        zg_bullet_t *b = &g->bullets[i];
        if (!b->alive) continue;
        for (unsigned j = 0; j < ZG_POOL_ZOMBIES; j++) {
            zg_zombie_t *z = &g->zombies[j];
            if (!zg_bullet_hits_zombie(b, z)) continue;

            // 伤害可能大于剩余血量,必须用有符号数算再夹回 0:
            // uint8_t 直接相减会绕回 250+,那只僵尸就永远打不死了。
            const int left = (int)z->hp - (int)b->damage;
            z->hp = (left > 0) ? (uint8_t)left : 0;
            z->flash_ms = 90;
            fx_add(g, ZG_FX_HIT, zcx(z), ZG_POS_TO_PX(z->y), FX_TTL_HIT);

            if (z->hp > 0) {
                if (b->pierce_left > 0) { b->pierce_left--; continue; }  // 穿透,继续打下一只
                b->alive = false;
                break;
            }

            // 打死:得分、特效、回收僵尸槽位。分值就是表上那个数,不再随关卡浮动:
            // 关卡本身按 1000 分一档推进,分数必须算得出来,不然"第二关 2000 分"
            // 会变成一句没法核对的话。
            z->alive = false;
            g->kills++;
            g->score += zg_zombie_info(z->type)->score;
            fx_add(g, ZG_FX_DEATH, zcx(z), zcy(z), FX_TTL_DEATH);
            if (g->score > g->best) commit_best(g);   // 破纪录随时标记,结束时要落盘
            // 穿甲弹打死一只不算打完:剩下的穿透次数继续往下攒,才能一排扫掉。
            if (b->pierce_left > 0) { b->pierce_left--; continue; }
            b->alive = false;
            break;
        }
    }
}

// ---- 道具 ----

// 两件道具等概率掉。已经到 4 发顶了还吃到 PEA+ 时不浪费:照样收,只是不再加,
// 所以 UI 上"接到了一个绿色的东西"永远有反馈,不会出现接了没反应的情况。
static void spawn_item(zg_game_t *g)
{
    zg_item_t *slot = NULL;
    for (unsigned i = 0; i < ZG_POOL_ITEMS; i++) {
        if (!g->items[i].alive) { slot = &g->items[i]; break; }
    }
    if (!slot) return;

    const zg_item_kind_t kind = (zg_item_kind_t)(rnd_next(&g->rng) & 1u);
    *slot = (zg_item_t){
        .x = ZG_PX_TO_POS(rnd_below(&g->rng, ZG_FIELD_W - ZG_ITEM_SIZE)),
        .y = -ZG_PX_TO_POS(ZG_ITEM_SIZE),
        .vy = ZG_ITEM_VY,
        .alive = true,
        .kind = kind,
    };
}

// 接到道具:改状态、亮特效;漏过底边只是消失,不扣分——道具是奖励,不该变成负担。
static void step_items(zg_game_t *g, uint16_t dt)
{
    for (unsigned i = 0; i < ZG_POOL_ITEMS; i++) {
        zg_item_t *it = &g->items[i];
        if (!it->alive) continue;
        it->y = advance(it->y, it->vy, dt);

        if (zg_item_hits_player(g, it)) {
            fx_add(g, ZG_FX_PICKUP, (int16_t)(ZG_POS_TO_PX(it->x) + ZG_ITEM_SIZE / 2),
                   (int16_t)(ZG_POS_TO_PX(it->y) + ZG_ITEM_SIZE / 2), FX_TTL_PICKUP);
            if (it->kind == ZG_ITEM_RAPID) {
                g->rapid_ms = ZG_RAPID_MS;   // 重复吃到就重新计时,不叠加成 40 秒
            } else if (zg_volley(g) < ZG_MAX_SHOTS) {
                g->bonus_shots++;
            }
            it->alive = false;
            continue;
        }
        if (it->y >= ZG_PX_TO_POS(ZG_FIELD_H)) it->alive = false;
    }
}

static void step_spawn(zg_game_t *g, uint16_t dt)
{
    // 两个倒计时都照常走,只是横幅期间不许登场:横幅落下那一帧,攒到 0 的
    // 倒计时立刻生效,所以"换关"看得见,也不会被新掉下来的道具打断读字。
    g->spawn_ms = tick_down(g->spawn_ms, dt);
    g->item_ms = tick_down(g->item_ms, dt);
    if (g->levelup_ms > 0) return;

    if (g->spawn_ms == 0) {
        spawn_zombie(g);
        // 第 9 关起有概率连刷两只:前面完全单机,压迫感只留到最后两关。
        if (g->level >= 9 && (rnd_next(&g->rng) & 1u) == 0) spawn_zombie(g);
        g->spawn_ms = zg_spawn_interval_ms(g->level);
    }
    if (g->item_ms == 0) {
        spawn_item(g);
        g->item_ms = zg_item_interval_ms(g->level);
    }
}

void zg_init(zg_game_t *game, uint32_t seed, uint32_t best)
{
    memset(game, 0, sizeof(*game));
    game->phase = ZG_PHASE_READY;
    game->score = 0;
    game->best = best;
    game->hp = ZG_HP_MAX;
    game->lives = ZG_LIVES_MAX;
    game->weapon = ZG_WPN_PEA;
    game->level = 1;
    game->player_x = ZG_PX_TO_POS((ZG_FIELD_W - ZG_PLAYER_W) / 2);
    game->player_y = ZG_PX_TO_POS(ZG_FIELD_H - ZG_PLAYER_H);
    game->ready_ms = ZG_READY_FIRST_MS;
    game->invuln_ms = ZG_INVULN_RESPAWN_MS;
    game->spawn_ms = zg_spawn_interval_ms(1);
    // 第一件道具 8 秒后掉下来:太早玩家还在读按键说明,太晚又整关见不到道具。
    game->item_ms = ZG_ITEM_FIRST_MS;
    // 种子为 0 会让 xorshift 一直输出 0,僵尸全挤在同一个位置,所以换个非零常量。
    game->rng = seed ? seed : 0x9E3779B9u;
}

bool zg_restart(zg_game_t *game)
{
    // GAME OVER 和通关都算"这一局结束了",左右键才是重开;进行中按左右只是走位。
    if (game->phase != ZG_PHASE_OVER && game->phase != ZG_PHASE_WIN) return false;
    const uint32_t best = game->best;
    const uint32_t rng = game->rng;
    const bool dirty = game->best_dirty;
    zg_init(game, rng, best);
    game->best_dirty = dirty;   // 还没落盘的纪录不能因为重开而丢掉
    return true;
}

void zg_set_keys(zg_game_t *game, bool left, bool right)
{
    game->key_left = left;
    game->key_right = right;
    // 左右同时按下按"停"处理,方向键互斥时后按的赢不了,这板子也采不到同按。
    const int8_t dir = (left == right) ? 0 : (right ? 1 : -1);
    // 只有继续同一个方向才算"按住",换方向和松手都从慢档重新起算。
    if (dir != game->move_dir) game->hold_ms = 0;
    game->move_dir = dir;
}

void zg_set_paused(zg_game_t *game, bool paused)
{
    // 结束和通关都已经不算"正在一局",那里不需要暂停,重开会自然清掉。
    game->paused = paused && (game->phase != ZG_PHASE_OVER) &&
                   (game->phase != ZG_PHASE_WIN);
}

void zg_step(zg_game_t *game, uint16_t dt_ms)
{
    if (game->paused || dt_ms < ZG_DT_MIN_MS) return;
    const uint16_t dt = (dt_ms > ZG_DT_MAX_MS) ? ZG_DT_MAX_MS : dt_ms;

    step_fx(game, dt);

    switch (game->phase) {
    case ZG_PHASE_READY:
        game->ready_ms = tick_down(game->ready_ms, dt);
        step_player(game, dt);   // 倒计时里就能先走位,不至于开局被闷在原地
        if (game->ready_ms == 0) {
            game->phase = ZG_PHASE_PLAY;
            game->reload_ms = 0;
            game->spawn_ms = 400;   // 开局留 0.4 秒空场,让玩家先看清自己站在哪
        }
        return;
    case ZG_PHASE_PLAY:
        game->elapsed_ms += dt;
        game->levelup_ms = tick_down(game->levelup_ms, dt);
        step_player(game, dt);
        step_gun(game, dt);
        step_bullets(game, dt);
        step_zombies(game, dt);
        shoot_zombies(game);   // 移动之后再判定,一帧内不会出现"穿过又打中"
        // 分数在这一帧可能刚好跨过一档,所以通关和换关都在判定之后算:
        // 先判通关,不然可能"踏过终点又挨一下"。
        if (game->score >= ZG_WIN_SCORE) {
            game->phase = ZG_PHASE_WIN;
            game->level = ZG_LEVEL_MAX;
            game->levelup_ms = 0;
            clear_field(game);
            commit_best(game);
            return;
        }
        if (zg_level_of_score(game->score) != game->level) {
            // 换关:横幅亮起来,刷怪和掉道具在那几帧按住;枪跟着关卡一起升档。
            game->level = zg_level_of_score(game->score);
            game->levelup_ms = ZG_LEVELUP_MS;
            const zg_weapon_t next = zg_weapon_for_level(game->level);
            if (next != game->weapon) {
                game->weapon = next;
                fx_add(game, ZG_FX_GUNUP, ZG_FIELD_W / 2, ZG_FIELD_H / 3, FX_TTL_GUNUP);
            }
        }
        step_spawn(game, dt);
        step_items(game, dt);
        return;
    case ZG_PHASE_OVER:
    case ZG_PHASE_WIN:
    default:
        return;
    }
}
