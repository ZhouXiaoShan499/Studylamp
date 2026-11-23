/*
 * INMP441（标准 I2S 麦，非 PDM）作为 audio_in 的实现。
 *
 * 配置与 firmware/idf/lamp_sr/main/main.c:200-253 的已验证配置一致：
 *   BCLK=GPIO16、WS/LRCLK=GPIO18、SD=GPIO17，L/R 接 GND 选左声道。
 *   INMP441 输出 24 bit、MSB 对齐在 32 bit 槽里，且要求 WS 周期 64 BCLK；
 *   用 16 bit 槽只有 32 BCLK，麦克风会错位输出低频漂移 —— 这两行别改。
 *
 * 前置增益 +18 dB（×8）也是从 lamp_sr 抄来的：裸麦没有 AGC，正常坐姿 50 cm
 * 说话 peak 只有 40~419（约 -38~-58 dBFS），MultiNet 认不出来；补到 ~1200 才够。
 * 必须钳位，否则凑近说话会整数溢出翻转成反向尖峰，识别彻底失效。
 */
#include <stddef.h>
#include <stdint.h>

#include "audio_impl.h"
#include "board.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "mic_i2s";

#define MIC_RATE 16000
#define MIC_PRE_GAIN 8

/* AFE 的 feed chunk 在 16 kHz / 单声道下是 512 点；留一倍余量给未来的双麦。 */
#define RAW_MAX_SAMPLES 2048

/* 每多少次连续空读打一行日志：2 s 一次，不刷屏但能立刻看出"根本没数据"。 */
#define STARVED_LOG_EVERY 100

static i2s_chan_handle_t s_rx = NULL;
static int32_t s_raw[RAW_MAX_SAMPLES];
static uint32_t s_starved = 0;

esp_err_t i2s_mic_start(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 256;

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(err));
        return err;
    }

    /* I2S_STD_*_DEFAULT_CONFIG 是初始化器列表（不是表达式），只能在定义时用。 */
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_MIC_BCLK,
            .ws = PIN_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = PIN_MIC_SD,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    err = i2s_channel_init_std_mode(s_rx, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode: %s", esp_err_to_name(err));
        return err;
    }

    err = i2s_channel_enable(s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "INMP441 %d Hz (BCLK=%d WS=%d SD=%d gain=x%d)", MIC_RATE, PIN_MIC_BCLK, PIN_MIC_WS,
             PIN_MIC_SD, MIC_PRE_GAIN);
    return ESP_OK;
}

size_t i2s_mic_read(int16_t *pcm, size_t samples, uint32_t timeout_ms)
{
    if (s_rx == NULL || pcm == NULL || samples == 0) {
        return 0;
    }
    if (samples > RAW_MAX_SAMPLES) {
        ESP_LOGE(TAG, "asked %u samples > %d", (unsigned)samples, RAW_MAX_SAMPLES);
        return 0;
    }

    size_t got = 0;
    const TickType_t wait = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);
    const esp_err_t err = i2s_channel_read(s_rx, s_raw, samples * sizeof(int32_t), &got, wait);
    if (err != ESP_OK || got == 0) {
        /* "读不到" 与 "读到全 0" 是两回事：
         *   读不到 = I2S 没有时钟/数据（麦克风没接、L/R 悬空、3V3 没上）；
         *   读到全 0 = 时钟在跑但麦没驱动数据线。
         * 分不开就会把接线问题误判成"说话太小声"。 */
        if (s_starved == 0 || (s_starved % STARVED_LOG_EVERY) == 0) {
            ESP_LOGW(TAG, "i2s starved: %s got=%u bytes (consecutive #%u)", esp_err_to_name(err),
                     (unsigned)got, (unsigned)s_starved + 1);
        }
        s_starved++;
        return 0;
    }
    s_starved = 0;
    const size_t n = got / sizeof(int32_t);
    for (size_t i = 0; i < n; ++i) {
        /* 有效位在高 16 bit；同一循环里做前置增益 + 饱和钳位。 */
        int32_t v = (int32_t)(s_raw[i] >> 16) * MIC_PRE_GAIN;
        if (v > 32767) {
            v = 32767;
        } else if (v < -32768) {
            v = -32768;
        }
        pcm[i] = (int16_t)v;
    }
    return n;
}
