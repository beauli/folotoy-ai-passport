// main/app_zombie.h
#pragma once

#include "bsp_button.h"
#include "esp_err.h"

// 启动豌豆射手守草坪:读回最高分、自建游戏 UI、拉起游戏任务。
// 按键由 main.c 分发到 app_zombie_key()。
esp_err_t app_zombie_start(void);

// 停止应用:先让游戏任务在时限内退出,再删 LVGL 定时器与页面。
esp_err_t app_zombie_stop(void);

// 按键入口。运行于共享 esp_timer 任务,只做入队,可安全从按键回调调用。
void app_zombie_key(bsp_btn_t btn, bsp_btn_ev_t ev);
