// main/zombie_store.c
// 纪录各存一个标量,直接用 NVS 的 u32/u8 接口,不需要自定义编码。
// 命名空间与参考示例分开,换固件不会把别人的键读成垃圾。
#include "zombie_store.h"

#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "z_store";
static const char *ZG_NAMESPACE = "zshooter";
static const char *ZG_KEY_BEST = "best";
static const char *ZG_KEY_LEVEL = "level";

bool zombie_store_init(zombie_save_t *save)
{
    save->best = 0;
    save->level = 0;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重建: %s", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s —— 本次分数与最远关卡只留在内存里", esp_err_to_name(err));
        return false;
    }

    nvs_handle_t handle;
    err = nvs_open(ZG_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        uint32_t best = 0;
        if (nvs_get_u32(handle, ZG_KEY_BEST, &best) == ESP_OK) save->best = best;
        uint8_t level = 0;
        // 两个键各自独立读:刷过别的固件、或者只写成功了一半,最坏是回到"没纪录",
        // 不会出现读崩或把垃圾当存档。反正纪录不回放位置,对不上也就只是一行日志。
        if (nvs_get_u8(handle, ZG_KEY_LEVEL, &level) == ESP_OK) save->level = level;
        nvs_close(handle);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "读纪录失败: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "历史最高分 %lu,最远打到第 %u 关(开机仍从第一关开始)",
             (unsigned long)save->best, (unsigned)save->level);
    return true;
}

bool zombie_store_save_record(uint32_t best, uint8_t level)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ZG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open 失败: %s", esp_err_to_name(err));
        return false;
    }
    // 分数与关卡一起提交:两个键描述的是同一局的同一个结论,拆开写会存出对不上的半格。
    err = nvs_set_u32(handle, ZG_KEY_BEST, best);
    if (err == ESP_OK) err = nvs_set_u8(handle, ZG_KEY_LEVEL, level);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写纪录失败: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "已保存纪录:最高 %lu 分,最远第 %u 关", (unsigned long)best, (unsigned)level);
    return true;
}
