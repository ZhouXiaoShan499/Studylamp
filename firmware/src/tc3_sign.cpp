#include "tc3_sign.h"

#include <mbedtls/md.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

namespace tc3 {

const char* const kContentType = "application/json; charset=utf-8";

namespace {

const char* const kHexDigits = "0123456789abcdef";

const mbedtls_md_info_t* sha256Info() {
    return mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
}

void toHex(const uint8_t* raw, size_t len, char* out) {
    for (size_t i = 0; i < len; ++i) {
        out[2 * i] = kHexDigits[raw[i] >> 4];
        out[2 * i + 1] = kHexDigits[raw[i] & 0x0F];
    }
    out[2 * len] = '\0';
}

// 一趟算完的 SHA256，正文已经在内存里，不额外分配。
void sha256Hex(const uint8_t* data, size_t len, char out[65]) {
    uint8_t digest[32];
    const mbedtls_md_info_t* info = sha256Info();
    if (info == nullptr || mbedtls_md(info, data, len, digest) != 0) {
        out[0] = '\0';
        return;
    }
    toHex(digest, sizeof(digest), out);
}

void hmacSha256(const uint8_t* key, size_t key_len, const uint8_t* msg, size_t msg_len,
                uint8_t out[32]) {
    mbedtls_md_hmac(sha256Info(), key, key_len, msg, msg_len, out);
}

// 派生密钥：kDate -> kService -> kSigning，最后一步才是签名。
// 链条里任何一环用错 service 或 date，得到的都是一串「看起来很正常」的十六进制，
// 只有服务端能判错，所以本地一定要有 selfTest()。
void deriveAndSign(const char* secret_key, const char* date, const char* service,
                   const char* msg, size_t msg_len, char out[65]) {
    uint8_t k_date[32];
    uint8_t k_service[32];
    uint8_t k_signing[32];
    uint8_t signature[32];

    String prefix = String("TC3") + secret_key;
    hmacSha256(reinterpret_cast<const uint8_t*>(prefix.c_str()), prefix.length(),
               reinterpret_cast<const uint8_t*>(date), strlen(date), k_date);
    hmacSha256(k_date, sizeof(k_date), reinterpret_cast<const uint8_t*>(service), strlen(service),
               k_service);
    static const char kRequest[] = "tc3_request";
    hmacSha256(k_service, sizeof(k_service), reinterpret_cast<const uint8_t*>(kRequest),
               sizeof(kRequest) - 1, k_signing);
    hmacSha256(k_signing, sizeof(k_signing), reinterpret_cast<const uint8_t*>(msg), msg_len,
               signature);
    toHex(signature, sizeof(signature), out);
}

}  // namespace

String signatureHex(const char* secret_key, const char* date, const char* service,
                    const String& string_to_sign) {
    char out[65];
    deriveAndSign(secret_key, date, service, string_to_sign.c_str(), string_to_sign.length(), out);
    return String(out);
}

String authorization(const char* secret_id, const char* secret_key, const char* host,
                     const char* service, const char* action_lower, const uint8_t* body,
                     size_t body_len, uint32_t ts_utc, String* out_canonical) {
    char payload_sha[65];
    sha256Hex(body, body_len, payload_sha);

    // date 必须来自同一个 ts 且按 UTC+0 推。用本地时区会在东八区凌晨翻车
    // （白天正常、夜里 SignatureExpire），这是 docs/14 点名的第一个坑。
    const time_t stamp = static_cast<time_t>(ts_utc);
    struct tm utc {};
    gmtime_r(&stamp, &utc);
    char date[11];
    snprintf(date, sizeof(date), "%04d-%02d-%02d", utc.tm_year + 1900, utc.tm_mon + 1,
             utc.tm_mday);

    // CanonicalRequest：方法、URI、query、规范头、签名头、正文散列。
    // 规范头自己也带结尾换行，所以「规范头」和「签名头」之间是两个换行，
    // 少一个就是 SignatureFailure（官方 CVM 向量的规范串 SHA 会对不上）。
    String canonical;
    canonical.reserve(512);
    canonical += F("POST\n/\n\n");
    canonical += F("content-type:");
    canonical += kContentType;
    canonical += F("\nhost:");
    canonical += host;
    canonical += F("\nx-tc-action:");
    canonical += action_lower;  // 值必须小写，发送的 X-TC-Action 头才是驼峰
    canonical += F("\n\ncontent-type;host;x-tc-action\n");
    canonical += payload_sha;

    char canonical_sha[65];
    sha256Hex(reinterpret_cast<const uint8_t*>(canonical.c_str()), canonical.length(),
              canonical_sha);

    String to_sign;
    to_sign.reserve(256);
    to_sign += F("TC3-HMAC-SHA256\n");
    to_sign += String(static_cast<unsigned long>(ts_utc));
    to_sign += '\n';
    to_sign += date;
    to_sign += '/';
    to_sign += service;
    to_sign += F("/tc3_request\n");
    to_sign += canonical_sha;

    const String signature = signatureHex(secret_key, date, service, to_sign);

    if (out_canonical != nullptr) *out_canonical = canonical;

    String auth;
    auth.reserve(256);
    auth += F("TC3-HMAC-SHA256 Credential=");
    auth += secret_id;
    auth += '/';
    auth += date;
    auth += '/';
    auth += service;
    auth += F("/tc3_request, SignedHeaders=content-type;host;x-tc-action, Signature=");
    auth += signature;
    return auth;
}

// ---------------------------------------------------------------------------
// 官方示例向量（DescribeInstances，2019-02-25，ts=1551113065）
// 期望值与腾讯云官方 SDK tencentcloud.common.sign.Sign.sign_tc3() 的输出一致，
// Python 版 tools/tencent_voice_check.py --self-test 用的是同一组常量。
// ---------------------------------------------------------------------------
bool selfTest() {
    static const char kPayload[] =
        "{\"Limit\": 1, \"Filters\": [{\"Values\": [\"\\u672a\\u547d\\u540d\"], "
        "\"Name\": \"instance-name\"}]}";
    const String payload(kPayload);
    const char* kExpectedPayloadSha =
        "35e9c5b0e3ae67532d3c9f17ead6c90222632e5b1ff7f6e89887f1398934f064";
    const char* kExpectedCanonicalSha =
        "7019a55be8395899b900fb5564e4200d984910f34794a27cb3fb7d10ff6a1e84";
    const char* kExpectedSignature =
        "e801a66cc33c3633e72c49cb43fd4864de18c6a78fe8caffd73883bfeb4ba8b5";

    bool ok = true;

    char payload_sha[65];
    sha256Hex(reinterpret_cast<const uint8_t*>(payload.c_str()), payload.length(), payload_sha);
    const bool payload_ok = String(payload_sha) == kExpectedPayloadSha;
    Serial.printf("  payload SHA256   %s  %s\n", String(payload_sha).substring(0, 16).c_str(),
                  payload_ok ? "OK" : "FAIL");
    ok &= payload_ok;

    String canonical;
    const String auth = authorization("SECRETID-VECTOR", "SECRETKEY-VECTOR", "cvm.tencentcloudapi.com",
                                      "cvm", "describeinstances",
                                      reinterpret_cast<const uint8_t*>(payload.c_str()),
                                      payload.length(), 1551113065UL, &canonical);
    char canonical_sha[65];
    sha256Hex(reinterpret_cast<const uint8_t*>(canonical.c_str()), canonical.length(),
              canonical_sha);
    const bool canonical_ok = String(canonical_sha) == kExpectedCanonicalSha;
    Serial.printf("  Canonical SHA256 %s  %s\n", String(canonical_sha).substring(0, 16).c_str(),
                  canonical_ok ? "OK" : "FAIL");
    ok &= canonical_ok;

    const int at = auth.indexOf("Signature=");
    const String signature = at >= 0 ? auth.substring(at + 10) : String();
    const bool sign_ok = signature == kExpectedSignature;
    Serial.printf("  HMAC 派生链      %s  %s\n", signature.substring(0, 16).c_str(),
                  sign_ok ? "OK" : "FAIL");
    ok &= sign_ok;

#if 0  // 需要看完整串时改 1：与 API Explorer 的「签名串生成」逐字节对照
    Serial.println("  --- CanonicalRequest ---");
    Serial.println(canonical);
    Serial.println("  --- Authorization ---");
    Serial.println(auth);
#endif

    return ok;
}

void printSelfTest() {
    Serial.println("=== TC3 签名自检（官方示例，不需要 Key）===");
    const bool ok = selfTest();
    Serial.println(ok ? "  PASS：拼接顺序与官方 SDK 逐字节一致"
                      : "  FAIL：签名与官方向量不一致，先别接网络，回来查拼接");
}

}  // namespace tc3
