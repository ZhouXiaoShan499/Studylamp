/*
 * CAM12 的 UAC 咪头作为 audio_in 的实现 —— M2b，本 PR **未实现**。
 *
 * 为什么先留空壳而不是硬写：usb_stream 组件还没 vendored 进 lamp_av/components/，
 * 也没有 4 处 IDF 5.4.3 补丁（见 docs/42 §3）。在没有组件的工程里写一堆
 * #include <usb_stream.h> 只会编不过。空壳的职责是"诚实地失败"：
 * 让 voice_pipeline 在启动时就知道音频源没起来，而不是拿静音去跑识别、
 * 最后误判成"麦克风坏了"。
 *
 * 补实现时要做的（细节见 docs/43 §6.3）：
 *
 *   1. 把 usb_stream 1.4.0 + 4 处补丁放进 firmware/idf/lamp_av/components/usb_stream/，
 *      并在 main/CMakeLists.txt 的 REQUIRES 里加上 usb_stream；
 *   2. mic_cb 里**只**做 xRingbufferSend（官方注释：cannot block in here!）；
 *   3. 独立任务从环缓冲取数据，按 uac_frame_size_list_get() 报上来的
 *      ch_num / bit_resolution / samples_frequence 转成 16 kHz 单声道 int16：
 *        16k 直接用；32k 做 2:1；48k 做 3:1（先 3 点平均抗混叠）；
 *        多声道只取第 0 声道；24/32 bit 右移到 16 bit；
 *      也可以用 uac_frame_size_reset(STREAM_UAC_MIC, 1, 16, 16000) 主动要 16k，
 *      但必须在流挂起状态下调用；
 *   4. 增益设 1（模块自带音量），不要再乘 i2s_mic.c 里的 ×8。
 *
 * 喇叭方向（M5）：uac_spk_streaming_write() + 播放期间
 * usb_streaming_control(STREAM_UAC_MIC, CTRL_SUSPEND, NULL) 做半双工。
 */
#include <stddef.h>
#include <stdint.h>

#include "audio_impl.h"
#include "esp_log.h"

static const char *TAG = "mic_uac";

esp_err_t uac_mic_start(void)
{
    ESP_LOGE(TAG, "CAM12 UAC mic not implemented yet (M2b)");
    ESP_LOGE(TAG, "vendor usb_stream into components/ first, see docs/43 sec.6.3");
    ESP_LOGE(TAG, "meanwhile switch Kconfig -> audio source = INMP441 (I2S)");
    return ESP_ERR_NOT_SUPPORTED;
}

size_t uac_mic_read(int16_t *pcm, size_t samples, uint32_t timeout_ms)
{
    (void)pcm;
    (void)samples;
    (void)timeout_ms;
    return 0;
}
