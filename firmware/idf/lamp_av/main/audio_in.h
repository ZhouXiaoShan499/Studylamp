#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * 音频源抽象：全工程只认一种口径 —— 16 kHz / 单声道 / int16 / 小端。
 *
 * 采样率协商、位宽转换、降采样/升采样、声道抽取全部关在各实现内部：
 *   i2s_mic.c  INMP441（I2S，32 bit 槽右移 16）
 *   uac_mic.c  CAM12 的 UAC 咪头（usb_stream，M2b）
 *
 * 为什么要这一层：摄像头（UVC）和咪头（UAC）是同一个 USB 设备上的两个功能，
 * 摄像头不出图并不影响咪头出声（usb_stream 三条流各自独立使能）。把它拆开，
 * "用嘴开灯"就不必等"摄像头能出图"。
 *
 * 业务层（voice_pipeline.c）**不得**包含 <usb_stream.h> 或 <driver/i2s_std.h>。
 */

#define AUDIO_IN_RATE 16000

typedef enum {
    AUDIO_SRC_UAC = 0,
    AUDIO_SRC_I2S,
} audio_src_t;

/* 启动当前 Kconfig 选中的源。失败时返回具体错误，不静默出静音。 */
esp_err_t audio_in_start(void);

/* 阻塞读，凑满 samples 个采样点才返回；返回实际拿到的采样点数（0 = 超时/关断）。 */
size_t audio_in_read(int16_t *pcm, size_t samples, uint32_t timeout_ms);

/* 半双工：TTS 播放期间关掉采集，避免喇叭自己触发自己（M5 用）。 */
void audio_in_set_enabled(bool on);
bool audio_in_enabled(void);

/* 当前源名字，开机日志用（"i2s" / "uac"）。 */
const char *audio_in_source_name(void);
