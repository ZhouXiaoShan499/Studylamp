#include "wifi_link.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>

#include "config.h"
#include "net_console.h"
#include "ui.h"

namespace wifilink {
namespace {

constexpr const char* kNamespace = "lampwifi";
constexpr size_t kMaxScan = 24;

// 一张网络试多久。12 秒是实测折中：路由器 DHCP 慢的时候 6 秒不够，
// 而密码错一般 2~3 秒就返回 WL_CONNECT_FAILED，会走下面的提前失败分支。
constexpr uint32_t kConnectTimeoutMs = 12000;
constexpr uint32_t kScanTimeoutMs = 12000;
// 一轮全失败后的静默间隔。30 秒既不至于把射频占死，也能在换房间后很快恢复。
constexpr uint32_t kRetryIntervalMs = 30000;
// 掉线先等 4 秒（路由器重启/信道切换很常见），别一掉就重新扫。
constexpr uint32_t kLostGraceMs = 4000;

struct Entry {
    String ssid;
    String pass;
};

Entry nets_[kMaxNetworks];
size_t count_ = 0;
String lastSsid_;

struct ApInfo {
    String ssid;
    int32_t rssi;
    uint8_t channel;
    bool secure;
};

ApInfo aps_[kMaxScan];
int apCount_ = 0;

enum class Phase { Idle, Connecting, Scanning, Online, Backoff };

Phase phase_ = Phase::Idle;
int attempt_ = -1;                     // 正在试 nets_ 的哪一张
int candidates_[kMaxNetworks];         // 本轮扫描后的尝试顺序
size_t candidateCount_ = 0;
size_t candidatePos_ = 0;
uint32_t deadline_ = 0;
uint32_t attemptStart_ = 0;
uint32_t nextTryAt_ = 0;
uint32_t lostAt_ = 0;
uint32_t startedAt_ = 0;
bool started_ = false;

// ------------------------------------------------------------------ 存储 ---
int indexOf(const String& ssid) {
    for (size_t i = 0; i < count_; ++i) {
        if (nets_[i].ssid == ssid) return static_cast<int>(i);
    }
    return -1;
}

bool validSsid(const String& ssid) {
    return !ssid.isEmpty() && ssid.length() <= 32;
}

bool validPass(const String& pass) { return pass.length() <= 63; }

// 凭据只在配网时写，不参与每次重试，NVS 不会被写坏。
void saveNets() {
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
        Serial.println("[wifi] nvs open failed (save)");
        return;
    }
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (size_t i = 0; i < count_; ++i) {
        JsonObject item = arr.add<JsonObject>();
        item["s"] = nets_[i].ssid;
        item["p"] = nets_[i].pass;
    }
    String blob;
    serializeJson(doc, blob);
    prefs.putString("nets", blob);
    prefs.end();
}

// 老固件读的是 ssid/pass 两个键。每次成功连上都同步一份，
// 这样从新固件回刷旧固件时不用重新配网。
void rememberLast(int index) {
    if (index < 0 || index >= static_cast<int>(count_)) return;
    const String& ssid = nets_[index].ssid;
    if (ssid == lastSsid_) return;  // 没变就不写 flash
    lastSsid_ = ssid;

    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) return;
    prefs.putString("last", lastSsid_);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", nets_[index].pass);
    prefs.end();
}

void loadNets() {
    count_ = 0;
    Preferences prefs;
    String legacySsid;
    String legacyPass;
    if (prefs.begin(kNamespace, true)) {
        lastSsid_ = prefs.getString("last", "");
        legacySsid = prefs.getString("ssid", "");
        legacyPass = prefs.getString("pass", "");
        const String blob = prefs.getString("nets", "");
        if (!blob.isEmpty()) {
            JsonDocument doc;
            if (deserializeJson(doc, blob) == DeserializationError::Ok) {
                for (JsonVariant item : doc.as<JsonArray>()) {
                    if (count_ >= kMaxNetworks) break;
                    JsonObject obj = item.as<JsonObject>();
                    const String ssid = obj["s"] | "";
                    const String pass = obj["p"] | "";
                    if (!validSsid(ssid)) continue;
                    nets_[count_].ssid = ssid;
                    nets_[count_].pass = pass;
                    ++count_;
                }
            } else {
                Serial.println("[wifi] nvs nets blob unreadable, ignoring");
            }
        }
        prefs.end();
    }

    // 单网络时代的老键：迁移成一张表，并立刻以新格式落盘。
    if (count_ == 0 && validSsid(legacySsid)) {
        nets_[0].ssid = legacySsid;
        nets_[0].pass = legacyPass;
        count_ = 1;
        saveNets();
        Serial.println("[wifi] migrated legacy ssid/pass to nets");
    }

    // 没有 NVS 凭据时用 secrets.h 兜底：开发机上烧一次就能联网，
    // 量产板子是空的，会直接进配网热点。
    if (count_ == 0 && validSsid(String(LAMP_WIFI_SSID))) {
        nets_[0].ssid = LAMP_WIFI_SSID;
        nets_[0].pass = LAMP_WIFI_PASS;
        count_ = 1;
        Serial.println("[wifi] seeded from secrets.h");
    }

    if (lastSsid_.isEmpty() && count_ > 0) lastSsid_ = nets_[0].ssid;
}

// ------------------------------------------------------------- 连接状态机 ---
void enterBackoff(uint32_t now, uint32_t delayMs) {
    phase_ = Phase::Backoff;
    nextTryAt_ = now + delayMs;
    attempt_ = -1;
}

void beginConnect(int index, uint32_t now) {
    if (index < 0 || index >= static_cast<int>(count_)) {
        enterBackoff(now, kRetryIntervalMs);
        return;
    }
    attempt_ = index;
    // 热点在跑时不能 disconnect(true)：那会把射频整个停掉，正拿着网页配网
    // 的手机立刻掉线。false 只断 STA，AP 继续服务。
    WiFi.disconnect(!netconsole::apActive());
    delay(50);  // 等射频复位，紧接着 begin() 才不会继承上一轮的失败状态
    const String& pass = nets_[index].pass;
    WiFi.begin(nets_[index].ssid.c_str(), pass.isEmpty() ? nullptr : pass.c_str());

    phase_ = Phase::Connecting;
    attemptStart_ = now;
    deadline_ = now + kConnectTimeoutMs;
    Serial.printf("[wifi] try \"%s\" (%d/%u)\n", nets_[index].ssid.c_str(), index + 1,
                  static_cast<unsigned>(count_));
}

void startScan(uint32_t now) {
    WiFi.scanDelete();
    const int rc = WiFi.scanNetworks(true, true);  // async, show hidden
    phase_ = Phase::Scanning;
    deadline_ = now + kScanTimeoutMs;
    if (rc == WIFI_SCAN_FAILED) {
        Serial.println("[wifi] scan start failed");
        enterBackoff(now, kRetryIntervalMs);
    }
}

void copyScan(int n) {
    apCount_ = 0;
    for (int i = 0; i < n && apCount_ < static_cast<int>(kMaxScan); ++i) {
        const String ssid = WiFi.SSID(i);
        if (ssid.isEmpty()) continue;  // 隐藏网络没名字，选了也没法区分
        bool duplicate = false;
        for (int k = 0; k < apCount_; ++k) {
            if (aps_[k].ssid == ssid) {
                duplicate = true;
                if (WiFi.RSSI(i) > aps_[k].rssi) {
                    aps_[k].rssi = WiFi.RSSI(i);
                    aps_[k].channel = static_cast<uint8_t>(WiFi.channel(i));
                }
                break;
            }
        }
        if (duplicate) continue;
        aps_[apCount_].ssid = ssid;
        aps_[apCount_].rssi = WiFi.RSSI(i);
        aps_[apCount_].channel = static_cast<uint8_t>(WiFi.channel(i));
        aps_[apCount_].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        ++apCount_;
    }
}

int bestRssiFor(const String& ssid) {
    int best = -1000;
    for (int i = 0; i < apCount_; ++i) {
        if (aps_[i].ssid == ssid && aps_[i].rssi > best) best = aps_[i].rssi;
    }
    return best;
}

// 笔记本的挑网逻辑，缩到 5 张表：先看谁在眼前，再按信号排队；
// 一张都看不见时才退化成「盲试上次那张」（隐藏 SSID 只能这么连）。
void buildCandidates() {
    candidateCount_ = 0;
    candidatePos_ = 0;

    struct Picked {
        int index;
        int rssi;
    } visible[kMaxNetworks];
    size_t visibleCount = 0;

    for (size_t i = 0; i < count_; ++i) {
        const int rssi = bestRssiFor(nets_[i].ssid);
        if (rssi > -1000) visible[visibleCount++] = {static_cast<int>(i), rssi};
    }
    // 插入排序：表最多 5 张，不值得为它引一个算法头。
    for (size_t i = 1; i < visibleCount; ++i) {
        Picked key = visible[i];
        size_t j = i;
        while (j > 0 && visible[j - 1].rssi < key.rssi) {
            visible[j] = visible[j - 1];
            --j;
        }
        visible[j] = key;
    }
    for (size_t i = 0; i < visibleCount; ++i) candidates_[candidateCount_++] = visible[i].index;

    if (candidateCount_ == 0) {
        const int last = indexOf(lastSsid_);
        candidates_[candidateCount_++] = last >= 0 ? last : 0;
        Serial.println("[wifi] no saved network visible, blind-trying last one");
    }
    Serial.printf("[wifi] candidates=%u\n", static_cast<unsigned>(candidateCount_));
}

bool networkSecurityMatches(int index) {
    // 只在扫描结果还在时用；查不到就当未知，交给驱动判断。
    for (int i = 0; i < apCount_; ++i) {
        if (aps_[i].ssid == nets_[index].ssid) {
            const bool needsPass = aps_[i].secure;
            if (needsPass && nets_[index].pass.isEmpty()) {
                Serial.printf("[wifi] \"%s\" needs a password, saved entry is empty\n",
                              nets_[index].ssid.c_str());
                return false;
            }
            return true;
        }
    }
    return true;
}

void onConnected(uint32_t now) {
    phase_ = Phase::Online;
    lostAt_ = 0;
    const String ipText = WiFi.localIP().toString();
    Serial.printf("[wifi] connected ssid=\"%s\" ip=%s rssi=%d\n", WiFi.SSID().c_str(),
                  ipText.c_str(), static_cast<int>(WiFi.RSSI()));
    ui::logf("{\"ev\":\"wifi_ok\",\"ssid\":\"%s\",\"ip\":\"%s\"}", WiFi.SSID().c_str(),
             ipText.c_str());

    // TC3 签名要求时钟误差 ≤5 分钟，开机连上就得对时；
    // 运行时配网时 cloudasr::begin() 早跑过了，这里是唯一一次补对时的机会。
    configTime(0, 0, "ntp.tencent.com", "ntp.aliyun.com", "cn.pool.ntp.org");

    rememberLast(attempt_);
    (void)now;
}

void nextCandidate(uint32_t now) {
    while (candidatePos_ < candidateCount_) {
        const int index = candidates_[candidatePos_++];
        if (!networkSecurityMatches(index)) continue;
        beginConnect(index, now);
        return;
    }
    Serial.println("[wifi] all saved networks failed, retry in 30 s");
    enterBackoff(now, kRetryIntervalMs);
}

}  // namespace

// ------------------------------------------------------------------ 对外 ---
void begin() {
    loadNets();
    if (count_ == 0) {
        Serial.println("[wifi] no credentials: 用手机连 LAMP-1-xxxx 热点配网，或串口 wifi connect");
        phase_ = Phase::Idle;
        return;
    }
    Serial.printf("[wifi] %u saved network(s), last=\"%s\"\n", static_cast<unsigned>(count_),
                  lastSsid_.c_str());
}

void start() {
    if (started_) return;
    started_ = true;
    startedAt_ = millis();
    if (count_ == 0) return;
    // 快路径：上次连上的那张。搬家/换办公室时它会失败，随后自动转扫描。
    const int last = indexOf(lastSsid_);
    beginConnect(last >= 0 ? last : 0, startedAt_);
}

void tick(uint32_t now) {
    if (count_ == 0) return;

    switch (phase_) {
        case Phase::Idle:
            if (started_) enterBackoff(now, 0);
            break;

        case Phase::Online:
            if (WiFi.status() == WL_CONNECTED) {
                lostAt_ = 0;
                break;
            }
            // 掉线先等几秒：路由器重启、信道切换都会短暂断开，
            // 立刻重扫既慢又容易把一次可自愈的抖动放大成一轮重连。
            if (lostAt_ == 0) {
                lostAt_ = now;
            } else if (now - lostAt_ >= kLostGraceMs) {
                Serial.println("[wifi] link lost, re-selecting");
                ui::logf("{\"ev\":\"wifi_lost\"}");
                lostAt_ = 0;
                enterBackoff(now, 500);
            }
            break;

        case Phase::Connecting: {
            const wl_status_t st = WiFi.status();
            if (st == WL_CONNECTED) {
                onConnected(now);
                break;
            }
            const uint32_t elapsed = now - attemptStart_;
            // 密码错、SSID 不在空口里都会在几秒内返回，没必要每张都等满 12 秒。
            const bool authFailed = (st == WL_CONNECT_FAILED && elapsed > 3000);
            const bool notFound = (st == WL_NO_SSID_AVAIL && elapsed > 6000);
            if (authFailed || notFound || now >= deadline_) {
                Serial.printf("[wifi] \"%s\" failed: %s\n", nets_[attempt_].ssid.c_str(),
                              authFailed   ? "auth"
                              : notFound   ? "no-ssid"
                                           : "timeout");
                WiFi.disconnect(!netconsole::apActive());
                delay(30);
                // 快路径（还没扫过）失败时别先睡 30 秒：搬家后第一次开机的
                // 典型场景就是「上次那张不在眼前」，应该立刻扫一遍。
                if (candidateCount_ == 0) {
                    startScan(now);
                } else {
                    nextCandidate(now);
                }
            }
            break;
        }

        case Phase::Scanning: {
            const int n = WiFi.scanComplete();
            if (n == WIFI_SCAN_RUNNING) {
                if (now >= deadline_) {
                    WiFi.scanDelete();
                    Serial.println("[wifi] scan timeout");
                    enterBackoff(now, kRetryIntervalMs);
                }
                break;
            }
            if (n == WIFI_SCAN_FAILED || n < 0) {
                Serial.println("[wifi] scan failed");
                WiFi.scanDelete();
                enterBackoff(now, kRetryIntervalMs);
                break;
            }
            copyScan(n);
            WiFi.scanDelete();
            Serial.printf("[wifi] scan: %d AP(s)\n", apCount_);
            buildCandidates();
            nextCandidate(now);
            break;
        }

        case Phase::Backoff:
            if (now >= nextTryAt_) startScan(now);
            break;
    }
}

bool hasCredentials() { return count_ > 0; }

size_t savedCount() { return count_; }

String savedSsid(size_t index) {
    if (index >= count_) return String();
    return nets_[index].ssid;
}

int lastIndex() { return indexOf(lastSsid_); }

bool connect(const String& ssid, const String& pass) {
    if (!validSsid(ssid)) {
        Serial.println("[wifi] bad ssid: empty or longer than 32 bytes");
        return false;
    }
    if (!validPass(pass)) {
        Serial.println("[wifi] bad password: longer than 63 bytes");
        return false;
    }

    int index = indexOf(ssid);
    if (index < 0) {
        if (count_ < kMaxNetworks) {
            index = static_cast<int>(count_++);
        } else {
            // 表满：换掉最久没用的第一张。lastSsid_ 指向的那张不动。
            index = 0;
            if (nets_[0].ssid == lastSsid_ && count_ > 1) index = 1;
            Serial.printf("[wifi] table full, replacing \"%s\"\n", nets_[index].ssid.c_str());
        }
        nets_[index].ssid = ssid;
    }
    nets_[index].pass = pass;
    saveNets();

    // 只报长度和有没有密码，密码本身绝不进串口/日志/网页。
    Serial.printf("[wifi] saved ssid=\"%s\" password=%s\n", ssid.c_str(),
                  pass.isEmpty() ? "empty" : "set");
    ui::logf("{\"ev\":\"wifi_saved\",\"ssid\":\"%s\"}", ssid.c_str());

    lastSsid_ = ssid;
    // 用户手动挑的网直接连，不走 12 秒快路径超时。
    beginConnect(index, millis());
    return true;
}

bool connectSaved(size_t index) {
    if (index >= count_) {
        Serial.printf("[wifi] no saved network #%u\n", static_cast<unsigned>(index + 1));
        return false;
    }
    lastSsid_ = nets_[index].ssid;
    Serial.printf("[wifi] selected \"%s\" from saved list\n", nets_[index].ssid.c_str());
    beginConnect(static_cast<int>(index), millis());
    return true;
}

bool forget(const String& ssid) {
    const int index = indexOf(ssid);
    if (index < 0) return false;
    const bool active = (attempt_ == index) || (WiFi.status() == WL_CONNECTED && WiFi.SSID() == ssid);

    for (size_t i = index; i + 1 < count_; ++i) nets_[i] = nets_[i + 1];
    --count_;
    nets_[count_].ssid = "";
    nets_[count_].pass = "";
    saveNets();

    if (lastSsid_ == ssid) {
        lastSsid_ = count_ > 0 ? nets_[0].ssid : "";
        Preferences prefs;
        if (prefs.begin(kNamespace, false)) {
            prefs.putString("last", lastSsid_);
            prefs.end();
        }
    }
    Serial.printf("[wifi] forgot \"%s\"\n", ssid.c_str());
    ui::logf("{\"ev\":\"wifi_forgot\",\"ssid\":\"%s\"}", ssid.c_str());

    if (active) {
        WiFi.disconnect(!netconsole::apActive());
        if (count_ > 0) {
            enterBackoff(millis(), 300);
        } else {
            phase_ = Phase::Idle;
            attempt_ = -1;
        }
    }
    return true;
}

void clearAll() {
    Preferences prefs;
    if (prefs.begin(kNamespace, false)) {
        prefs.clear();
        prefs.end();
    }
    // 只清 RAM 里的表；secrets.h 的编译期凭据仍然算数（开发板兜底）。
    count_ = 0;
    lastSsid_ = "";
    if (validSsid(String(LAMP_WIFI_SSID))) {
        nets_[0].ssid = LAMP_WIFI_SSID;
        nets_[0].pass = LAMP_WIFI_PASS;
        count_ = 1;
        lastSsid_ = nets_[0].ssid;
    }
    WiFi.disconnect(!netconsole::apActive());
    phase_ = Phase::Idle;
    attempt_ = -1;
    Serial.println(count_ > 0 ? "[wifi] cleared, falling back to secrets.h"
                              : "[wifi] cleared, no credentials left");
    ui::logf("{\"ev\":\"wifi_cleared\"}");
    if (count_ > 0) {
        // start() 有幂等保护（只跑第一次），清空后要重新放行。
        started_ = false;
        start();
    }
}

void autoConnectNow() {
    if (count_ == 0) return;
    Serial.println("[wifi] re-selecting now");
    if (WiFi.status() == WL_CONNECTED) WiFi.disconnect(!netconsole::apActive());
    startScan(millis());
}

int scan(uint32_t timeoutMs) {
    // 用户主动扫描：先停掉正在进行的尝试，避免和 begin() 抢射频（返回 -2）。
    if (phase_ == Phase::Connecting) {
        WiFi.disconnect(!netconsole::apActive());
        phase_ = Phase::Backoff;
        nextTryAt_ = millis() + 800;
    }
    WiFi.scanDelete();
    const int rc = WiFi.scanNetworks(true, true);
    if (rc == WIFI_SCAN_FAILED) return -1;

    const uint32_t until = millis() + timeoutMs;
    int n = WIFI_SCAN_RUNNING;
    while (static_cast<int32_t>(millis() - until) < 0) {
        n = WiFi.scanComplete();
        if (n != WIFI_SCAN_RUNNING) break;
        delay(25);
    }
    if (n < 0) {
        WiFi.scanDelete();
        return -1;
    }
    copyScan(n);
    WiFi.scanDelete();
    // 用户扫完不等于「只想看看」：还没联网时顺手用这份结果继续自动选网，
    // 否则页面停在「有列表但设备不动」，得再点一次网络。
    if (!online() && count_ > 0) {
        buildCandidates();
        nextCandidate(millis());
    }
    return apCount_;
}

int scanCount() { return apCount_; }

String scanSsid(int index) {
    if (index < 0 || index >= apCount_) return String();
    return aps_[index].ssid;
}

int scanRssi(int index) {
    if (index < 0 || index >= apCount_) return 0;
    return aps_[index].rssi;
}

int scanChannel(int index) {
    if (index < 0 || index >= apCount_) return 0;
    return aps_[index].channel;
}

bool scanSecure(int index) {
    if (index < 0 || index >= apCount_) return false;
    return aps_[index].secure;
}

int scanKnownIndex(int index) {
    if (index < 0 || index >= apCount_) return -1;
    return indexOf(aps_[index].ssid);
}

bool online() { return WiFi.status() == WL_CONNECTED; }

String activeSsid() {
    if (!online()) return String();
    return WiFi.SSID();
}

String ip() {
    if (online()) return WiFi.localIP().toString();
    if (netconsole::apActive()) return WiFi.softAPIP().toString();
    return String();
}

int rssi() { return online() ? static_cast<int>(WiFi.RSSI()) : 0; }

const char* phase() {
    switch (phase_) {
        case Phase::Idle:       return "idle";
        case Phase::Connecting: return "connecting";
        case Phase::Scanning:   return "scanning";
        case Phase::Online:     return "online";
        case Phase::Backoff:    return "retry";
    }
    return "idle";
}

bool busy() { return phase_ == Phase::Connecting || phase_ == Phase::Scanning; }

String summary() {
    String out;
    if (count_ == 0) {
        out = "wifi=unconfigured";
        return out;
    }
    if (online()) {
        out = String("wifi=up ssid=\"") + WiFi.SSID() + "\" ip=" + WiFi.localIP().toString() +
              " rssi=" + String(WiFi.RSSI());
    } else {
        out = String("wifi=down phase=") + phase();
        if (attempt_ >= 0) out += String(" trying=\"") + nets_[attempt_].ssid + "\"";
    }
    out += String(" saved=") + String(static_cast<unsigned>(count_));
    if (!lastSsid_.isEmpty()) out += String(" last=\"") + lastSsid_ + "\"";
    return out;
}

}  // namespace wifilink
