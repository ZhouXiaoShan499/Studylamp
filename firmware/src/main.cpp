// LAMP-1 MVP firmware: IR learn/replay + dual-CCT lighting + timers + motion.
// Single non-blocking loop; no delay() outside of short hardware sequences.
// See docs/03-技术方案.md for the design rationale behind each module.

#include <Arduino.h>
#include <Esp.h>
#include <LittleFS.h>
#include <sys/time.h>

#include "cloud_asr.h"
#include "config.h"
#include "ir_engine.h"
#include "ir_store.h"
#include "lighting.h"
#include "motion.h"
#include "pdm_mic.h"
#include "pwm.h"
#include "tc3_sign.h"
#include "timer_task.h"
#include "ui.h"
#include "voice_client.h"
#include "voice_uart.h"

#include <Preferences.h>
#include <WiFi.h>
#include <time.h>

namespace {

// ---------------------------------------------------------------- buttons ---
enum class BtnEvent : uint8_t { None, Short, Long };

struct Button {
    uint8_t pin;
    bool stable;
    bool lastRead;
    bool longFired;
    uint32_t lastChange;
    uint32_t pressedAt;
};

Button btnMode{PIN_BTN_MODE, false, false, false, 0, 0};
Button btnLight{PIN_BTN_LIGHT, false, false, false, 0, 0};
Button btnLearn{PIN_BTN_LEARN, false, false, false, 0, 0};

// 按键有效电平：HW-483 这类模块有上拉和下拉两种版本，接反了就是"永远按住"或"完全没反应"。
// 由 config.h 的 LAMP_BTN_ACTIVE_LOW 决定，不用改接线以外的任何逻辑。
inline bool buttonLevelPressed(int level) {
#if LAMP_BTN_ACTIVE_LOW
    return level == LOW;
#else
    return level == HIGH;
#endif
}

void buttonInit(Button& button, uint8_t pin) {
    button.pin = pin;
#if LAMP_BTN_ACTIVE_LOW
    pinMode(pin, INPUT_PULLUP);
#else
    pinMode(pin, INPUT);  // 模块自带下拉，开内部上拉会把电平拉死
#endif
    button.stable = buttonLevelPressed(digitalRead(pin));
    button.lastRead = button.stable;
    button.longFired = false;
    button.lastChange = millis();
    button.pressedAt = 0;
}

BtnEvent buttonPoll(Button& button, uint32_t now, uint16_t longMs) {
    const bool raw = buttonLevelPressed(digitalRead(button.pin));
    if (raw != button.lastRead) {
        button.lastRead = raw;
        button.lastChange = now;
    }
    if ((now - button.lastChange) >= BTN_DEBOUNCE_MS && raw != button.stable) {
        button.stable = raw;
        if (button.stable) {
            button.pressedAt = now;
            button.longFired = false;
        } else {
            if (button.longFired) {
                button.longFired = false;
                return BtnEvent::None;
            }
            return BtnEvent::Short;
        }
    }
    if (button.stable && !button.longFired && (now - button.pressedAt) >= longMs) {
        button.longFired = true;
        return BtnEvent::Long;
    }
    return BtnEvent::None;
}

// ------------------------------------------------------------------ buzzer ---
uint8_t beepLeft = 0;
bool beepOn = false;
uint32_t beepPhaseEnd = 0;

void beepRequest(uint8_t count) {
    if (count > 3) count = 3;
    beepLeft = count;
    beepOn = false;
    beepPhaseEnd = 0;
}

void beepTick(uint32_t now) {
    if (beepLeft == 0) {
        if (beepOn) {
            pwm::write(PIN_BUZZER, CH_BUZZER, 0);
            beepOn = false;
        }
        return;
    }
    if (now < beepPhaseEnd) return;
    if (beepOn) {
        pwm::write(PIN_BUZZER, CH_BUZZER, 0);
        beepOn = false;
        --beepLeft;
        beepPhaseEnd = now + 70;
    } else {
        pwm::write(PIN_BUZZER, CH_BUZZER, 1u << 9);  // ~50 % duty at 4 kHz
        beepOn = true;
        beepPhaseEnd = now + 80;
    }
}

// ------------------------------------------------------------- learn session ---
constexpr uint8_t kLearnSlots = 6;
bool learnActive = false;
uint8_t learnBase = 0;
uint8_t learnSlot = 0;

void exitLearn(const char* reason) {
    if (!learnActive) return;
    learnActive = false;
    irengine::cancelLearn();
    ui::setLearnHint("", -1);
    ui::logf("{\"ev\":\"learn_exit\",\"reason\":\"%s\"}", reason);
    ui::setStatus("ready");
}

void enterLearn(uint8_t base_slot) {
    if (base_slot + kLearnSlots > irstore::kSlots) {
        base_slot = irstore::kSlots - kLearnSlots;
    }
    learnBase = base_slot;
    learnSlot = base_slot;
    learnActive = true;
    char hint[24];
    snprintf(hint, sizeof(hint), "LEARN %u/%u", 1, kLearnSlots);
    ui::setLearnHint(hint, 0);
    ui::setStatus("learning");
    irengine::startLearn(learnSlot);
    motion::request(motion::Move::Nod);  // "I am listening"
}

void onLearnResult(uint8_t slot, bool ok, irengine::LearnError error, uint16_t len) {
    ui::logf("{\"ev\":\"learn_result\",\"slot\":%u,\"ok\":%d,\"reason\":\"%s\",\"len\":%u}",
             static_cast<unsigned>(slot), ok ? 1 : 0, irengine::errorText(error),
             static_cast<unsigned>(len));

    if (ok) {
        beepRequest(1);
        const uint8_t next = static_cast<uint8_t>(slot + 1);
        if (next >= learnBase + kLearnSlots) {
            ui::setLearnHint("SAVED", 100);
            exitLearn("complete");
            motion::request(motion::Move::Nod);
            return;
        }
        learnSlot = next;
        char hint[24];
        snprintf(hint, sizeof(hint), "LEARN %u/%u", static_cast<unsigned>(next - learnBase + 1),
                 static_cast<unsigned>(kLearnSlots));
        ui::setLearnHint(hint, static_cast<int8_t>(((next - learnBase) * 100) / kLearnSlots));
        irengine::startLearn(next);
        return;
    }

    if (error == irengine::LearnError::Timeout) {
        exitLearn("timeout");
        return;
    }

    beepRequest(2);
    ui::setLearnHint(irengine::errorText(error), -1);
    // Retry the same slot: the most common causes are distance, angle and a
    // weak remote battery, all of which the user can fix in two seconds.
    irengine::startLearn(slot);
}

// --------------------------------------------------------------- night mode ---
bool isNightNow() {
    if (!timers::timeValid()) return false;
    const uint32_t minuteOfDay = (timers::epoch() / 60) % 1440;
    return minuteOfDay >= (22 * 60) || minuteOfDay < (7 * 60);
}

// ------------------------------------------------------------------- actions ---
bool sendIrSlot(uint16_t slot) {
    // Drop the LED to 70 % while the IR burst runs: 240 mA on top of a 1 A LED
    // load dips the 5 V rail and can brown-out the ESP32 (docs/03 section 4.1).
    const bool wasOn = lighting::isOn();
    if (wasOn) {
        lighting::setScale(0.7f);
        lighting::flush();
    }
    const bool ok = irengine::send(static_cast<uint8_t>(slot));
    if (wasOn) {
        lighting::clearScale();
        lighting::flush();
    }
    ui::logf("{\"ev\":\"ir_send\",\"slot\":%u,\"ok\":%d}", static_cast<unsigned>(slot), ok ? 1 : 0);
    if (ok) beepRequest(1);
    return ok;
}

void applyAction(uint8_t action, uint16_t irSlot) {
    if (action & timers::ActionLightOn) lighting::setBrightness(70);
    if (action & timers::ActionLightOff) lighting::setBrightness(0);
    if (action & timers::ActionIr) sendIrSlot(irSlot);
    if (action & timers::ActionMotion) motion::request(motion::Move::Alert);
}

void onTimerFire(const timers::Task& task) {
    ui::logf("{\"ev\":\"timer_fire\",\"id\":%u,\"action\":%u,\"night\":%d}",
             static_cast<unsigned>(task.id), task.action, isNightNow() ? 1 : 0);
    motion::setNightMode(isNightNow());
    applyAction(task.action, task.ir_slot);
    beepRequest(1);
}

// ------------------------------------------------------------------- buttons ---
void onLightButton(BtnEvent event) {
    if (event == BtnEvent::Short) {
        // 短按 = 开/关。之前是"25→50→75→100 循环"，永远关不掉，用户会以为按键坏了。
        const bool turnOn = !lighting::isOn();
        const uint8_t next = turnOn ? 60 : 0;
        lighting::setBrightness(next);
        ui::logf("{\"ev\":\"light_set\",\"pct\":%u,\"src\":\"button\"}", next);
    } else if (event == BtnEvent::Long) {
        static const lighting::Preset cycle[] = {lighting::Preset::Study, lighting::Preset::Office,
                                                 lighting::Preset::Rest, lighting::Preset::Night};
        static uint8_t index = 0;
        index = static_cast<uint8_t>((index + 1) % (sizeof(cycle) / sizeof(cycle[0])));
        lighting::setPreset(cycle[index]);
        ui::logf("{\"ev\":\"light_preset\",\"name\":\"%s\"}", lighting::presetName(cycle[index]));
    }
}

// ------------------------------------------------------------------- voice ---
// 状态灯（HW-479 的 B 通道，GPIO14）短亮一下，作为"听到了"的确认。
// 没有蜂鸣器时这是唯一的非视觉外的反馈手段，成本为零。
uint32_t statusPulseUntil = 0;

#if LAMP_VOICE_UART
void statusPulse(uint16_t ms) { statusPulseUntil = millis() + ms; }

uint8_t voiceLastOnPct = 70;  // 「开灯」要能回到关灯前的亮度

void onVoiceCommand(uint8_t command) {
    // 夜间（22:00-07:00）只放行"关灯"：电视里一句"开灯"不该把灯点着。
    if (VOICE_NIGHT_GUARD && command != voiceuart::kLightOff && isNightNow()) {
        Serial.printf("[voice] cmd=%u ignored (night)\n", static_cast<unsigned>(command));
        return;
    }

    switch (command) {
        case voiceuart::kLightOn:
            lighting::setBrightness(voiceLastOnPct == 0 ? 70 : voiceLastOnPct);
            break;
        case voiceuart::kLightOff:
            if (lighting::isOn()) voiceLastOnPct = lighting::brightness();
            lighting::setBrightness(0);
            break;
        case voiceuart::kBrighter: {
            const uint16_t next = static_cast<uint16_t>(lighting::brightness()) + 20;
            lighting::setBrightness(static_cast<uint8_t>(next > 100 ? 100 : next));
            break;
        }
        case voiceuart::kDimmer: {
            const int next = static_cast<int>(lighting::brightness()) - 20;
            lighting::setBrightness(static_cast<uint8_t>(next < 5 ? 5 : next));
            break;
        }
        case voiceuart::kMax:
            lighting::setBrightness(100);
            break;
        case voiceuart::kNight:
            lighting::setPreset(lighting::Preset::Night);
            break;
        case voiceuart::kStudy:
            lighting::setPreset(lighting::Preset::Study);
            break;
        case voiceuart::kRest:
            lighting::setPreset(lighting::Preset::Rest);
            break;
        default:
            Serial.printf("[voice] unknown cmd=%u\n", static_cast<unsigned>(command));
            return;
    }

    statusPulse(150);
    ui::logf("{\"ev\":\"voice_cmd\",\"id\":%u,\"on\":%d,\"pct\":%u}", static_cast<unsigned>(command),
             lighting::isOn() ? 1 : 0, lighting::brightness());
}
#endif

void onModeButton(BtnEvent event) {
    if (event == BtnEvent::Short) {
        sendIrSlot(0);  // slot 0 is the primary key, normally the AC power toggle
        motion::request(motion::Move::Nod);
    } else if (event == BtnEvent::Long) {
        motion::setMuted(!motion::muted());
        ui::logf("{\"ev\":\"motion_mute\",\"muted\":%d}", motion::muted() ? 1 : 0);
        ui::setStatus(motion::muted() ? "motion muted" : "motion on");
        beepRequest(1);
    }
}

void onLearnButton(BtnEvent event) {
    if (learnActive) {
        if (event == BtnEvent::Short) {
            exitLearn("user");
        }
        return;
    }
    if (event == BtnEvent::Long) {
        // Start on the first slot that is still empty, so re-learning does not
        // silently overwrite a working key.
        uint8_t base = 0;
        while (base < irstore::kSlots && irstore::exists(base)) ++base;
        if (base >= irstore::kSlots - kLearnSlots) base = 0;
        enterLearn(base);
    } else if (event == BtnEvent::Short) {
        ui::setStatus(irstore::usedSlots() == 0 ? "no keys: hold LEARN" : "ready");
    }
}

void handleButtons(uint32_t now) {
    const BtnEvent light = buttonPoll(btnLight, now, BTN_LONGPRESS_MS);
    if (light != BtnEvent::None) onLightButton(light);

    const BtnEvent mode = buttonPoll(btnMode, now, BTN_LONGPRESS_MS);
    if (mode != BtnEvent::None) onModeButton(mode);

    const BtnEvent learn = buttonPoll(btnLearn, now, BTN_LEARN_LONGPRESS_MS);
    if (learn != BtnEvent::None) onLearnButton(learn);
}

// --------------------------------------------------------------------- status ---
void statusTick(uint32_t now) {
    static uint32_t lastBlink = 0;
    static bool ledOn = false;
    if (learnActive) {
        if (now - lastBlink >= 200) {
            lastBlink = now;
            ledOn = !ledOn;
            digitalWrite(PIN_STATUS_LED, ledOn ? HIGH : LOW);
        }
    } else if (statusPulseUntil != 0) {
        // 语音确认：短亮 150 ms。放在 learn 之后、常灭之前，避免和学码闪烁打架。
        if (static_cast<int32_t>(statusPulseUntil - now) > 0) {
            digitalWrite(PIN_STATUS_LED, HIGH);
        } else {
            digitalWrite(PIN_STATUS_LED, LOW);
            statusPulseUntil = 0;
            ledOn = false;
        }
    } else if (ledOn) {
        ledOn = false;
        digitalWrite(PIN_STATUS_LED, LOW);
    }
    motion::setNightMode(isNightNow());
}

// ------------------------------------------------------------------- WiFi ---
// 凭据来源：NVS（网页「连接 WiFi」或串口 `wifi connect` 写入）优先；
// NVS 为空时回落到 secrets.h 的编译期常量。换热点、改密码不用重新烧固件。
constexpr const char* kWifiNvsNamespace = "lampwifi";

String wifiSsid;
String wifiPass;
bool wifiFromNvs = false;
bool wifiStarted = false;
bool wifiScanned = false;
bool wifiAnnounced = false;
bool wifiNoCredsWarned = false;
uint32_t wifiLastAttempt = 0;
uint32_t wifiLastSync = 0;

// 连不上时把真实原因打出来。只看到 wifi=down 分不清是"热点不在 2.4 GHz"
// 还是"密码错"——这两种的处理方式完全不同。
const char* wifiStatusName(wl_status_t s) {
    switch (s) {
        case WL_NO_SSID_AVAIL:   return "no-ssid: 扫不到这个 SSID（热点多半在 5 GHz 或太远）";
        case WL_CONNECT_FAILED:  return "auth-failed: 密码或加密方式不对";
        case WL_CONNECTION_LOST: return "conn-lost: 连上又断了";
        case WL_DISCONNECTED:    return "disconnected: 重连中";
        case WL_IDLE_STATUS:     return "idle";
        default:                 return "other";
    }
}

int wifiHexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 网页用 encodeURIComponent() 编码 SSID/密码：空格、引号、中文都不会把命令行拆坏。
// 只解码 %XX，不把 '+' 当空格——encodeURIComponent 本身会把 '+' 编成 %2B。
String wifiUrlDecode(const char* text) {
    String out;
    if (text == nullptr) return out;
    for (const char* p = text; *p != '\0'; ++p) {
        if (*p == '%' && p[1] != '\0' && p[2] != '\0') {
            const int hi = wifiHexDigit(p[1]);
            const int lo = wifiHexDigit(p[2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                p += 2;
                continue;
            }
        }
        out += *p;
    }
    return out;
}

void wifiPrintStatus() {
    const wl_status_t st = WiFi.status();
    if (st == WL_CONNECTED) {
        Serial.printf("[wifi] connected ssid=\"%s\" ip=%s rssi=%d\n", WiFi.SSID().c_str(),
                      WiFi.localIP().toString().c_str(), static_cast<int>(WiFi.RSSI()));
    } else {
        Serial.printf("[wifi] status=%d %s\n", static_cast<int>(st), wifiStatusName(st));
    }
    if (wifiSsid.isEmpty()) {
        Serial.println("[wifi] credentials none");
    } else {
        Serial.printf("[wifi] credentials ssid=\"%s\" source=%s password=%s\n", wifiSsid.c_str(),
                      wifiFromNvs ? "nvs" : "secrets.h", wifiPass.isEmpty() ? "empty" : "set");
    }
}

void wifiLoadCredentials() {
    Preferences prefs;
    if (prefs.begin(kWifiNvsNamespace, true)) {
        wifiSsid = prefs.getString("ssid", "");
        wifiPass = prefs.getString("pass", "");
        prefs.end();
    }

    wifiFromNvs = !wifiSsid.isEmpty();
    if (!wifiFromNvs) {
        wifiSsid = LAMP_WIFI_SSID;
        wifiPass = LAMP_WIFI_PASS;
    }

    if (wifiSsid.isEmpty()) {
        wifiNoCredsWarned = true;
        Serial.println("[wifi] no credentials: send 'wifi connect <ssid> <pass>' or write include/secrets.h");
    } else {
        Serial.printf("[wifi] credentials ssid=\"%s\" source=%s password=%s\n", wifiSsid.c_str(),
                      wifiFromNvs ? "nvs" : "secrets.h", wifiPass.isEmpty() ? "empty" : "set");
    }
}

void wifiStartConnect() {
    if (wifiSsid.isEmpty()) {
        Serial.println("[wifi] no credentials: send 'wifi connect <ssid> <pass>'");
        return;
    }
    WiFi.disconnect(true);
    delay(50);  // 等射频复位；紧接着 begin() 才不会继承上一轮的失败状态
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());  // non-blocking
    wifiStarted = true;
    wifiScanned = false;
    wifiAnnounced = false;
    wifiNoCredsWarned = false;
    wifiLastAttempt = millis();
    Serial.printf("[wifi] connecting ssid=\"%s\"\n", wifiSsid.c_str());
}

bool wifiSaveCredentials(const String& ssid, const String& pass) {
    if (ssid.isEmpty()) {
        Serial.println("[wifi] bad ssid: empty");
        return false;
    }
    if (ssid.length() > 32) {
        Serial.println("[wifi] bad ssid: longer than 32 bytes");
        return false;
    }
    if (pass.length() > 63) {
        Serial.println("[wifi] bad password: longer than 63 bytes");
        return false;
    }

    Preferences prefs;
    if (!prefs.begin(kWifiNvsNamespace, false)) {
        Serial.println("[wifi] bad nvs: cannot open namespace");
        return false;
    }
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.end();

    wifiSsid = ssid;
    wifiPass = pass;
    wifiFromNvs = true;
    // 只报长度和有没有密码，密码本身绝不进串口/日志。
    Serial.printf("[wifi] saved ssid=\"%s\" password=%s\n", ssid.c_str(),
                  pass.isEmpty() ? "empty" : "set");
    wifiStartConnect();
    return true;
}

void wifiClearCredentials() {
    Preferences prefs;
    if (prefs.begin(kWifiNvsNamespace, false)) {
        prefs.remove("ssid");
        prefs.remove("pass");
        prefs.end();
    }

    wifiFromNvs = false;
    wifiSsid = LAMP_WIFI_SSID;
    wifiPass = LAMP_WIFI_PASS;
    wifiStarted = false;
    wifiScanned = false;
    wifiAnnounced = false;
    WiFi.disconnect(true);
    Serial.println("[wifi] cleared");

    if (wifiSsid.isEmpty()) {
        wifiNoCredsWarned = false;
    } else {
        Serial.printf("[wifi] falling back to secrets.h ssid=\"%s\"\n", wifiSsid.c_str());
        wifiStartConnect();
    }
}

int wifiScanAndPrint() {
    WiFi.scanDelete();
    const int n = WiFi.scanNetworks(false, true);
    if (n < 0) {
        Serial.printf("[wifi] scan failed code=%d\n", n);
        WiFi.scanDelete();
        return n;
    }
    Serial.printf("[wifi] scan: %d AP(s) visible\n", n);
    for (int i = 0; i < n && i < 20; ++i) {
        const String ssid = WiFi.SSID(i);
        const bool hidden = ssid.isEmpty();
        Serial.printf("[wifi]   %d dBm ch%-2d %s%s\n", static_cast<int>(WiFi.RSSI(i)),
                      static_cast<int>(WiFi.channel(i)), hidden ? "<hidden>" : ssid.c_str(),
                      (!hidden && ssid == wifiSsid) ? "  <== match" : "");
    }
    WiFi.scanDelete();
    Serial.println("[wifi] scan done");
    return n;
}

void wifiScanNow() {
    const bool wasConnected = (WiFi.status() == WL_CONNECTED);
    if (!wasConnected) {
        // 未连接时必须先彻底断开，否则扫描会和后台的 begin() 抢射频（返回 -2）。
        WiFi.disconnect(true);
        delay(80);
    }
    wifiScanAndPrint();
    // 扫描可能短暂打断射频：扫完还没连上就立刻重连，别让页面停在断网状态。
    if (WiFi.status() != WL_CONNECTED && !wifiSsid.isEmpty()) wifiStartConnect();
}

void wifiTick(uint32_t now) {
    if (wifiSsid.isEmpty()) {
        if (!wifiNoCredsWarned) {
            wifiNoCredsWarned = true;
            Serial.println("[wifi] no credentials: send 'wifi connect <ssid> <pass>' or write include/secrets.h");
        }
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {
        wifiAnnounced = false;
        if (now - wifiLastAttempt >= 10000) {
            wifiLastAttempt = now;

            if (wifiStarted) {
                const wl_status_t st = WiFi.status();
                Serial.printf("[wifi] status=%d %s\n", static_cast<int>(st), wifiStatusName(st));
            }

            // 第一次重试时扫一遍空口，把能看到的 AP 列出来（含是否命中目标 SSID）。
            // 关键：必须先把 WiFi 彻底断开、停掉后台连接，否则 scanNetworks() 会跟
            // begin() 抢射频，直接返回 -2 (SCAN_FAILED)，一个 AP 都列不出来。
            if (wifiStarted && !wifiScanned) {
                wifiScanned = true;
                WiFi.disconnect(true);   // 关闭射频，释放出来给扫描用
                delay(80);               // 等射频真正复位
                wifiScanAndPrint();
            }

            WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());  // non-blocking
        }
        return;
    }

    // 连上的那一刻打一条，否则日志里只有 wifi_begin 没有结果，看不出到底通没通。
    if (!wifiAnnounced) {
        wifiAnnounced = true;
        Serial.printf("[wifi] connected ssid=\"%s\" ip=%s rssi=%d\n", WiFi.SSID().c_str(),
                      WiFi.localIP().toString().c_str(), static_cast<int>(WiFi.RSSI()));
        ui::logf("{\"ev\":\"wifi_ok\",\"ip\":\"%s\"}", WiFi.localIP().toString().c_str());
        // 运行时才配网时 cloudasr::begin() 早已跑过，SNTP 没踢出去；
        // 这里补一次：TC3 签名要求时钟误差 ≤5 分钟，时钟不对第一跳必失败。
        configTime(0, 0, "ntp.tencent.com", "ntp.aliyun.com", "cn.pool.ntp.org");
    }
    if (now - wifiLastSync >= 5000) {
        wifiLastSync = now;
        const time_t value = time(nullptr);
        if (value > 1600000000L) {
            const uint32_t epoch = static_cast<uint32_t>(value);
            if (!timers::timeValid() || (timers::epoch() > epoch + 5) || (timers::epoch() + 5 < epoch)) {
                timers::setEpoch(epoch);
            }
        }
    }
}

// ------------------------------------------------------------- command shell ---
uint32_t micMonitorUntil = 0;

void printStatusReport() {
    Serial.printf("[status] fw=%s uptime=%lus heap=%lu\n", FW_VERSION,
                  static_cast<unsigned long>(millis() / 1000UL),
                  static_cast<unsigned long>(ESP.getFreeHeap()));
    Serial.printf("[status] light=%s pct=%u cct=%uK preset=%s\n", lighting::isOn() ? "on" : "off",
                  lighting::brightness(), lighting::cct(), lighting::presetName(lighting::preset()));
    Serial.printf("[status] ir=%s slots=%u/%u\n", irengine::isLearning() ? "learning" : "idle",
                  static_cast<unsigned>(irstore::usedSlots()),
                  static_cast<unsigned>(irstore::kSlots));
    Serial.printf("[status] timers=%u clock=%s motion=%s\n", static_cast<unsigned>(timers::count()),
                  timers::timeValid() ? "synced" : "no-epoch", motion::muted() ? "muted" : "on");
    if (wifiSsid.isEmpty()) {
        Serial.println("[status] wifi=unconfigured");
    } else if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[status] wifi=up ssid=\"%s\" ip=%s\n", WiFi.SSID().c_str(),
                      WiFi.localIP().toString().c_str());
    } else {
        Serial.printf("[status] wifi=down status=%d\n", static_cast<int>(WiFi.status()));
    }
    voice::printStatus();
}

void dumpLogTail() {
    File file = LittleFS.open("/log/events.log", FILE_READ);
    if (!file) {
        Serial.println("[log] empty");
        return;
    }
    const size_t size = file.size();
    const size_t window = 1200;
    const size_t start = (size > window) ? size - window : 0;
    file.seek(start);
    Serial.printf("[log] tail %u of %u bytes\n", static_cast<unsigned>(size - start),
                  static_cast<unsigned>(size));
    while (file.available()) Serial.write(file.read());
    file.close();
}

bool parseSlot(const char* text, uint8_t* out) {
    if (text == nullptr) return false;
    char* end = nullptr;
    const long value = strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < 0 || value >= irstore::kSlots) return false;
    *out = static_cast<uint8_t>(value);
    return true;
}

void onCommand(int argc, char** argv) {
    const char* cmd = argv[0];

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        ui::printHelp();
        return;
    }
    if (strcmp(cmd, "status") == 0) {
        printStatusReport();
        return;
    }

    if (strcmp(cmd, "light") == 0) {
        if (argc < 2) {
            Serial.println("usage: light <0-100>");
            return;
        }
        const int pct = atoi(argv[1]);
        if (pct < 0 || pct > 100) {
            Serial.println("light: 0..100");
            return;
        }
        lighting::setBrightness(static_cast<uint8_t>(pct));
        ui::logf("{\"ev\":\"light_set\",\"pct\":%d,\"src\":\"cli\"}", pct);
        return;
    }
    if (strcmp(cmd, "cct") == 0) {
        if (argc < 2) {
            Serial.println("usage: cct <2700-6500>");
            return;
        }
        lighting::setCct(static_cast<uint16_t>(atoi(argv[1])));
        return;
    }
    if (strcmp(cmd, "mode") == 0) {
        if (argc < 2) {
            Serial.println("usage: mode study|office|rest|night");
            return;
        }
        const char* name = argv[1];
        lighting::Preset preset = lighting::Preset::Study;
        if (strcmp(name, "office") == 0) {
            preset = lighting::Preset::Office;
        } else if (strcmp(name, "rest") == 0) {
            preset = lighting::Preset::Rest;
        } else if (strcmp(name, "night") == 0) {
            preset = lighting::Preset::Night;
        } else if (strcmp(name, "study") != 0) {
            Serial.println("mode: study|office|rest|night");
            return;
        }
        lighting::setPreset(preset);
        ui::logf("{\"ev\":\"light_preset\",\"name\":\"%s\"}", lighting::presetName(preset));
        return;
    }

    if (strcmp(cmd, "learn") == 0) {
        if (argc >= 2 && (strcmp(argv[1], "cancel") == 0 || strcmp(argv[1], "stop") == 0)) {
            exitLearn("user");
            return;
        }
        if (learnActive) {
            Serial.println("learn already running, use 'learn cancel'");
            return;
        }
        uint8_t slot = 0;
        if (!parseSlot(argc >= 2 ? argv[1] : nullptr, &slot)) {
            Serial.println("usage: learn <slot 0-15> | learn cancel");
            return;
        }
        enterLearn(slot);
        return;
    }
    if (strcmp(cmd, "send") == 0) {
        uint8_t slot = 0;
        if (!parseSlot(argc >= 2 ? argv[1] : nullptr, &slot)) {
            Serial.println("usage: send <slot 0-15>");
            return;
        }
        sendIrSlot(slot);
        return;
    }
    if (strcmp(cmd, "slots") == 0) {
        Serial.printf("[slots] %u/%u used\n", static_cast<unsigned>(irstore::usedSlots()),
                      static_cast<unsigned>(irstore::kSlots));
        for (uint8_t i = 0; i < irstore::kSlots; ++i) {
            if (irstore::exists(i)) Serial.printf("  slot %u: stored\n", i);
        }
        return;
    }
    if (strcmp(cmd, "erase") == 0) {
        uint8_t slot = 0;
        if (!parseSlot(argc >= 2 ? argv[1] : nullptr, &slot)) {
            Serial.println("usage: erase <slot 0-15>");
            return;
        }
        const irstore::Result result = irstore::erase(slot);
        Serial.printf("erase %u: %s\n", slot, irstore::toString(result));
        return;
    }

    if (strcmp(cmd, "timer") == 0) {
        if (argc < 2) {
            Serial.println("usage: timer <seconds> [slot]");
            return;
        }
        const uint32_t seconds = strtoul(argv[1], nullptr, 10);
        uint8_t action = timers::ActionLightOn;
        uint16_t irSlot = 0;
        if (argc >= 3) {
            uint8_t slot = 0;
            if (!parseSlot(argv[2], &slot)) {
                Serial.println("timer: bad slot");
                return;
            }
            action |= timers::ActionIr;
            irSlot = slot;
        }
        const uint8_t id = timers::addCountdown(seconds, action, irSlot);
        if (id == 0xFF) {
            Serial.println("timer: no free task slot");
        } else {
            Serial.printf("timer #%u armed\n", id);
        }
        return;
    }
    if (strcmp(cmd, "daily") == 0) {
        if (argc < 2) {
            Serial.println("usage: daily HH:MM [slot]");
            return;
        }
        unsigned hour = 0, minute = 0;
        if (sscanf(argv[1], "%u:%u", &hour, &minute) != 2 || hour > 23 || minute > 59) {
            Serial.println("daily: HH:MM expected");
            return;
        }
        uint8_t action = timers::ActionLightOn;
        uint16_t irSlot = 0;
        if (argc >= 3) {
            uint8_t slot = 0;
            if (!parseSlot(argv[2], &slot)) {
                Serial.println("daily: bad slot");
                return;
            }
            action |= timers::ActionIr;
            irSlot = slot;
        }
        const uint8_t id = timers::addDaily(static_cast<uint16_t>(hour * 60 + minute),
                                           timers::EveryDay, action, irSlot);
        if (id == 0xFF) {
            Serial.println("daily: no free task slot");
        } else if (!timers::timeValid()) {
            Serial.printf("daily #%u stored, waiting for a clock sync\n", id);
        } else {
            Serial.printf("daily #%u armed for %02u:%02u\n", id, hour, minute);
        }
        return;
    }
    if (strcmp(cmd, "tasks") == 0) {
        const uint8_t count = timers::count();
        Serial.printf("[tasks] %u/%u\n", static_cast<unsigned>(count), static_cast<unsigned>(TASK_MAX));
        const timers::Task* tasks = timers::all();
        for (uint8_t i = 0; i < count; ++i) {
            Serial.printf("  %s\n", timers::describe(tasks[i]).c_str());
        }
        return;
    }
    if (strcmp(cmd, "del") == 0) {
        const int id = argc >= 2 ? atoi(argv[1]) : -1;
        const bool ok = (id >= 0) && timers::removeTask(static_cast<uint8_t>(id));
        Serial.println(ok ? "task removed" : "usage: del <id>");
        return;
    }
    if (strcmp(cmd, "clear") == 0) {
        timers::clearAll();
        Serial.println("all tasks cleared");
        return;
    }
    if (strcmp(cmd, "time") == 0) {
        if (argc < 2) {
            Serial.println("usage: time <epoch-seconds>");
            return;
        }
        const uint32_t epoch = static_cast<uint32_t>(strtoul(argv[1], nullptr, 10));
        timers::setEpoch(epoch);
#if LAMP_CLOUD_ASR
        // 顺手把系统时钟也设了：TC3 签名读的是 time()，不是 timers 里的计数。
        // 现场没网时靠手机看一眼时间手填，比等 NTP 快（docs/14 §5.4）。
        struct timeval tv {};
        tv.tv_sec = static_cast<time_t>(epoch);
        settimeofday(&tv, nullptr);
        Serial.printf("clock set to %lu（系统时钟 + 定时器）\n",
                      static_cast<unsigned long>(epoch));
#else
        Serial.printf("clock set to %lu\n", static_cast<unsigned long>(timers::epoch()));
#endif
        return;
    }

    if (strcmp(cmd, "motion") == 0) {
        if (argc < 2) {
            Serial.println("usage: motion nod|left|right|home|alert");
            return;
        }
        motion::Move move = motion::Move::Home;
        if (strcmp(argv[1], "nod") == 0) {
            move = motion::Move::Nod;
        } else if (strcmp(argv[1], "left") == 0) {
            move = motion::Move::TurnLeft;
        } else if (strcmp(argv[1], "right") == 0) {
            move = motion::Move::TurnRight;
        } else if (strcmp(argv[1], "alert") == 0) {
            move = motion::Move::Alert;
        } else if (strcmp(argv[1], "home") != 0) {
            Serial.println("motion: nod|left|right|home|alert");
            return;
        }
        const bool ok = motion::request(move);
        Serial.printf("motion %s: %s\n", motion::moveName(move), ok ? "started" : "suppressed");
        return;
    }
    if (strcmp(cmd, "mute") == 0 || strcmp(cmd, "unmute") == 0) {
        const bool mute = strcmp(cmd, "mute") == 0;
        motion::setMuted(mute);
        Serial.printf("motion %s\n", mute ? "muted" : "enabled");
        return;
    }
    if (strcmp(cmd, "log") == 0) {
        dumpLogTail();
        return;
    }

    // ---- voice / microphone ------------------------------------------------
    if (strcmp(cmd, "mic") == 0) {
        const char* sub = argc >= 2 ? argv[1] : "test";
        if (strcmp(sub, "on") == 0) {
            voice::setMonitor(true);
        } else if (strcmp(sub, "off") == 0) {
            voice::setMonitor(false);
        } else if (strcmp(sub, "probe") == 0) {
            pdmmic::diagnose();
        } else if (strcmp(sub, "test") == 0) {
            const int seconds = argc >= 3 ? atoi(argv[2]) : 5;
            const int clamped = (seconds <= 0 || seconds > 60) ? 5 : seconds;
            voice::setMonitor(true);
            micMonitorUntil = millis() + static_cast<uint32_t>(clamped) * 1000UL;
            Serial.printf("mic test: %d s, clap or speak now\n", clamped);
        } else {
            Serial.println("usage: mic on|off|test [seconds]");
        }
        return;
    }
    if (strcmp(cmd, "vad") == 0) {
        // 现场调 VAD 灵敏度：vad 120 更灵敏，vad 0 回到编译期默认值。
        if (argc < 2) {
            Serial.printf("[voice] VAD threshold = %.0f (default %.0f)\n",
                          voice::threshold(), MIC_VAD_RMS);
            Serial.println("usage: vad <rms> | vad 0 (restore default)");
        } else {
            voice::setThreshold(static_cast<float>(atof(argv[1])));
        }
        return;
    }
    if (strcmp(cmd, "key") == 0) {
        if (argc < 2) {
#if LAMP_BACKEND_TENCENT
            Serial.println("usage: key <SecretId> <SecretKey> <TokenHubKey>");
            Serial.println("  SecretId=AKID...  SecretKey=32 位  TokenHubKey=sk-...");
#elif LAMP_BACKEND_ALIYUN
            Serial.println("usage: key <DASHSCOPE_API_KEY>   // 百炼一个 key 就够");
#else
            Serial.println("usage: key <API_KEY> [SECRET_KEY]");
#endif
            return;
        }
        cloudasr::setKey(argv[1], argc >= 3 ? argv[2] : nullptr,
                         argc >= 4 ? argv[3] : nullptr);
        return;
    }
    if (strcmp(cmd, "asr") == 0) {
        // `asr selftest` 跑 TC3 官方向量：不联网、不需要 Key，
        // 用来把「签名算错」和「网络不通」彻底分开（docs/26 §11 的 P2）。
        if (argc >= 2 && strcmp(argv[1], "selftest") == 0) {
            tc3::printSelfTest();
        } else {
            cloudasr::printStatus();
        }
        return;
    }
    if (strcmp(cmd, "voice") == 0) {
        const char* sub = argc >= 2 ? argv[1] : "status";
        if (strcmp(sub, "status") == 0) {
            voice::printStatus();
        } else if (strcmp(sub, "listen") == 0) {
            voice::requestListen();
        } else if (strcmp(sub, "say") == 0) {
            String text;
            for (int i = 2; i < argc; ++i) {
                if (i > 2) text += ' ';
                text += argv[i];
            }
            voice::sendText(text.c_str());
        } else {
            Serial.println("usage: voice status|listen|say <text>");
        }
        return;
    }
    if (strcmp(cmd, "wifi") == 0) {
        const char* sub = argc >= 2 ? argv[1] : "status";
        if (strcmp(sub, "status") == 0) {
            wifiPrintStatus();
            return;
        }
        if (strcmp(sub, "scan") == 0) {
            wifiScanNow();
            return;
        }
        if (strcmp(sub, "clear") == 0 || strcmp(sub, "reset") == 0) {
            wifiClearCredentials();
            return;
        }

        // wifi connect <ssid> <pass>，另外接受 wifi <ssid> <pass> 简写。
        // 值由网页用 encodeURIComponent() 编码；密码传 "-" 表示开放网络（无密码）。
        int ssidArg = 1;
        int passArg = 2;
        if (strcmp(sub, "connect") == 0 || strcmp(sub, "set") == 0) {
            ssidArg = 2;
            passArg = 3;
        }
        if (argc <= ssidArg) {
            Serial.println("usage: wifi connect <ssid> <password>   ('-' = open network)");
            Serial.println("       wifi status | wifi scan | wifi clear");
            return;
        }
        const String ssid = wifiUrlDecode(argv[ssidArg]);
        String pass;
        if (argc > passArg && strcmp(argv[passArg], "-") != 0) {
            pass = wifiUrlDecode(argv[passArg]);
        }
        wifiSaveCredentials(ssid, pass);
        return;
    }

    Serial.printf("unknown command: %s (try 'help')\n", cmd);
}

}  // namespace

// ---------------------------------------------------------------- entry points
void setup() {
    irstore::begin();  // mounts LittleFS; ui::begin() writes into /log
    ui::begin();
    ui::setCommandHandler(onCommand);

    // WiFi 凭据自己存在 "lampwifi" 命名空间里，不需要 Arduino core 再存一份；
    // persistent(false) 可以避免每 10 秒一次的重试反复写 flash。
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    wifiLoadCredentials();

    pinMode(PIN_STATUS_LED, OUTPUT);
    digitalWrite(PIN_STATUS_LED, LOW);

    buttonInit(btnMode, PIN_BTN_MODE);
    buttonInit(btnLight, PIN_BTN_LIGHT);
    buttonInit(btnLearn, PIN_BTN_LEARN);

    lighting::begin();
    motion::begin();
    // 蜂鸣器此前从未 attach 通道，beepTick 一直在往一个没配置的通道写值，
    // 所以按键/定时器的提示音其实从来没响过。10 bit 下 1<<9 正好约 50 % 占空。
    pwm::attach(PIN_BUZZER, CH_BUZZER, LED_PWM_HZ, 10);
    pwm::write(PIN_BUZZER, CH_BUZZER, 0);
    timers::begin();
    timers::setHandler(onTimerFire);
    irengine::begin();
    irengine::setHandler(onLearnResult);

#if LAMP_HAS_VOICE
    voice::begin();
#endif
#if LAMP_VOICE_UART
    voiceuart::begin(VOICE_UART_BAUD, onVoiceCommand);
#endif

    // 外设初始化完再开射频：避免开机电流尖峰和 I2S/射频抢资源。
    if (!wifiSsid.isEmpty()) wifiStartConnect();

    ui::logf("{\"ev\":\"boot\",\"fw\":\"%s\",\"heap\":%lu}", FW_VERSION,
             static_cast<unsigned long>(ESP.getFreeHeap()));
    Serial.printf("[mem] psram=%lu heap=%lu\n",
                  static_cast<unsigned long>(ESP.getPsramSize()),
                  static_cast<unsigned long>(ESP.getFreeHeap()));
    ui::setStatus("ready");
}

void loop() {
    const uint32_t now = millis();

    ui::tick(now);
    handleButtons(now);
    irengine::poll();
    lighting::tick(now);
    motion::tick(now);
    timers::tick(now);
    statusTick(now);
    beepTick(now);

    wifiTick(now);
#if LAMP_HAS_VOICE
    voice::tick(now);
#endif
#if LAMP_VOICE_UART
    voiceuart::poll();
#endif

    if (micMonitorUntil != 0 && static_cast<int32_t>(now - micMonitorUntil) >= 0) {
        micMonitorUntil = 0;
        voice::setMonitor(false);
        Serial.println("mic test: done");
    }
}