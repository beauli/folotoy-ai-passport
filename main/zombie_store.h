// main/zombie_store.h
// 纪录的 NVS 薄封装。读写都是阻塞 Flash 操作,只能在应用任务里调用,
// 不能在按键回调或 LVGL 任务里调。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 开机读回来的一格纪录:历史最高分,加上最远打到第几关。
// 这是纪录不是断点 —— 读回来只用来显示和打日志,下一局照样从第一关开打,
// 所以这里存的 level 为 0 表示这台机器上还没打过。
typedef struct {
    uint32_t best;
    uint8_t level;
} zombie_save_t;

// 初始化 NVS 并读回纪录。字段在失败时全部归零,游戏照常能从第一关玩,
// 只是这一局的纪录不会跨重启保留 —— 返回值告诉调用方要不要打降级日志。
bool zombie_store_init(zombie_save_t *save);

// 落盘纪录:两个键一起写、一起 commit,免得掉电留下"分数是新的、关卡是旧的"那种半格。
// 调用方负责只在真的超过现有纪录时才写(应用层比对内存里的镜像),
// 一局里也就是换关跨过新纪录和打死时各一次,不是每打死一只写一遍。
bool zombie_store_save_record(uint32_t best, uint8_t level);
