#include "ir_engine.h"

#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>
#include <IRremoteESP8266.h>

namespace irengine {
namespace {

IRrecv irrecv(PIN_IR_RECV, IR_CAPTURE_BUFFER, IR_CAPTURE_TIMEOUT_MS, true);
IRsend irsend(PIN_IR_SEND);

LearnState state = LearnState::Idle;
LearnHandler handler = nullptr;
uint8_t currentSlot = 0;
uint32_t deadlineMs = 0;
uint32_t rxBlockedUntilMs = 0;

uint16_t firstCapture[irstore::kMaxRawLen];
uint16_t firstLen = 0;
uint16_t scratch[irstore::kMaxRawLen];
irstore::Record record;  // ~2 KB static, never on the stack

bool withinTolerance(uint16_t a, uint16_t b) {
    const uint16_t diff = (a > b) ? static_cast<uint16_t>(a - b) : static_cast<uint16_t>(b - a);
    const uint32_t base = (a > b) ? a : b;
    if (base == 0) return diff == 0;
    return static_cast<uint32_t>(diff) * 100UL <= base * IR_TOLERANCE_PCT;
}

bool capturesMatch(const uint16_t* a, uint16_t alen, const uint16_t* b, uint16_t blen) {
    if (alen != blen) return false;
    for (uint16_t i = 0; i < alen; ++i) {
        if (!withinTolerance(a[i], b[i])) return false;
    }
    return true;
}

void finish(bool ok, LearnError error, uint16_t len) {
    state = LearnState::Idle;
    firstLen = 0;
    irrecv.disableIRIn();
    Serial.printf("[ir] learn slot=%u ok=%d err=%s len=%u\n", static_cast<unsigned>(currentSlot),
                  ok ? 1 : 0, errorText(error), static_cast<unsigned>(len));
    if (handler != nullptr) handler(currentSlot, ok, error, len);
}

}  // namespace

const char* errorText(LearnError error) {
    switch (error) {
        case LearnError::None: return "none";
        case LearnError::Timeout: return "timeout";
        case LearnError::TooShort: return "too_short";
        case LearnError::TooLong: return "too_long";
        case LearnError::Unstable: return "unstable";
        case LearnError::SaveFailed: return "save_failed";
    }
    return "unknown";
}

const char* stateText(LearnState value) {
    switch (value) {
        case LearnState::Idle: return "idle";
        case LearnState::WaitFirst: return "wait_first";
        case LearnState::WaitSecond: return "wait_second";
    }
    return "unknown";
}

void begin() {
    irsend.begin();
    // IRrecv 构造后默认就是 idle（未 enableIRIn，timer 句柄为 NULL）。
    // 不能在此调用 irrecv.disableIRIn()：它在 ESP32 上会无条件对 NULL 的
    // timer 调 timerAlarmDisable()，导致 LoadProhibited 崩溃。
    state = LearnState::Idle;
    Serial.printf("[ir] receiver=%u sender=%u\n", PIN_IR_RECV, PIN_IR_SEND);
}

void setHandler(LearnHandler value) { handler = value; }

void startLearn(uint8_t slot) {
    if (slot >= irstore::kSlots) return;
    currentSlot = slot;
    firstLen = 0;
    state = LearnState::WaitFirst;
    deadlineMs = millis() + IR_LEARN_TIMEOUT_MS;
    irrecv.enableIRIn();
    Serial.printf("[ir] learn start slot=%u timeout=%ums\n", static_cast<unsigned>(slot),
                  IR_LEARN_TIMEOUT_MS);
}

void cancelLearn() {
    if (state == LearnState::Idle) return;
    finish(false, LearnError::Timeout, 0);
}

bool isLearning() { return state != LearnState::Idle; }
uint8_t learningSlot() { return currentSlot; }

uint32_t learnRemainingMs() {
    if (state == LearnState::Idle) return 0;
    const uint32_t now = millis();
    return (now >= deadlineMs) ? 0 : (deadlineMs - now);
}

bool hasCode(uint8_t slot) {
    return slot < irstore::kSlots && irstore::exists(slot);
}

void poll() {
    if (state == LearnState::Idle) return;
    if (millis() < rxBlockedUntilMs) return;  // ignore our own transmission tail
    if (millis() > deadlineMs) {
        finish(false, LearnError::Timeout, 0);
        return;
    }

    decode_results results;
    if (!irrecv.decode(&results)) return;

    // Oversized frames would overflow our buffer: reject instead of truncating.
    if (results.rawlen >= IR_CAPTURE_BUFFER) {
        irrecv.resume();
        finish(false, LearnError::TooLong, 0);
        return;
    }

    // IRremoteESP8266 2.9 的 IRrecv 没有 encode()；用 IRutils 的
    // resultToRawArray() 把 decode_results 转成 sendRaw() 兼容的微秒数组，
    // 长度由 getCorrectedRawLength() 给出，返回值需要调用方 delete[]。
    uint16_t len = 0;
    const uint16_t rawLen = getCorrectedRawLength(&results);
    if (rawLen > irstore::kMaxRawLen) {
        irrecv.resume();
        finish(false, LearnError::TooLong, 0);
        return;
    }
    uint16_t* raw = resultToRawArray(&results);
    if (raw != nullptr) {
        len = rawLen;
        memcpy(scratch, raw, static_cast<size_t>(len) * sizeof(uint16_t));
        delete[] raw;
    }
    irrecv.resume();

    if (len < irstore::kMinRawLen) {
        finish(false, LearnError::TooShort, len);
        return;
    }

    if (state == LearnState::WaitFirst) {
        memcpy(firstCapture, scratch, static_cast<size_t>(len) * 2);
        firstLen = len;
        state = LearnState::WaitSecond;
        deadlineMs = millis() + IR_LEARN_TIMEOUT_MS;  // restart the 15 s window
        Serial.printf("[ir] first capture len=%u, press the same key again\n",
                      static_cast<unsigned>(len));
        return;
    }

    // Second capture: both frames must describe the same signal.
    if (!capturesMatch(firstCapture, firstLen, scratch, len)) {
        finish(false, LearnError::Unstable, len);
        return;
    }

    const irstore::Result saved =
        irstore::save(currentSlot, scratch, len, 38);  // 38 kHz is the universal default
    if (saved != irstore::Result::Ok) {
        finish(false, LearnError::SaveFailed, len);
        return;
    }
    // Read back so a bad flash write cannot be reported as success.
    if (irstore::load(currentSlot, record) != irstore::Result::Ok ||
        record.len != len) {
        finish(false, LearnError::SaveFailed, len);
        return;
    }
    finish(true, LearnError::None, len);
}

bool send(uint8_t slot) {
    if (slot >= irstore::kSlots) return false;
    if (irstore::load(slot, record) != irstore::Result::Ok) {
        Serial.printf("[ir] send slot=%u -> %s\n", static_cast<unsigned>(slot), "no code");
        return false;
    }

    const bool wasLearning = isLearning();
    if (wasLearning) cancelLearn();  // 内部 finish() 已 disableIRIn，timer 已初始化，安全
    irrecv.pause();  // 用 pause 而非 disableIRIn：未 enableIRIn 时 timer 为 NULL，disableIRIn 会崩溃

    for (uint8_t i = 0; i < IR_SEND_REPEATS; ++i) {
        irsend.sendRaw(record.raw, record.len, record.freq_khz);
        if (i + 1 < IR_SEND_REPEATS) delay(IR_SEND_GAP_MS);
    }
    rxBlockedUntilMs = millis() + IR_BLOCK_RX_MS;

    Serial.printf("[ir] send slot=%u len=%u freq=%ukHz x%u\n", static_cast<unsigned>(slot),
                  static_cast<unsigned>(record.len), static_cast<unsigned>(record.freq_khz),
                  IR_SEND_REPEATS);
    return true;
}

}  // namespace irengine