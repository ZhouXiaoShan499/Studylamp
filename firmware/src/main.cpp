// LAMP-1 MVP firmware: IR learn/replay + dual-CCT lighting + timers + motion.
// Single non-blocking loop; no delay() outside of short hardware sequences.
// See docs/03-技术方案.md for the design rationale behind each module.

#include <Arduino.h>
#include <Esp.h>
#include <LittleFS.h>

#include "config.h"
#include "ir_engine.h"
#include "ir_store.h"
#include "lighting.h"
#include "motion.h"
#include "pdm_mic.h"
#include "pwm.h"
#include "timer_task.h"
#include "ui.h"
#include "voice_client.h"

#if defined(LAMP_WIFI_SSID) && defined(LAMP_WIFI_PASS)
#include <WiFi.h>
#include <time.h>
#define LAMP_WIFI_ENABLED 1
#else
#define LAMP_WIFI_ENABLED 0
#endif

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

void buttonInit(Button& button, uint8_t pin) {
    button.pin = pin;
    pinMode(pin, INPUT_PULLUP);
    button.stable = (digitalRead(pin) == LOW);
    button.lastRead = button.stable;
    button.longFired = false;
    button.lastChange = millis();
    button.pressedAt = 0;
}

BtnEvent buttonPoll(Button& button, uint32_t now, uint16_t longMs) {
    const bool raw = (digitalRead(button.pin) == LOW);  // active low
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
    } else if (ledOn) {
        ledOn = false;
        digitalWrite(PIN_STATUS_LED, LOW);
    }
    motion::setNightMode(isNightNow());
}

#if LAMP_WIFI_ENABLED
void wifiTick(uint32_t now) {
    static uint32_t lastAttempt = 0;
    static uint32_t lastSync = 0;
    static bool started = false;

    if (WiFi.status() != WL_CONNECTED) {
        if (now - lastAttempt >= 10000) {
            lastAttempt = now;
            WiFi.begin(LAMP_WIFI_SSID, LAMP_WIFI_PASS);  // non-blocking
            if (!started) {
                started = true;
                ui::logf("{\"ev\":\"wifi_begin\",\"ssid\":\"%s\"}", LAMP_WIFI_SSID);
            }
        }
        return;
    }
    if (now - lastSync >= 5000) {
        lastSync = now;
        const time_t value = time(nullptr);
        if (value > 1600000000L) {
            const uint32_t epoch = static_cast<uint32_t>(value);
            if (!timers::timeValid() || (timers::epoch() > epoch + 5) || (timers::epoch() + 5 < epoch)) {
                timers::setEpoch(epoch);
            }
        }
    }
}
#endif

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
        timers::setEpoch(static_cast<uint32_t>(strtoul(argv[1], nullptr, 10)));
        Serial.printf("clock set to %lu\n", static_cast<unsigned long>(timers::epoch()));
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

    Serial.printf("unknown command: %s (try 'help')\n", cmd);
}

}  // namespace

// ---------------------------------------------------------------- entry points
void setup() {
    irstore::begin();  // mounts LittleFS; ui::begin() writes into /log
    ui::begin();
    ui::setCommandHandler(onCommand);

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

    ui::logf("{\"ev\":\"boot\",\"fw\":\"%s\",\"heap\":%lu}", FW_VERSION,
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

#if LAMP_WIFI_ENABLED
    wifiTick(now);
#endif
#if LAMP_HAS_VOICE
    voice::tick(now);
#endif

    if (micMonitorUntil != 0 && static_cast<int32_t>(now - micMonitorUntil) >= 0) {
        micMonitorUntil = 0;
        voice::setMonitor(false);
        Serial.println("mic test: done");
    }
}