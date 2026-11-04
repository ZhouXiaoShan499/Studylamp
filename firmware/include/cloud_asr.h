#pragma once
// 云端 ASR：把一段 16 kHz / 16 bit / 单声道 PCM 发出去，换回一句中文文本。
//
// 为什么需要它：板上只有 INMP441。识别的三条路里，离线模块没买、ESP-SR 要迁
// ESP-IDF，只剩"把音频传到云端"。这样板子只负责采音和开关灯，不再需要一台
// 常开的电脑跑 Vosk。
//
// 两家供应商，编译期二选一（LAMP_CLOUD_ASR_PROVIDER）：
//   1 = 百度短语音（默认）：裸 PCM 直传，16 kHz/16 bit/单声道，要 API Key + Secret Key
//   2 = OpenAI /v1/audio/transcriptions：multipart 上传，只要一个 API Key
//
// 凭据存 NVS，串口 `key <API_KEY> [SECRET_KEY]` 写入，不落代码库、不进 git。
// 没有凭据时 ready() 为 false，主固件其余功能（按键、灯、定时）照常工作。

#include <Arduino.h>

#include "config.h"

namespace cloudasr {

// 从 NVS 读凭据并（百度）换一次 token。没有凭据返回 false，但不影响其他功能。
bool begin();

// 凭据齐全且 WiFi 已连上。voice_client 用它决定"要不要开始录音"。
bool ready();

// 串口写入凭据。百度要两个参数，OpenAI 只要第一个。返回 token 是否拿到。
bool setKey(const char* api_key, const char* secret_key);

// 同步上传并识别，失败返回空串。调用期间会阻塞 0.5~2 s，调用方需自行给出反馈。
String transcribe(const int16_t* pcm, size_t samples);

const char* providerName();
void printStatus();

}  // namespace cloudasr
