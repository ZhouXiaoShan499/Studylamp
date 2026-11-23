#pragma once

/*
 * 离线命令词识别（esp-sr AFE + MultiNet）→ lamp_state。
 *
 * 链路：audio_in（16 kHz 单声道 int16）→ AFE（降噪/VAD）→ MultiNet → 意图 → 灯。
 * 不依赖网络、不依赖电脑、不花钱。
 *
 * 识别部分直接沿用 firmware/idf/lamp_sr/main/main.c 的已验证结构（3/3），
 * 只把"取数据"从 i2s_channel_read 换成 audio_in_read。
 */
void voice_pipeline_start(void);
