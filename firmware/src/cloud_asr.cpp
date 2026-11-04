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

#if LAMP_CLOUD_ASR_PROVIDER == 2
constexpr const char* kOpenAiHost = "api.openai.com";
constexpr const char* kOpenAiPath = "/v1/audio/transcriptions";
#else
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

#if LAMP_CLOUD_ASR_PROVIDER == 2

// ---------------------------------------------------------------------------
// OpenAI /v1/audio/transcriptions：multipart/form-data + WAV 头。
// 用裸 WiFiClientSecure 流式写，避免为了拼 multipart 再复制一份 64 KB 音频。
// ---------------------------------------------------------------------------
String recognizeOnce(const int16_t* pcm, size_t samples) {
    WiFiClientSecure client;
    client.setInsecure();  // 自用台架：跳过证书校验，产品化要换成根证书
    client.setTimeout(CLOUD_ASR_TIMEOUT_MS / 1000);
    if (!client.connect(kOpenAiHost, 443)) {
        Serial.println("[asr] openai connect failed");
        return String();
    }

    const uint32_t dataBytes = static_cast<uint32_t>(samples * sizeof(int16_t));
    uint8_t wav[44] = {0};
    const uint32_t riffSize = 36 + dataBytes;
    const uint32_t sampleRate = MIC_SAMPLE_RATE;
    const uint32_t byteRate = sampleRate * 2;
    const uint32_t fmtLen = 16;
    const uint16_t pcmFmt = 1;
    const uint16_t channels = 1;
    const uint16_t blockAlign = 2;
    const uint16_t bits = 16;
    memcpy(wav + 0, "RIFF", 4);
    memcpy(wav + 4, &riffSize, 4);
    memcpy(wav + 8, "WAVEfmt ", 8);
    memcpy(wav + 16, &fmtLen, 4);
    memcpy(wav + 20, &pcmFmt, 2);
    memcpy(wav + 22, &channels, 2);
    memcpy(wav + 24, &sampleRate, 4);
    memcpy(wav + 28, &byteRate, 4);
    memcpy(wav + 32, &blockAlign, 2);
    memcpy(wav + 34, &bits, 2);
    memcpy(wav + 36, "data", 4);
    memcpy(wav + 40, &dataBytes, 4);

    const char* boundary = "----lamp-utt-7d1f";
    const String head = String("--") + boundary +
                        "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n" +
                        CLOUD_ASR_OPENAI_MODEL + "\r\n";
    const String fileHead = String("--") + boundary +
                            "\r\nContent-Disposition: form-data; name=\"file\"; "
                            "filename=\"utt.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
    const String tail = String("\r\n--") + boundary + "--\r\n";
    const size_t total = head.length() + fileHead.length() + sizeof(wav) + dataBytes + tail.length();

    client.printf(
        "POST %s HTTP/1.1\r\nHost: %s\r\nAuthorization: Bearer %s\r\n"
        "Content-Type: multipart/form-data; boundary=%s\r\nContent-Length: %u\r\n"
        "Connection: close\r\n\r\n",
        kOpenAiPath, kOpenAiHost, sApiKey.c_str(), boundary, static_cast<unsigned>(total));
    client.print(head);
    client.print(fileHead);
    client.write(wav, sizeof(wav));

    const uint8_t* raw = reinterpret_cast<const uint8_t*>(pcm);
    for (size_t off = 0; off < dataBytes; off += 2048) {
        const size_t n = (dataBytes - off < 2048) ? (dataBytes - off) : 2048;
        client.write(raw + off, n);
    }
    client.print(tail);

    String reply;
    const uint32_t deadline = millis() + CLOUD_ASR_TIMEOUT_MS;
    while (millis() < deadline) {
        while (client.available() > 0) {
            reply += static_cast<char>(client.read());
            if (reply.length() > 4096) break;
        }
        if (!client.connected()) break;
        delay(1);
    }
    client.stop();

    const int sep = reply.indexOf("\r\n\r\n");
    if (sep < 0) {
        Serial.println("[asr] openai: no response body");
        return String();
    }
    const String body = reply.substring(sep + 4);

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) {
        Serial.printf("[asr] openai: bad json (%u bytes)\n", static_cast<unsigned>(body.length()));
        return String();
    }
    if (!doc["error"].isNull()) {
        const char* message = doc["error"]["message"] | "unknown";
        Serial.printf("[asr] openai error: %s\n", message);
        return String();
    }
    return cleanText(String(doc["text"] | ""));
}

#else

// ---------------------------------------------------------------------------
// 百度短语音：先换 token（有效期 30 天），再用 RAW 方式把裸 PCM 当 body 传。
// 不用 JSON + base64，省掉 1/3 的内存膨胀。
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

#endif  // provider

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

#if LAMP_CLOUD_ASR_PROVIDER != 2
    if (WiFi.status() == WL_CONNECTED) fetchToken();
#endif
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

#if LAMP_CLOUD_ASR_PROVIDER == 2
    return true;
#else
    if (sSecretKey.isEmpty()) {
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

#if LAMP_CLOUD_ASR_PROVIDER == 2
    return recognizeOnce(pcm, samples);
#else
    if (sToken.isEmpty() && !fetchToken()) return String();
    String text = recognizeOnce(pcm, samples);
    if (text.isEmpty() && sToken.isEmpty()) {
        // recognizeOnce 判定 token 过期，重取一次再试
        if (fetchToken()) text = recognizeOnce(pcm, samples);
    }
    return text;
#endif
}

const char* providerName() {
#if LAMP_CLOUD_ASR_PROVIDER == 2
    return "openai";
#else
    return "baidu";
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
