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

namespace cloudasr {
namespace {

constexpr const char* kNvsNamespace = "cloudasr";

// 百度短语音的地址在两种 provider 下都要用：provider=2 时语音仍由百度转文字，
// 只是拿到文字后再交给国产 LLM 判断动作。
constexpr const char* kBaiduTokenUrl = "https://aip.baidubce.com/oauth/2.0/token";
constexpr const char* kBaiduAsrUrl = "https://vop.baidu.com/server_api";

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

#if LAMP_CLOUD_ASR_PROVIDER == 2

// ---------------------------------------------------------------------------
// 国产大模型（OpenAI 兼容 /chat/completions）：把百度识别出的文字交给 LLM，
// 让它只回一个动作标签。没有喇叭，LLM 不需要回一句完整的话，只回一个词，
// 延迟和成本都压到最低。
//   system 提示词约定：只允许回 on / off / up / down / max / night / study / rest / none
// ---------------------------------------------------------------------------
String classifyIntent(const String& text) {
    if (text.isEmpty()) return String();

    WiFiClientSecure client;
    client.setInsecure();  // 自用台架；产品化换根证书
    client.setTimeout(CLOUD_ASR_TIMEOUT_MS / 1000);
    if (!client.connect(CLOUD_LLM_HOST, 443)) {
        Serial.println("[llm] connect failed");
        return String();
    }

    JsonDocument req;
    req["model"] = CLOUD_LLM_MODEL;
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
                  CLOUD_LLM_PATH, CLOUD_LLM_HOST, sApiKey.c_str(),
                  static_cast<unsigned>(body.length()));
    client.print(body);

    String reply;
    const uint32_t deadline = millis() + CLOUD_ASR_TIMEOUT_MS;
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

#endif  // LAMP_CLOUD_ASR_PROVIDER == 2

}  // namespace

bool begin() {
    if (sBegun) return hasKeys();
    sBegun = true;

    sPrefs.begin(kNvsNamespace, true);
    sApiKey = sPrefs.getString("ak", "");
    sSecretKey = sPrefs.getString("sk", "");
    sPrefs.end();

    if (!hasKeys()) {
        Serial.println("[asr] 未配置 API Key：串口执行 key <API_KEY> [SECRET_KEY]");
        return false;
    }
    Serial.printf("[asr] provider=%s key=%s...\n", providerName(), sApiKey.substring(0, 4).c_str());

    // 两种 provider 语音都由百度转文字，所以都要换 token。
    if (WiFi.status() == WL_CONNECTED) fetchToken();
    return true;
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

    if (sSecretKey.isEmpty()) {
        // 百度转文字必须要 Secret Key（provider=2 时语音仍由百度负责）。
        Serial.println("[asr] 百度还需要 Secret Key：key <API_KEY> <SECRET_KEY>");
        return false;
    }
    return (WiFi.status() == WL_CONNECTED) ? fetchToken() : false;
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

    // 第一步：百度把语音转成文字（两种 provider 都一样）。
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
}

const char* providerName() {
#if LAMP_CLOUD_ASR_PROVIDER == 2
    return "baidu+llm";
#else
    return "baidu";
#endif
}

String classifyText(const String& text) {
#if LAMP_CLOUD_ASR_PROVIDER == 2
    return classifyIntent(text);
#else
    return text;
#endif
}

void printStatus() {
    const bool wifiUp = WiFi.status() == WL_CONNECTED;
    Serial.printf("[asr] provider=%s wifi=%s key=%s token=%s age=%lus\n", providerName(),
                  wifiUp ? "up" : "down", hasKeys() ? "set" : "missing",
                  sToken.isEmpty() ? "none" : "ok",
                  sToken.isEmpty() ? 0UL : static_cast<unsigned long>((millis() - sTokenAtMs) / 1000));
}

}  // namespace cloudasr

#endif  // LAMP_CLOUD_ASR
