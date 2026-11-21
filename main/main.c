// main/main.c —— FoloToy AI Passport 豌豆射手守草坪应用入口。
// 只做板级初始化、按键分发与应用启动:游戏数值与状态机在 main/zombie_game.c,
// 界面与运行任务在 main/app_zombie.c,最高分落盘在 main/zombie_store.c。
#include "app_zombie.h"
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_battery.h"
#include "bsp_pins.h"      // 错误日志里要打印 BSP_LCD_* 引脚号
#include "key_guard.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"

static const char *TAG = "main";

// Waking from deep sleep by key reboots the chip while that key is still held: the
// button component starts a few hundred milliseconds later, registers a press that
// is already down, and grades it a long press once it has been down for its
// threshold, so an unintended action fires almost immediately. Drop key actions
// until that press comes up; the deadline is only a safety net so a stuck key
// cannot leave the pad dead. The state machine lives in key_guard.h so it can be
// unit-tested without the rest of the application.
#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)
static key_guard_t s_key_guard;
static volatile bool s_input_ready;

// Button callbacks run on the shared esp_timer task, so this only hands the event to
// the app layer, which queues it and returns without touching LVGL or the engine.
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    // Consume the guard before the readiness check: a release can arrive after the
    // post-init voltage sample but before the app accepts keys, and dropping it here
    // would leave the guard armed to swallow the next real press.
    const bool was_armed = s_key_guard.armed;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) {
        if (was_armed && !s_key_guard.armed) {
            ESP_LOGI(TAG, "唤醒键已松手,按键恢复响应");
        }
        return;
    }
    if (!s_input_ready) return;
    app_zombie_key(btn, ev);
}

void app_main(void)
{
    ESP_LOGI(TAG, "FoloToy AI Passport 豌豆射手守草坪启动");
    const uint32_t wake_causes = esp_sleep_get_wakeup_causes();
    if (wake_causes & ~(1u << ESP_SLEEP_WAKEUP_UNDEFINED)) {
        ESP_LOGI(TAG, "休眠唤醒原因位图: 0x%02x", (unsigned)wake_causes);
    }
    // 按键唤醒时,那颗键在整个启动期间还按着。先挡住它的动作(见 on_key)。
    if (wake_causes & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
        ESP_LOGI(TAG, "按键唤醒:先屏蔽按键动作,直到那颗键松手");
    }

    bsp_i2c_init();

    // 屏幕是本应用的唯一输出,失败就没有画面可言 —— 打清楚日志后退出。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,游戏无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    // 电量只是右上角的装饰:电池计失败不影响玩法,界面会显示 "--"。
    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "电池初始化失败,右上角电量显示为 --");
    }

    // 游戏自己读取 NVS 里的最高分;读不到只影响纪录,不影响开局。
    if (app_zombie_start() != ESP_OK) {
        ESP_LOGE(TAG, "游戏启动失败,参考上面的显示/NVS 错误日志");
        return;
    }
    s_input_ready = true;

    const esp_err_t button_err = bsp_button_init(on_key, NULL);
    if (button_err != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败: %s", esp_err_to_name(button_err));
        return;
    }
    if (s_key_guard.armed) {
        // 若唤醒键在按键驱动启动前就已松手,组件不会产生 release 事件,守卫会一直
        // 拦到兜底。初始化完成后按真实电压同步一次:已松手立即撤防。
        key_guard_sync_released(&s_key_guard,
                                bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN);
        if (!s_key_guard.armed) {
            ESP_LOGI(TAG, "唤醒键在按键驱动启动前已松手,提前撤防");
        }
    }

    ESP_LOGI(TAG, "就绪:上键左移/下键右移,同一方向按住 0.5 秒后速度翻倍,长按确定键暂停;每 1000 分换一关共 10 关,10000 分通关发奖章");
}
