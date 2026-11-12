#include "cloud_asr.h"

#if !LAMP_CLOUD_ASR

// 云端识别关闭：全部空实现，voice_client 走回原来的局域网 Vosk 路径。
namespace cloudasr {
bool begin() { return false; }
bool ready() { return false; }
bool setKey(const char*, const char*, const char*) { return false; }
String transcribe(const int16_t*, size_t) { return String(); }
const char* providerName() { return "off"; }
void printStatus() { Serial.println("[asr] cloud ASR disabled (LAMP_CLOUD_ASR=0)"); }
}  // namespace cloudasr

#else

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "local_intent.h"

#if LAMP_BACKEND_ALIYUN || LAMP_BACKEND_TENCENT
#include <mbedtls/base64.h>
#endif

#if LAMP_BACKEND_TENCENT
#include <time.h>

#include "tc3_sign.h"
#endif

namespace cloudasr {
namespace {

constexpr const char* kNvsNamespace = "cloudasr";

// 百度只在 provider 1/2 用：1 是转文字后本地匹配关键词，2 是转文字后再交 LLM。
// 这里必须用肯定式判断：早期写的是 `!= 3`，一加 provider=4 就为真，
// 百度的两个函数会被重新编进来（docs/13 坑 1 / docs/14 §2 都点了这一条）。
#if LAMP_BACKEND_BAIDU
constexpr const char* kBaiduTokenUrl = "https://aip.baidubce.com/oauth/2.0/token";
constexpr const char* kBaiduAsrUrl = "https://vop.baidu.com/server_api";
#endif

Preferences sPrefs;
String sApiKey;      // provider 1/2 = 百度 API Key；3 = 百炼 Key；4 = 腾讯 SecretId
String sSecretKey;   // provider 1/2 = 百度 Secret Key；4 = 腾讯 SecretKey
#if LAMP_BACKEND_LLM
// 第二跳的独立 Key（腾讯云 TokenHub 的 sk-...）。空 = 沿用 ASR 那把 Key，
// 也就是 provider=3 一个百炼 Key 打两跳的老行为。
String sLlmKey;
#endif
#if LAMP_BACKEND_BAIDU
String sToken;          // 百度的 access_token，30 天有效
uint32_t sTokenAtMs = 0;
#endif
bool sBegun = false;

bool hasKeys() { return !sApiKey.isEmpty(); }

// 云端返回的问号/句号对关键词匹配没意义，先清掉。
String cleanText(String text) {
    text.trim();
    text.replace("。", "");
    text.replace("，", "");
    text.replace("！", "");
    text.replace("？", "");
    text.replace("、", "");
    text.replace(" ", "");
    text.trim();
    return text;
}

// ---------------------------------------------------------------------------
// 百度短语音：先换 token（有效期 30 天），再用 RAW 方式把裸 PCM 当 body 传。
// 不用 JSON + base64，省掉 1/3 的内存膨胀。
// provider=2 时这一步仍然执行：国产 LLM 不做语音转文字，先由百度把声音变成字。
// ---------------------------------------------------------------------------
#if LAMP_BACKEND_BAIDU

bool fetchToken() {
    if (sApiKey.isEmpty() || sSecretKey.isEmpty()) {
        Serial.println("[asr] no key: send `key <API_KEY> <SECRET_KEY>` over serial");
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(CLOUD_ASR_TIMEOUT_MS);
    http.setConnectTimeout(CLOUD_ASR_TIMEOUT_MS);

    const String url = String(kBaiduTokenUrl) + "?grant_type=client_credentials&client_id=" + sApiKey +
                       "&client_secret=" + sSecretKey;
    if (!http.begin(client, url)) {
        Serial.println("[asr] token: begin failed");
        return false;
    }
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    const int code = http.POST(reinterpret_cast<uint8_t*>(const_cast<char*>("")), 0);
    if (code != HTTP_CODE_OK) {
        Serial.printf("[asr] token: http %d\n", code);
        http.end();
        return false;
    }
    const String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) {
        Serial.println("[asr] token: bad json (检查 API Key / Secret Key)");
        return false;
    }
    const char* token = doc["access_token"] | "";
    if (token[0] == '\0') {
        const char* reason = doc["error_description"] | "";
        if (reason[0] == '\0') reason = doc["error"] | "unknown";
        Serial.printf("[asr] token failed: %s\n", reason);
        return false;
    }

    sToken = token;
    sTokenAtMs = millis();
    Serial.printf("[asr] token ok (%lu s valid)\n", static_cast<unsigned long>(doc["expires_in"] | 0L));
    return true;
}

String recognizeOnce(const int16_t* pcm, size_t samples) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(CLOUD_ASR_TIMEOUT_MS);
    http.setConnectTimeout(CLOUD_ASR_TIMEOUT_MS);

    const String url = String(kBaiduAsrUrl) + "?cuid=" + CLOUD_ASR_CUID + "&token=" + sToken +
                       "&dev_pid=" + String(CLOUD_ASR_DEV_PID);
    if (!http.begin(client, url)) {
        Serial.println("[asr] asr: begin failed");
        return String();
    }
    http.addHeader("Content-Type", "audio/pcm;rate=16000");
    const int code = http.POST(reinterpret_cast<uint8_t*>(const_cast<int16_t*>(pcm)),
                               samples * sizeof(int16_t));
    if (code != HTTP_CODE_OK) {
        Serial.printf("[asr] asr: http %d\n", code);
        http.end();
        return String();
    }
    const String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) {
        Serial.println("[asr] asr: bad json");
        return String();
    }

    const int errNo = doc["err_no"] | -1;
    if (errNo != 0) {
        const char* message = doc["err_msg"] | "";
        Serial.printf("[asr] err_no=%d %s\n", errNo, message);
        // 111/3302 = token 无效或过期，下次调用前重取
        if (errNo == 111 || errNo == 3302) sToken = "";
        return String();
    }
    return cleanText(String(doc["result"][0] | ""));
}

#endif  // LAMP_BACKEND_BAIDU

#if LAMP_BACKEND_LLM

// ---------------------------------------------------------------------------
// 一次 /chat/completions：把识别出的文字交给 LLM，让它只回一个动作标签。
// 没有喇叭，LLM 不需要回一句完整的话，只回一个词，延迟和成本都压到最低。
//   system 提示词约定：只允许回 on / off / up / down / max / night / study / rest / none
// key/host/path/model 由调用方给：provider=2 直连各家，provider=3 打百炼兼容模式，
// 填了腾讯模型 Key 则打腾讯 TokenHub —— 三者的差别只有这四个参数，鉴权都是 Bearer。
//
// disable_thinking 只对推理型模型（腾讯 DeepSeek-V4.1-Flash 等）有意义：
// 这类模型默认先吐 reasoning，max_tokens 给小了会被思考过程吃光、content 返回空，
// 表现是「灯不动、串口只有一对空引号」。关掉思考后实测 1.6 s、内容 1~2 个 token。
// ---------------------------------------------------------------------------
String postChatIntent(const String& text, const char* key, const char* host, const char* path,
                      const char* model, uint16_t timeout_ms, bool disable_thinking) {
    if (text.isEmpty()) return String();
    if (key == nullptr || key[0] == '\0') {
        Serial.println("[llm] no key, dropped");
        return String();
    }

    JsonDocument req;
    req["model"] = String(model);
    JsonArray messages = req["messages"].to<JsonArray>();
    JsonObject sys = messages.add<JsonObject>();
    sys["role"] = "system";
    sys["content"] =
        "你是桌面机器人 LAMP-1 的中枢，分两种情况回："
        "① 用户这句话是在控制台灯：只回一个英文标签，不要标点、不要解释——"
        "开灯=on，关灯=off，调亮/亮一点=up，调暗/暗一点=down，最亮=max，夜灯=night，"
        "学习模式=study，休息模式=rest。"
        "② 其它任何话：用简体中文口语回答，最多两句话，并以 ANSWER: 开头。";
    JsonObject usr = messages.add<JsonObject>();
    usr["role"] = "user";
    usr["content"] = text;
    req["temperature"] = 0;
    // 关掉思考后要留够"回答一句话"的额度；只判标签时 8 个 token 就够。
    req["max_tokens"] = disable_thinking ? 200 : 8;
    if (disable_thinking) req["thinking"]["type"] = "disabled";

    String body;
    serializeJson(req, body);

    // 这里必须用 HTTPClient 而不是手写 socket：响应可能是 chunked，
    // 手写的「取 \r\n\r\n 之后全部字节」会把分块长度行当成 JSON 的一部分。
    WiFiClientSecure client;
    client.setInsecure();  // 自用台架；产品化换根证书
    HTTPClient http;
    http.setTimeout(timeout_ms);
    http.setConnectTimeout(timeout_ms);
    if (!http.begin(client, String("https://") + host + path)) {
        Serial.println("[llm] begin failed");
        return String();
    }
    http.addHeader("Content-Type", "application/json; charset=utf-8");
    http.addHeader("Authorization", String("Bearer ") + key);

    const uint32_t t_http = millis();
    const int code = http.POST(reinterpret_cast<uint8_t*>(body.begin()), body.length());
    if (code != HTTP_CODE_OK) {
        const String err = http.getString();
        Serial.printf("[llm] http %d %.200s\n", code, err.c_str());
        http.end();
        return String();
    }
    const String reply = http.getString();
    http.end();
    Serial.printf("[llm] reply %u B in %lu ms\n", static_cast<unsigned>(reply.length()),
                  static_cast<unsigned long>(millis() - t_http));

    JsonDocument doc;
    if (deserializeJson(doc, reply) != DeserializationError::Ok) {
        Serial.printf("[llm] bad json %.160s\n", reply.c_str());
        return String();
    }
    if (!doc["error"].isNull()) {
        const char* message = doc["error"]["message"] | "unknown";
        Serial.printf("[llm] error: %s\n", message);
        return String();
    }
    return cleanText(String(doc["choices"][0]["message"]["content"] | ""));
}

// 第二跳的 Key 与落点：填了腾讯模型 Key 就整跳改走腾讯，否则沿用 ASR 那把。
bool llmGoesToTencent() { return !sLlmKey.isEmpty(); }
const char* llmKey() { return llmGoesToTencent() ? sLlmKey.c_str() : sApiKey.c_str(); }

#if LAMP_BACKEND_TENCENT
// provider=4：听和想是两套凭据。这一跳只认 TokenHub 的 sk-，
// 绝不能拿 SecretId 去当 Bearer —— 那会得到 401002，看着像 Key 填错了，
// 实际是拿错了一把钥匙。
String classifyIntent(const String& text) {
    if (sLlmKey.isEmpty()) {
        Serial.println("[llm] 缺 TokenHub Key：串口 `key <SecretId> <SecretKey> <sk-...>`");
        return String();
    }
    return postChatIntent(text, sLlmKey.c_str(), LAMP_TENCENT_LLM_HOST, LAMP_TENCENT_LLM_PATH,
                          LAMP_TENCENT_LLM_MODEL, CLOUD_TC_LLM_TIMEOUT_MS, true);
}
#elif LAMP_BACKEND_ALIYUN
// provider=3：ASR 打百炼；语义这一跳默认也是百炼（同一个 Key、同一个域名），
// 填了 LAMP_CLOUD_LLM_KEY 就只把第二跳换成腾讯 DeepSeek，ASR 一动不动。
String classifyIntent(const String& text) {
    if (llmGoesToTencent()) {
        return postChatIntent(text, llmKey(), LAMP_TENCENT_LLM_HOST, LAMP_TENCENT_LLM_PATH,
                              LAMP_TENCENT_LLM_MODEL, CLOUD_BAILIAN_TIMEOUT_MS, true);
    }
    return postChatIntent(text, llmKey(), CLOUD_BAILIAN_HOST, CLOUD_BAILIAN_PATH,
                          CLOUD_BAILIAN_LLM_MODEL, CLOUD_BAILIAN_TIMEOUT_MS, false);
}
#else
// provider=2：ASR 仍由百度负责，语义这一跳默认直连各家 OpenAI 兼容端点，
// 填了 LAMP_CLOUD_LLM_KEY 就走腾讯。
String classifyIntent(const String& text) {
    if (llmGoesToTencent()) {
        return postChatIntent(text, llmKey(), LAMP_TENCENT_LLM_HOST, LAMP_TENCENT_LLM_PATH,
                              LAMP_TENCENT_LLM_MODEL, CLOUD_ASR_TIMEOUT_MS, true);
    }
    return postChatIntent(text, llmKey(), CLOUD_LLM_HOST, CLOUD_LLM_PATH, CLOUD_LLM_MODEL,
                          CLOUD_ASR_TIMEOUT_MS, false);
}
#endif

// ---------------------------------------------------------------------------
// 把 LLM 的返回统一成两种形态：
//   "on"/"off"/...  —— 灯的命令标签（本地词表没接住的近义说法）
//   "ANSWER:..."    —— 自由对话的回答（没有喇叭，这句会打到串口/网页上）
// 模型偶尔不守格式，按"整句等于标签"来判定，宁可当对话也不误开灯。
// ---------------------------------------------------------------------------
#if LAMP_BACKEND_LLM
String normalizeLlmReply(const String& raw) {
    String s = raw;
    s.trim();
    if (s.isEmpty()) return s;

    String lower = s;
    lower.toLowerCase();
    if (lower == "on" || lower == "off" || lower == "up" || lower == "down" ||
        lower == "max" || lower == "night" || lower == "study" || lower == "rest" ||
        lower == "none") {
        return lower;
    }
    if (lower.startsWith("answer:")) {
        return "ANSWER:" + s.substring(7);
    }
    return "ANSWER:" + s;
}
#endif

#endif  // LAMP_BACKEND_LLM

// ---------------------------------------------------------------------------
// WAV / 大缓冲：百炼和腾讯都要「完整 WAV 再 base64」，两家共用这两个工具。
// 2 s / 16 kHz / 单声道 = 64044 B，base64 后 84~86 KB，必须走 PSRAM。
// ---------------------------------------------------------------------------
#if LAMP_BACKEND_ALIYUN || LAMP_BACKEND_TENCENT
uint8_t* allocBig(size_t bytes) {
    uint8_t* p = static_cast<uint8_t*>(ps_malloc(bytes));
    if (p == nullptr) p = static_cast<uint8_t*>(malloc(bytes));
    return p;
}

void writeWavHeader(uint8_t* h, size_t pcm_bytes) {
    const uint32_t sample_rate = MIC_SAMPLE_RATE;
    const uint16_t channels = 1;
    const uint16_t bits = 16;
    const uint32_t byte_rate = sample_rate * channels * bits / 8;
    const uint16_t block_align = channels * bits / 8;
    const uint32_t riff_size = 36 + static_cast<uint32_t>(pcm_bytes);
    const uint32_t fmt_size = 16;
    const uint16_t fmt_pcm = 1;
    const uint32_t data_size = static_cast<uint32_t>(pcm_bytes);

    memcpy(h + 0, "RIFF", 4);
    memcpy(h + 4, &riff_size, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    memcpy(h + 16, &fmt_size, 4);
    memcpy(h + 20, &fmt_pcm, 2);
    memcpy(h + 22, &channels, 2);
    memcpy(h + 24, &sample_rate, 4);
    memcpy(h + 28, &byte_rate, 4);
    memcpy(h + 32, &block_align, 2);
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data_size, 4);
}
#endif

#if LAMP_BACKEND_ALIYUN

// ---------------------------------------------------------------------------
// 阿里云百炼：qwen3-asr-flash 的 OpenAI 兼容接口。
// 和百度最大的差别是它不吃裸 PCM——必须给完整 WAV（含 44 字节头）再 base64。
// 2 s / 16 kHz / 单声道 = 64044 B，base64 后约 85 KB，整段 JSON 一次 POST，
// 同步返回 choices[0].message.content。走 PSRAM，不占内部 RAM。
// ---------------------------------------------------------------------------
String transcribeBailian(const int16_t* pcm, size_t samples) {
    const size_t pcm_bytes = samples * sizeof(int16_t);
    const size_t wav_bytes = 44 + pcm_bytes;

    uint8_t* wav = allocBig(wav_bytes);
    if (wav == nullptr) {
        Serial.printf("[asr] oom: wav %u B\n", static_cast<unsigned>(wav_bytes));
        return String();
    }
    writeWavHeader(wav, pcm_bytes);
    memcpy(wav + 44, pcm, pcm_bytes);

    char head[224];
    const int head_len = snprintf(head, sizeof(head),
                                  "{\"model\":\"%s\",\"stream\":false,"
                                  "\"asr_options\":{\"language\":\"zh\",\"enable_itn\":false},"
                                  "\"messages\":[{\"role\":\"user\",\"content\":["
                                  "{\"type\":\"input_audio\",\"input_audio\":"
                                  "{\"data\":\"data:audio/wav;base64,",
                                  CLOUD_BAILIAN_ASR_MODEL);
    constexpr const char* kTail = "\"}}]}]}";
    const size_t tail_len = strlen(kTail);
    // mbedtls_base64_encode 默认每 76 字符插一个换行符 '\n'（RFC 4648 换行），
    // 所以输出会比纯 base64 的 4*(n/3) 多出 n/57 左右的换行符。
    // 官方容量公式：4*((n+2)/3) + 每 76 字符一个换行。这里直接多留 2% 余量最稳。
    const size_t b64_cap = 4 * ((wav_bytes + 2) / 3) + (wav_bytes / 57) + 16;
    const size_t body_cap = static_cast<size_t>(head_len) + b64_cap + tail_len;

    uint8_t* body = allocBig(body_cap);
    if (body == nullptr) {
        Serial.printf("[asr] oom: body %u B\n", static_cast<unsigned>(body_cap));
        free(wav);
        return String();
    }

    memcpy(body, head, static_cast<size_t>(head_len));
    size_t b64_len = 0;
    const uint32_t t_b64 = millis();
    const int rc = mbedtls_base64_encode(body + head_len, b64_cap, &b64_len, wav, wav_bytes);
    free(wav);
    if (rc != 0) {
        Serial.printf("[asr] base64 failed rc=%d\n", rc);
        free(body);
        return String();
    }
    memcpy(body + head_len + b64_len, kTail, tail_len);
    const size_t body_len = static_cast<size_t>(head_len) + b64_len + tail_len;
    Serial.printf("[asr] wav %u B -> body %u B (b64 %lu ms)\n", static_cast<unsigned>(wav_bytes),
                  static_cast<unsigned>(body_len),
                  static_cast<unsigned long>(millis() - t_b64));

    const String url = String("https://") + CLOUD_BAILIAN_HOST + CLOUD_BAILIAN_PATH;
    WiFiClientSecure client;
    client.setInsecure();  // 自用台架；产品化换根证书
    HTTPClient http;
    http.setTimeout(CLOUD_BAILIAN_TIMEOUT_MS);
    http.setConnectTimeout(CLOUD_BAILIAN_TIMEOUT_MS);
    if (!http.begin(client, url)) {
        Serial.println("[asr] asr: begin failed");
        free(body);
        return String();
    }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + sApiKey);

    const uint32_t t_http = millis();
    const int code = http.POST(body, body_len);
    free(body);
    if (code != HTTP_CODE_OK) {
        const String err = http.getString();
        Serial.printf("[asr] asr: http %d %.160s\n", code, err.c_str());
        http.end();
        return String();
    }
    const String reply = http.getString();
    http.end();
    Serial.printf("[asr] asr: %u B in %lu ms\n", static_cast<unsigned>(reply.length()),
                  static_cast<unsigned long>(millis() - t_http));

    JsonDocument doc;
    if (deserializeJson(doc, reply) != DeserializationError::Ok) {
        Serial.println("[asr] asr: bad json");
        return String();
    }
    if (!doc["error"].isNull()) {
        const char* message = doc["error"]["message"] | "unknown";
        Serial.printf("[asr] asr error: %s\n", message);
        return String();
    }

    // OpenAI 兼容模式下 content 是字符串；DashScope 原生协议下是数组取 text。
    // 这里两种都收，免得上游换协议直接哑掉。
    JsonVariant content = doc["choices"][0]["message"]["content"];
    String text;
    if (content.is<JsonArray>()) {
        text = content[0]["text"] | "";
    } else if (!content.isNull()) {
        text = content.as<String>();
    }
    return cleanText(text);
}

#endif  // LAMP_BACKEND_ALIYUN

#if LAMP_BACKEND_TENCENT

// ---------------------------------------------------------------------------
// 腾讯云一句话识别：SentenceRecognition / 2019-06-14
// 和百炼的硬差别有三处，全部会以「看起来很怪」的方式失败：
//   1. base64 后**不能带任何换行**（mbedtls 默认每 76 字符插 \n），
//      不压平会报 InvalidParameterValue.ErrorInvalidVoicedata，像「音频坏了」；
//   2. 请求要带 TC3 签名，签名里含时间戳，必须先对时（±5 分钟窗口）；
//   3. 签名用的 Content-Type 必须与实发的一字不差，被 HTTP 层改写就 SignatureFailure。
// ---------------------------------------------------------------------------

// 低于门槛说明还没对时。此时发请求必然 SignatureExpire，
// 宁可打一行明确日志，也不发一个注定失败的请求。
bool clockReady() {
    return static_cast<uint32_t>(time(nullptr)) >= CLOCK_MIN_VALID_EPOCH;
}

bool sClockKicked = false;
uint32_t sClockKickAtMs = 0;

// 把 NTP 请求踢出去（不阻塞）。成功后 time() 才有值。
void kickClockSync() {
    if (clockReady()) return;
    if (sClockKicked && millis() - sClockKickAtMs < 10000) return;
    sClockKicked = true;
    sClockKickAtMs = millis();
    configTime(0, 0, "ntp.tencent.com", "ntp.aliyun.com", "cn.pool.ntp.org");
    Serial.println("[asr] 已发 SNTP 对时请求（TC3 要求 ±5 分钟）");
}

bool waitClock(uint16_t timeout_ms) {
    if (clockReady()) return true;
    kickClockSync();
    const uint32_t deadline = millis() + timeout_ms;
    while (millis() < deadline) {
        delay(50);
        if (clockReady()) {
            Serial.printf("[asr] 对时成功 epoch=%lu\n", static_cast<unsigned long>(time(nullptr)));
            return true;
        }
    }
    Serial.println("[asr] 对时失败：串口敲 `time <epoch>` 手填，或检查 NTP 出网");
    return false;
}

// 原地压掉 \r\n。腾讯云明确要求「编码后不带回车换行」。
void flattenBase64(uint8_t* buf, size_t* len) {
    size_t w = 0;
    for (size_t r = 0; r < *len; ++r) {
        const char c = static_cast<char>(buf[r]);
        if (c == '\n' || c == '\r') continue;
        buf[w++] = buf[r];
    }
    *len = w;
    buf[w] = '\0';
}

// 腾讯云的错误码是可检索的枚举，比「一句话错误」好排查得多，直接建表。
void printTcHint(const String& code) {
    if (code == "AuthFailure.SignatureExpire") {
        Serial.println("  → 板子时间偏了：重新对时，或串口 `time <epoch>` 手填");
    } else if (code == "AuthFailure.SignatureFailure") {
        Serial.println("  → 签名串与实发请求不一致：Content-Type 被改写 / 头部不一致");
    } else if (code == "AuthFailure.SecretIdNotFound") {
        Serial.println("  → SecretId 无效：NVS 里可能还留着旧厂商的 Key（docs/26 §5.2）");
    } else if (code == "FailedOperation.UserNotRegistered") {
        Serial.println("  → 控制台没开通「语音识别」，Key 再对也调不通");
    } else if (code == "FailedOperation.ServiceIsolate") {
        Serial.println("  → 账号欠费停服，充值后 1~2 分钟恢复");
    } else if (code.indexOf("InvalidParameterValue.ErrorInvalidVoicedata") >= 0) {
        Serial.println("  → 音频不合法：base64 里带了换行，或 WAV 头 / 采样率不对");
    } else if (code == "InvalidParameterValue.ErrorNoVoice") {
        Serial.println("  → 音频里没检测到有效人声");
    }
}

String transcribeTencent(const int16_t* pcm, size_t samples) {
    // 签名强依赖时钟，这一步不过就别发请求。
    if (!waitClock(3000)) return String();

    const size_t pcm_bytes = samples * sizeof(int16_t);
    const size_t wav_bytes = 44 + pcm_bytes;

    uint8_t* wav = allocBig(wav_bytes);
    if (wav == nullptr) {
        Serial.printf("[asr] oom: wav %u B\n", static_cast<unsigned>(wav_bytes));
        return String();
    }
    writeWavHeader(wav, pcm_bytes);
    memcpy(wav + 44, pcm, pcm_bytes);

    // 请求体 = 头部 JSON + 一段 base64 + 收尾。base64 直接写进 body，
    // 少一次「先编码到临时缓冲再拼」的大块拷贝（2 s 音频约 85 KB）。
    // 容量公式必须把 mbedtls 每 76 字符插的换行算进去：4*(n/3) 只是纯 base64，
    // 少算换行会直接返回 rc=-42（BUFFER_TOO_SMALL），这就是 docs/13 坑 5。
    const size_t head_cap = 512;
    const size_t b64_cap = 4 * ((wav_bytes + 2) / 3) + (wav_bytes / 57) + 16;
    const size_t body_cap = head_cap + b64_cap + 8;

    uint8_t* body = allocBig(body_cap);
    if (body == nullptr) {
        Serial.printf("[asr] oom: body %u B\n", static_cast<unsigned>(body_cap));
        free(wav);
        return String();
    }

    // UsrAudioKey 是必填项（调用者身份），只是在设备上没业务含义，给个随机串即可。
    char usr_key[33];
    snprintf(usr_key, sizeof(usr_key), "%08lx%08lx%08lx%08lx",
             static_cast<unsigned long>(millis()), static_cast<unsigned long>(random(0x7FFFFFFF)),
             static_cast<unsigned long>(random(0x7FFFFFFF)),
             static_cast<unsigned long>(random(0x7FFFFFFF)));

    // DataLen 是**原始音频**的字节数，含 44 字节 WAV 头 —— 和 Data 解出来的字节数一致。
    // 填成裸 PCM 长度（少 44）在控制台看起来没差，但属于自相矛盾的请求。
    const int head_len = snprintf(
        reinterpret_cast<char*>(body), head_cap,
        "{\"ProjectId\":0,\"SubServiceType\":2,\"EngSerViceType\":\"%s\",\"SourceType\":1,"
        "\"VoiceFormat\":\"wav\",\"UsrAudioKey\":\"%s\",\"DataLen\":%u,\"FilterPunc\":%u,"
        "\"HotwordList\":\"%s\",\"Data\":\"",
        CLOUD_TC_ASR_ENGINE, usr_key, static_cast<unsigned>(wav_bytes),
        static_cast<unsigned>(CLOUD_TC_FILTER_PUNC), CLOUD_TC_HOTWORDS);
    if (head_len <= 0 || static_cast<size_t>(head_len) >= head_cap) {
        Serial.println("[asr] head too long");
        free(wav);
        free(body);
        return String();
    }

    size_t b64_len = 0;
    const uint32_t t_b64 = millis();
    const int rc = mbedtls_base64_encode(body + head_len, b64_cap, &b64_len, wav, wav_bytes);
    free(wav);
    if (rc != 0) {
        Serial.printf("[asr] base64 failed rc=%d\n", rc);
        free(body);
        return String();
    }
    flattenBase64(body + head_len, &b64_len);
    body[head_len + b64_len] = '"';
    body[head_len + b64_len + 1] = '}';
    const size_t body_len = static_cast<size_t>(head_len) + b64_len + 2;
    Serial.printf("[asr] wav %u B -> body %u B (b64 %lu ms)\n", static_cast<unsigned>(wav_bytes),
                  static_cast<unsigned>(body_len),
                  static_cast<unsigned long>(millis() - t_b64));

    // 时间戳与签名的 date 必须同源，这里只取一次。
    const uint32_t ts = static_cast<uint32_t>(time(nullptr));
    String canonical;
    const String auth = tc3::authorization(sApiKey.c_str(), sSecretKey.c_str(), CLOUD_TC_ASR_HOST,
                                           CLOUD_TC_ASR_SERVICE, "sentencerecognition", body,
                                           body_len, ts, &canonical);

    WiFiClientSecure client;
    client.setInsecure();  // 自用台架；产品化换根证书
    HTTPClient http;
    http.setTimeout(CLOUD_TC_ASR_TIMEOUT_MS);
    http.setConnectTimeout(CLOUD_TC_ASR_TIMEOUT_MS);
    if (!http.begin(client, String("https://") + CLOUD_TC_ASR_HOST + "/")) {
        Serial.println("[asr] begin failed");
        free(body);
        return String();
    }
    // 这一行必须是同一个常量：签名算的是它，发送也得是它，一字不差。
    http.addHeader("Content-Type", tc3::kContentType);
    http.addHeader("Authorization", auth);
    http.addHeader("X-TC-Action", CLOUD_TC_ASR_ACTION);
    http.addHeader("X-TC-Version", CLOUD_TC_ASR_VERSION);
    http.addHeader("X-TC-Timestamp", String(ts));

    const uint32_t t_http = millis();
    const int code = http.POST(body, body_len);
    free(body);
    if (code != HTTP_CODE_OK) {
        const String err = http.getString();
        Serial.printf("[asr] http %d %.200s\n", code, err.c_str());
        http.end();
        return String();
    }
    const String reply = http.getString();
    http.end();
    Serial.printf("[asr] reply %u B in %lu ms\n", static_cast<unsigned>(reply.length()),
                  static_cast<unsigned long>(millis() - t_http));

    JsonDocument doc;
    if (deserializeJson(doc, reply) != DeserializationError::Ok) {
        Serial.printf("[asr] bad json %.160s\n", reply.c_str());
        return String();
    }

    JsonObject resp = doc["Response"];
    if (!resp["Error"].isNull()) {
        const String ec = String(resp["Error"]["Code"] | "?");
        const String em = String(resp["Error"]["Message"] | "");
        Serial.printf("[asr] %s %s\n", ec.c_str(), em.c_str());
        printTcHint(ec);
        // 签名过期是时钟问题而不是逻辑问题：放开对时闸，下次调用重试一次。
        if (ec == "AuthFailure.SignatureExpire") sClockKicked = false;
        return String();
    }
    return cleanText(String(resp["Result"] | ""));
}

#endif  // LAMP_BACKEND_TENCENT

}  // namespace

// ---------------------------------------------------------------------------
// 一段文本的统一路由：先本地词表（固定命令，<1 ms），没命中才问 LLM。
// 真正的 ASR 结果和串口 `voice say <text>` 都走这里，测的才是同一条路。
// ---------------------------------------------------------------------------
String interpret(const String& text) {
    if (text.isEmpty()) return String();

    const String local = localintent::match(text);
    if (!local.isEmpty()) {
        Serial.printf("[CMD] \"%s\" -> %s (local)\n", text.c_str(), local.c_str());
        return local;
    }

#if LAMP_BACKEND_LLM
    const String llm = normalizeLlmReply(classifyIntent(text));
    if (llm.startsWith("ANSWER:")) {
        Serial.printf("[LLM] \"%s\" -> 对话\n", text.c_str());
    } else if (!llm.isEmpty()) {
        Serial.printf("[LLM] \"%s\" -> %s\n", text.c_str(), llm.c_str());
    }
    return llm;
#else
    Serial.printf("[voice] 没匹配到命令: %s\n", text.c_str());
    return String();
#endif
}

bool begin() {
    if (sBegun) return hasKeys();
    sBegun = true;

    sPrefs.begin(kNvsNamespace, true);
    sApiKey = sPrefs.getString("ak", "");
    sSecretKey = sPrefs.getString("sk", "");
#if LAMP_BACKEND_LLM
    sLlmKey = sPrefs.getString("lk", "");
#endif
    sPrefs.end();

    // NVS 空着就退回编译期常量：写在 include/secrets.h 里，不进仓库。
    if (sApiKey.isEmpty() && strlen(LAMP_CLOUD_API_KEY) > 0) {
        sApiKey = LAMP_CLOUD_API_KEY;
        sSecretKey = LAMP_CLOUD_SECRET_KEY;
        Serial.println("[asr] key 来自 secrets.h（NVS 为空）");
    }

#if LAMP_BACKEND_LLM
    // 第二跳的独立 Key：NVS 的 lk > secrets.h 的 LAMP_CLOUD_LLM_KEY。
    // 刻意不回落到 sApiKey —— 一旦回落，百炼那种「一个 Key 打两跳」会被误判成腾讯，
    // 而 provider=4 时 sApiKey 是 SecretId，拿去当 Bearer 只会得到 401002。
    if (sLlmKey.isEmpty() && strlen(LAMP_CLOUD_LLM_KEY) > 0) {
        sLlmKey = LAMP_CLOUD_LLM_KEY;
        Serial.println("[asr] 第二跳 key 来自 secrets.h（腾讯 TokenHub）");
    }
    if (llmGoesToTencent()) {
        Serial.printf("[llm] 第二跳走腾讯 %s%s model=%s\n", LAMP_TENCENT_LLM_HOST,
                      LAMP_TENCENT_LLM_PATH, LAMP_TENCENT_LLM_MODEL);
    }
#endif

    if (!hasKeys()) {
#if LAMP_BACKEND_TENCENT
        Serial.println("[asr] 未配置凭据：串口 `key <SecretId> <SecretKey> <TokenHubKey>`");
#elif LAMP_BACKEND_ALIYUN
        Serial.println("[asr] 未配置 API Key：串口执行 key <DASHSCOPE_API_KEY>");
#else
        Serial.println("[asr] 未配置 API Key：串口执行 key <API_KEY> [SECRET_KEY]");
#endif
        return false;
    }

#if LAMP_BACKEND_TENCENT
    // SecretId 打印前 4 位足够定位问题，SecretKey 一个字都不打。
    Serial.printf("[asr] provider=%s secretId=%s... secretKey=%s\n", providerName(),
                  sApiKey.substring(0, 4).c_str(), sSecretKey.isEmpty() ? "missing" : "set");
    if (sSecretKey.isEmpty()) {
        Serial.println("[asr] 缺 SecretKey：TC3 签名必须两个都给");
        return false;
    }
    if (sLlmKey.isEmpty()) {
        Serial.println("[asr] 缺 TokenHub Key：第二跳会一直返回 none");
    }
    // Wi-Fi 已连上就先把 NTP 踢出去，等第一次说话时时钟已经好了。
    if (WiFi.status() == WL_CONNECTED) kickClockSync();
    return true;
#elif LAMP_BACKEND_ALIYUN
    // 百炼用 Bearer Key 直接鉴权，没有 token 交换这一步。
    return true;
#else
    // provider 1/2 语音都由百度转文字，所以都要先换 access_token。
    if (WiFi.status() == WL_CONNECTED) fetchToken();
    return true;
#endif
}

bool ready() {
    return hasKeys() && WiFi.status() == WL_CONNECTED;
}

bool setKey(const char* api_key, const char* secret_key, const char* llm_key) {
    if (api_key == nullptr || api_key[0] == '\0') {
        Serial.println("[asr] usage: key <API_KEY> [SECRET_KEY] [LLM_KEY]");
        return false;
    }
    sApiKey = api_key;
    sSecretKey = (secret_key != nullptr) ? secret_key : "";
    sBegun = true;

    sPrefs.begin(kNvsNamespace, false);
    sPrefs.putString("ak", sApiKey);
    sPrefs.putString("sk", sSecretKey);
#if LAMP_BACKEND_LLM
    // 第三个参数只在 provider=4（SecretId/SecretKey/TokenHubKey 三分）时才有意义；
    // 不传就保持原值，避免覆盖已经在 secrets.h 里配好的那把。
    if (llm_key != nullptr && llm_key[0] != '\0') {
        sLlmKey = llm_key;
        sPrefs.putString("lk", sLlmKey);
    }
#endif
    sPrefs.end();
    Serial.println("[asr] key saved to NVS");

#if LAMP_BACKEND_LLM && !LAMP_BACKEND_TENCENT
    if (llm_key != nullptr && llm_key[0] != '\0') {
        Serial.println("[asr] 第二跳 Key 已更新（改了 lk 槽）");
    }
#endif

#if LAMP_BACKEND_TENCENT
    if (sSecretKey.isEmpty() || sLlmKey.isEmpty()) {
        Serial.println("[asr] 腾讯要三个值：key <SecretId> <SecretKey> <sk- TokenHubKey>");
        return false;
    }
    if (WiFi.status() == WL_CONNECTED) kickClockSync();
    return (WiFi.status() == WL_CONNECTED);
#elif LAMP_BACKEND_ALIYUN
    // 百炼只要一个 Key，第二个参数直接忽略。
    if (!sSecretKey.isEmpty()) Serial.println("[asr] 百炼不用 Secret Key，已忽略");
    return (WiFi.status() == WL_CONNECTED);
#else
    if (sSecretKey.isEmpty()) {
        // 百度转文字必须要 Secret Key（provider=2 时语音仍由百度负责）。
        Serial.println("[asr] 百度还需要 Secret Key：key <API_KEY> <SECRET_KEY>");
        return false;
    }
    return (WiFi.status() == WL_CONNECTED) ? fetchToken() : false;
#endif
}

String transcribe(const int16_t* pcm, size_t samples) {
    if (pcm == nullptr || samples == 0) return String();
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[asr] wifi down, dropped");
        return String();
    }
    if (!hasKeys()) {
        Serial.println("[asr] no key, dropped");
        return String();
    }

#if LAMP_BACKEND_TENCENT
    // 第一跳：腾讯把 WAV 转成文字；第二跳的路由交给 interpret()（本地词表 → LLM）。
    const String text = transcribeTencent(pcm, samples);
    if (text.isEmpty()) return text;
    Serial.printf("[ASR] \"%s\"\n", text.c_str());
    return interpret(text);
#elif LAMP_BACKEND_ALIYUN
    // 第一步：百炼把 WAV 转成文字（一个 Key、一次 POST），第二步沿用同一条 LLM 分支。
    const String text = transcribeBailian(pcm, samples);
    if (text.isEmpty()) return text;
    Serial.printf("[ASR] \"%s\"\n", text.c_str());
    const String intent = classifyIntent(text);
    Serial.printf("[LLM] \"%s\" -> %s\n", text.c_str(), intent.c_str());
    return intent;
#else
    // 第一步：百度把语音转成文字（provider 1/2 都一样）。
    if (sToken.isEmpty() && !fetchToken()) return String();
    String text = recognizeOnce(pcm, samples);
    if (text.isEmpty() && sToken.isEmpty()) {
        // recognizeOnce 判定 token 过期，重取一次再试
        if (fetchToken()) text = recognizeOnce(pcm, samples);
    }
    if (text.isEmpty()) return text;

#if LAMP_BACKEND_LLM
    // 第二步：国产 LLM 把文字映射成动作标签，返回 on/off/up/down/... 给上层。
    Serial.printf("[ASR] \"%s\"\n", text.c_str());
    const String intent = classifyIntent(text);
    Serial.printf("[LLM] \"%s\" -> %s\n", text.c_str(), intent.c_str());
    return intent;
#else
    return text;
#endif

#endif
}

const char* providerName() {
#if LAMP_BACKEND_TENCENT
    return "tencent";
#elif LAMP_BACKEND_ALIYUN
    return "bailian";
#elif LAMP_BACKEND_LLM
    return "baidu+llm";
#else
    return "baidu";
#endif
}

String classifyText(const String& text) {
#if LAMP_BACKEND_LLM
    return classifyIntent(text);
#else
    return text;
#endif
}

void printStatus() {
    const bool wifiUp = WiFi.status() == WL_CONNECTED;
#if LAMP_BACKEND_TENCENT
    // 排 SignatureExpire 时第一眼看 clock：没对时的话，签名算法再对也没用。
    Serial.printf("[asr] provider=%s wifi=%s secretId=%s secretKey=%s clock=%s\n", providerName(),
                  wifiUp ? "up" : "down", hasKeys() ? "set" : "missing",
                  sSecretKey.isEmpty() ? "missing" : "set",
                  clockReady() ? "synced" : "NOT-synced");
    Serial.printf("[asr] asr=%s @ %s\n", CLOUD_TC_ASR_ENGINE, CLOUD_TC_ASR_HOST);
    Serial.printf("[llm] %s @ %s key=%s\n", LAMP_TENCENT_LLM_MODEL, LAMP_TENCENT_LLM_HOST,
                  sLlmKey.isEmpty() ? "missing" : "set");
#elif LAMP_BACKEND_ALIYUN
    // 百炼没有 token，状态里把两个模型名带上，换模型时一眼看出跑的是哪个。
    Serial.printf("[asr] provider=%s asr=%s llm=%s wifi=%s key=%s\n", providerName(),
                  CLOUD_BAILIAN_ASR_MODEL, CLOUD_BAILIAN_LLM_MODEL, wifiUp ? "up" : "down",
                  hasKeys() ? "set" : "missing");
#else
    Serial.printf("[asr] provider=%s wifi=%s key=%s token=%s age=%lus\n", providerName(),
                  wifiUp ? "up" : "down", hasKeys() ? "set" : "missing",
                  sToken.isEmpty() ? "none" : "ok",
                  sToken.isEmpty() ? 0UL : static_cast<unsigned long>((millis() - sTokenAtMs) / 1000));
#endif
#if LAMP_BACKEND_LLM && !LAMP_BACKEND_TENCENT
    // 第二跳到底打哪儿。排 401002 / invalid_api_key 时第一眼要看这一行。
    if (llmGoesToTencent()) {
        Serial.printf("[llm] 腾讯 %s%s model=%s key=%s\n", LAMP_TENCENT_LLM_HOST,
                      LAMP_TENCENT_LLM_PATH, LAMP_TENCENT_LLM_MODEL,
                      sLlmKey.isEmpty() ? "missing" : "set");
    } else {
        Serial.println("[llm] 跟随 ASR 厂商（与第一跳同一把 Key）");
    }
#endif
}

}  // namespace cloudasr

#endif  // LAMP_CLOUD_ASR
