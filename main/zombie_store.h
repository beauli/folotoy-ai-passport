// main/zombie_store.h
// 最高分的 NVS 薄封装。读写都是阻塞 Flash 操作,只能在应用任务里调用,
// 不能在按键回调或 LVGL 任务里调。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 初始化 NVS 并读回历史最高分。*best 在失败时保持 0,游戏照常能玩,
// 只是这一局的纪录不会跨重启保留 —— 返回值告诉调用方要不要打降级日志。
bool zombie_store_init(uint32_t *best);

// 写入新的最高分。只在真正破纪录后被调用,一局游戏最多写一次。
bool zombie_store_save_best(uint32_t best);
