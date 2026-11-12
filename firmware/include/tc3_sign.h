#pragma once
// 腾讯云 API 3.0 的 TC3-HMAC-SHA256 签名（纯函数，不碰 WiFi、不碰 NVS）。
//
// 为什么单独成文件：签名对不对和网络通不通是两类问题，混在一起排查最费时间
// （docs/13 记录的九个坑里有五个都是这么来的）。这个模块只有纯计算，
// 用 selfTest() 跑官方 CVM 向量就能证明算法正确，不需要 Key、不需要联网。
//
// 三个必踩点（照抄 docs/14 §5.1，别凭记忆改）：
//   1. date 必须由 X-TC-Timestamp 按 UTC+0 推，不能用本地时区；
//   2. canonical headers 里 x-tc-action 的「值」是小写，实际发送的 X-TC-Action
//      头是驼峰 —— 这是设计如此，不是笔误；
//   3. content-type 签的是 "application/json; charset=utf-8"，发送时必须一字不差，
//      被任何一层 HTTP 封装改写都会报 AuthFailure.SignatureFailure。

#include <Arduino.h>

namespace tc3 {

// 签名时约定的 Content-Type。签名和发送必须共用这一个常量。
extern const char* const kContentType;

// 只算签名（对应官方文档里的 Signature 段，也是 Python 版 sign_tc3 的同名函数）。
//   date     : yyyy-mm-dd，UTC+0
//   service  : "asr" / "tts" / "cvm"
String signatureHex(const char* secret_key, const char* date, const char* service,
                    const String& string_to_sign);

// 拼出完整的 Authorization 头。
//   host        : asr.tencentcloudapi.com（不含 https://）
//   action_lower: sentencerecognition（全小写，进规范头）
//   body/len    : 即将发送的原始请求体，签名算的是同一份字节
//   ts_utc      : X-TC-Timestamp，与签名的 date 必须同源
//   out_canonical: 可选，回传 CanonicalRequest，方便和 API Explorer 逐字节对照
String authorization(const char* secret_id, const char* secret_key, const char* host,
                     const char* service, const char* action_lower, const uint8_t* body,
                     size_t body_len, uint32_t ts_utc, String* out_canonical = nullptr);

// 用官方 CVM 示例向量自检，不联网、不需要 Key。
// 返回 true 表示拼接顺序与官方 SDK 逐字节一致。
bool selfTest();

// 串口打一行摘要，给 `asr selftest` 用。
void printSelfTest();

}  // namespace tc3
