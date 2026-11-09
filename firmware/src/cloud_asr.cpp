#include "cloud_asr.h"

#if !LAMP_CLOUD_ASR

// 云端识别关闭：全部空实现，voice_client 走回原来的局域网 Vosk 路径。
namespace cloudasr {
bool begin() { return false; }
bool ready() { return false; }
bool setKey(const char*, const char*) { return false; }
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

#if LAMP_CLOUD_ASR_PROVIDER == 3
#include <mbedtls/base64.h>
#endif

namespace cloudasr {
namespace {

constexpr const char* kNvsNamespace = "cloudasr";

// 百度只在 provider 1/2 用：1 是转文字后本地匹配关键词，2 是转文字后再交 LLM。
// 3 整条换成百炼，下面这段连编译都不进，避免未使用告警。
#if LAMP_CLOUD_ASR_PROVIDER != 3
constexpr const char* kBaiduTokenUrl = "https://aip.baidubce.com/oauth/2.0/token";
constexpr const char* kBaiduAsrUrl = "https://vop.baidu.com/server_api";
#endif

Preferences sPrefs;
String sApiKey;
String sSecretKey;
String sToken;          // 百度的 access_token；OpenAI 用不到
uint32_t sTokenAtMs = 0;
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
#if LAMP_CLOUD_ASR_PROVIDER != 3

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

#endif  // LAMP_CLOUD_ASR_PROVIDER != 3

#if LAMP_CLOUD_ASR_PROVIDER >= 2

// ---------------------------------------------------------------------------
// 一次 /chat/completions：把识别出的文字交给 LLM，让它只回一个动作标签。
// 没有喇叭，LLM 不需要回一句完整的话，只回一个词，延迟和成本都压到最低。
//   system 提示词约定：只允许回 on / off / up / down / max / night / study / rest / none
// host/path/model 由调用方给：provider=2 直连各家，provider=3 打百炼兼容模式。
// ---------------------------------------------------------------------------
String postChatIntent(const String& text, const char* host, const char* path, const char* model,
                      uint16_t timeout_ms) {
    if (text.isEmpty()) return String();

    WiFiClientSecure client;
    client.setInsecure();  // 自用台架；产品化换根证书
    client.setTimeout(timeout_ms / 1000);
    if (!client.connect(host, 443)) {
        Serial.println("[llm] connect failed");
        return String();
    }

    JsonDocument req;
    req["model"] = String(model);
    JsonArray messages = req["messages"].to<JsonArray>();
    JsonObject sys = messages.add<JsonObject>();
    sys["role"] = "system";
    sys["content"] =
        "你是台灯的控制中枢。根据用户这句话，只回一个英文单词，不要解释、不要标点："
        "开灯=on，关灯=off，调亮=up，调暗=down，调到最亮=max，夜灯=night，"
        "学习/看书模式=study，休息/睡眠模式=rest，其他无关的话=none。";
    JsonObject usr = messages.add<JsonObject>();
    usr["role"] = "user";
    usr["content"] = text;
    req["temperature"] = 0;
    req["max_tokens"] = 8;

    String body;
    serializeJson(req, body);

    client.printf("POST %s HTTP/1.1\r\nHost: %s\r\nAuthorization: Bearer %s\r\n"
                  "Content-Type: application/json\r\nContent-Length: %u\r\n"
                  "Connection: close\r\n\r\n",
                  path, host, sApiKey.c_str(),
                  static_cast<unsigned>(body.length()));
    client.print(body);

    String reply;
    const uint32_t deadline = millis() + timeout_ms;
    while (millis() < deadline) {
        while (client.available() > 0) {
            reply += static_cast<char>(client.read());
            if (reply.length() > 2048) break;
        }
        if (!client.connected()) break;
        delay(1);
    }
    client.stop();

    const int sep = reply.indexOf("\r\n\r\n");
    if (sep < 0) {
        Serial.println("[llm] no response body");
        return String();
    }
    const String json = reply.substring(sep + 4);

    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) {
        Serial.printf("[llm] bad json (%u bytes)\n", static_cast<unsigned>(json.length()));
        return String();
    }
    if (!doc["error"].isNull()) {
        const char* message = doc["error"]["message"] | "unknown";
        Serial.printf("[llm] error: %s\n", message);
        return String();
    }
    return cleanText(String(doc["choices"][0]["message"]["content"] | ""));
}

#if LAMP_CLOUD_ASR_PROVIDER == 2
// provider=2：直连各家 OpenAI 兼容端点（api.deepseek.com 等）。
String classifyIntent(const String& text) {
    return postChatIntent(text, CLOUD_LLM_HOST, CLOUD_LLM_PATH, CLOUD_LLM_MODEL,
                          CLOUD_ASR_TIMEOUT_MS);
}
#else
// provider=3：同一个百炼 Key、同一个域名，只把模型换成 deepseek-v4.1-flash。
String classifyIntent(const String& text) {
    return postChatIntent(text, CLOUD_BAILIAN_HOST, CLOUD_BAILIAN_PATH, CLOUD_BAILIAN_LLM_MODEL,
                          CLOUD_BAILIAN_TIMEOUT_MS);
}
#endif

#endif  // LAMP_CLOUD_ASR_PROVIDER >= 2

#if LAMP_CLOUD_ASR_PROVIDER == 3

// ---------------------------------------------------------------------------
// 阿里云百炼：qwen3-asr-flash 的 OpenAI 兼容接口。
// 和百度最大的差别是它不吃裸 PCM——必须给完整 WAV（含 44 字节头）再 base64。
// 2 s / 16 kHz / 单声道 = 64044 B，base64 后约 85 KB，整段 JSON 一次 POST，
// 同步返回 choices[0].message.content。走 PSRAM，不占内部 RAM。
// ---------------------------------------------------------------------------
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

#endif  // LAMP_CLOUD_ASR_PROVIDER == 3

}  // namespace

bool begin() {
    if (sBegun) return hasKeys();
    sBegun = true;

    sPrefs.begin(kNvsNamespace, true);
    sApiKey = sPrefs.getString("ak", "");
    sSecretKey = sPrefs.getString("sk", "");
    sPrefs.end();

    // NVS 空着就退回编译期常量：写在 include/secrets.h 里，不进仓库。
    if (sApiKey.isEmpty() && strlen(LAMP_CLOUD_API_KEY) > 0) {
        sApiKey = LAMP_CLOUD_API_KEY;
        sSecretKey = LAMP_CLOUD_SECRET_KEY;
        Serial.println("[asr] key 来自 secrets.h（NVS 为空）");
    }

    if (!hasKeys()) {
#if LAMP_CLOUD_ASR_PROVIDER == 3
        Serial.println("[asr] 未配置 API Key：串口执行 key <DASHSCOPE_API_KEY>");
#else
        Serial.println("[asr] 未配置 API Key：串口执行 key <API_KEY> [SECRET_KEY]");
#endif
        return false;
    }
    Serial.printf("[asr] provider=%s key=%s...\n", providerName(), sApiKey.substring(0, 4).c_str());

#if LAMP_CLOUD_ASR_PROVIDER == 3
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

bool setKey(const char* api_key, const char* secret_key) {
    if (api_key == nullptr || api_key[0] == '\0') {
        Serial.println("[asr] usage: key <API_KEY> [SECRET_KEY]");
        return false;
    }
    sApiKey = api_key;
    sSecretKey = (secret_key != nullptr) ? secret_key : "";
    sToken = "";
    sBegun = true;

    sPrefs.begin(kNvsNamespace, false);
    sPrefs.putString("ak", sApiKey);
    sPrefs.putString("sk", sSecretKey);
    sPrefs.end();
    Serial.println("[asr] key saved to NVS");

#if LAMP_CLOUD_ASR_PROVIDER == 3
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

#if LAMP_CLOUD_ASR_PROVIDER == 3
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

#if LAMP_CLOUD_ASR_PROVIDER == 2
    // 第二步：国产 LLM 把文字映射成动作标签，返回 on/off/up/down/... 给上层。
    Serial.printf("[ASR] \"%s\"\n", text.c_str());
    const String intent = classifyIntent(text);
    Serial.printf("[LLM] \"%s\" -> %s\n", text.c_str(), intent.c_str());
    return intent;
#else
    return text;
#endif

#endif  // LAMP_CLOUD_ASR_PROVIDER == 3
}

const char* providerName() {
#if LAMP_CLOUD_ASR_PROVIDER == 2
    return "baidu+llm";
#elif LAMP_CLOUD_ASR_PROVIDER == 3
    return "bailian";
#else
    return "baidu";
#endif
}

String classifyText(const String& text) {
#if LAMP_CLOUD_ASR_PROVIDER >= 2
    return classifyIntent(text);
#else
    return text;
#endif
}

void printStatus() {
    const bool wifiUp = WiFi.status() == WL_CONNECTED;
#if LAMP_CLOUD_ASR_PROVIDER == 3
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
}

}  // namespace cloudasr

#endif  // LAMP_CLOUD_ASR
