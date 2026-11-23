/*
 * lamp_av —— 唯一的 ESP-IDF 灯固件（docs/42 的固定工程，docs/43 的 M1/M2）。
 *
 * 本文件的职责只是"按顺序把模块点起来"，所有业务都在各自模块里：
 *   lighting.c        双通道 LEDC + Gamma 2.2 + 色温表 + 600 ms 斜坡
 *   lamp_state.c      亮度/色温/预设，唯一写入口
 *   button.c          GPIO4 短按开关、长按预设循环
 *   audio_in.c        音频源抽象（I2S / UAC）
 *   voice_pipeline.c  esp-sr AFE + MultiNet → lamp_state
 *
 * 与 Arduino 固件的一处硬区别：GPIO19/20 归 USB（Host 模式给 CAM12），
 * 所以日志只能走 UART0（COM5），不能再当 USB-Serial-JTAG/CDC 用。
 */
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "nvs_flash.h"

#include "audio_in.h"
#include "board.h"
#include "button.h"
#include "lamp_state.h"
#include "lighting.h"
#include "voice_pipeline.h"

static const char *TAG = "app";

void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    ESP_LOGW(TAG, "=============== lamp_av M1+M2 ===============");
    ESP_LOGW(TAG, "idf=%s chip=S3 rev v%d.%d cores=%d psram=%d KB", esp_get_idf_version(),
             chip.revision / 100, chip.revision % 100, chip.cores,
             (int)(esp_psram_get_size() / 1024));
    ESP_LOGW(TAG, "lamp: cool=GPIO%d warm=GPIO%d | btn=GPIO%d", PIN_LED_COOL, PIN_LED_WARM, PIN_BTN);
    ESP_LOGW(TAG, "audio: %s @ %d Hz mono int16", audio_in_source_name(), AUDIO_IN_RATE);
    ESP_LOGW(TAG, "log: UART0 (USB 19/20 reserved for CAM12)");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs: erase + reinit (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    lighting_init();
    lamp_state_init();
    button_start();

    /* 识别任务自己拉起来：麦克风/模型失败时只打日志，按键与灯照常工作。 */
    voice_pipeline_start();

    ESP_LOGW(TAG, "boot done");
}
