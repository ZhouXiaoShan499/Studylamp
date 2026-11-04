#pragma once
// 语音链路：INMP441 -> 16 kHz s16le -> 识别 -> 文本 -> 灯光动作。
//
// 两种传输（编译期互斥，见 config.h 的 LAMP_CLOUD_ASR）：
//   云端（默认）：整段 PCM 交给 cloud_asr，一次 HTTPS 换回文本，本地关键词点灯。
//                 不需要第二台电脑；断网不可用。
//   局域网（旧）：WebSocket 上传 -> PC 的 Vosk ASR -> Ollama -> text reply。
//                 需要一台常开的电脑，适合没有云端 key 的场合。
//
// 模块自己管 VAD 状态机和"什么时候算说完"，上层只管 begin() + tick()。
// WiFi 凭据来自 include/secrets.h；云端 key 来自 NVS（串口 `key` 写入）。

#include <Arduino.h>

#include "config.h"

namespace voice {

void begin();
// Call from loop() as often as possible. Never blocks longer than one frame.
void tick(uint32_t now_ms);

bool connected();
bool listening();
bool monitor();

// Prints the RMS level of every frame (task 1 verification: breath/clap test).
void setMonitor(bool on);

// Records the next utterance even if it is quieter than MIC_VAD_RMS.
void requestListen();

// Sends plain text to the PC (task 2 verification without the mic).
void sendText(const char* text);

void printStatus();

}  // namespace voice