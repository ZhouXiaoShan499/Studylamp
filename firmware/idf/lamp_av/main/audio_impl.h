#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * 音频源实现的内部约定。只有 audio_in.c 用，业务层别包含这个头。
 * 两个实现都必须保证：输出 16 kHz / 单声道 / int16。
 */

/* ---- INMP441（I2S） ---------------------------------------------------- */
esp_err_t i2s_mic_start(void);
size_t i2s_mic_read(int16_t *pcm, size_t samples, uint32_t timeout_ms);

/* ---- CAM12 UAC 咪头（usb_stream，M2b） --------------------------------- */
esp_err_t uac_mic_start(void);
size_t uac_mic_read(int16_t *pcm, size_t samples, uint32_t timeout_ms);
