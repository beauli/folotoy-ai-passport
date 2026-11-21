// main/zombie_store.c
// 最高分只存一个 u32,直接用 NVS 的标量接口,不需要自定义编码。
// 命名空间与参考示例分开,换固件不会把别人的键读成垃圾。
#include "zombie_store.h"

#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "z_store";
static const char *ZG_NAMESPACE = "zshooter";
static const char *ZG_KEY_BEST = "best";

static esp_err_t nvs_open_ns(nvs_handle_t *handle, nvs_open_mode_t mode)
{
    return nvs_open(ZG_NAMESPACE, mode, handle);
}

bool zombie_store_init(uint32_t *best)
{
    *best = 0;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重建: %s", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s —— 本次最高分只留在内存里", esp_err_to_name(err));
        return false;
    }

    nvs_handle_t handle;
    err = nvs_open_ns(&handle, NVS_READONLY);
    if (err == ESP_OK) {
        uint32_t stored = 0;
        if (nvs_get_u32(handle, ZG_KEY_BEST, &stored) == ESP_OK) *best = stored;
        nvs_close(handle);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "读最高分失败: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "历史最高分 %lu", (unsigned long)*best);
    return true;
}

bool zombie_store_save_best(uint32_t best)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open_ns(&handle, NVS_READWRITE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open 失败: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_set_u32(handle, ZG_KEY_BEST, best);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写最高分失败: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "已保存最高分 %lu", (unsigned long)best);
    return true;
}
