#include "voice_uart.h"

#if !LAMP_VOICE_UART

// 模块未接：全部空实现，主固件照常编译、照常跑按键。
namespace voiceuart {
void begin(uint32_t, Handler) {}
void poll() {}
bool enabled() { return false; }
uint8_t lastCommand() { return 0; }
uint32_t badFrames() { return 0; }
}  // namespace voiceuart

#else

namespace voiceuart {
namespace {

constexpr uint8_t kHead0 = 0xAA;
constexpr uint8_t kHead1 = 0x55;
constexpr size_t kMaxPayload = 8;  // 真实命令帧 len 基本都是 0

HardwareSerial sSerial(1);  // UART1，只收不发
Handler sHandler = nullptr;
bool sReady = false;

enum class State : uint8_t { Head0, Head1, Cmd, Len, Data, Sum };

State sState = State::Head0;
uint8_t sCmd = 0;
uint8_t sLen = 0;
uint8_t sIndex = 0;
uint8_t sSum = 0;
uint8_t sPayload[kMaxPayload];

uint32_t sLastByteAt = 0;
uint8_t sLastCmd = 0;
uint32_t sLastCmdAt = 0;
uint32_t sBadFrames = 0;

void resetFrame() {
    sState = State::Head0;
    sCmd = 0;
    sLen = 0;
    sIndex = 0;
    sSum = 0;
}

void dispatch() {
    const uint32_t now = millis();
    // 模块会把同一条命令连报两三次，1.5 s 内只认第一次。
    if (sCmd == sLastCmd && (now - sLastCmdAt) < CMD_DEDUP_MS) return;
    sLastCmd = sCmd;
    sLastCmdAt = now;
    if (sHandler != nullptr) sHandler(sCmd);
}

void feed(uint8_t b) {
    switch (sState) {
        case State::Head0:
            if (b == kHead0) {
                sSum = b;
                sState = State::Head1;
            }
            break;
        case State::Head1:
            if (b == kHead1) {
                sSum = static_cast<uint8_t>(sSum + b);
                sState = State::Cmd;
            } else if (b == kHead0) {
                sSum = b;  // 0xAA 0xAA 0x55：重新对齐
            } else {
                resetFrame();
            }
            break;
        case State::Cmd:
            sCmd = b;
            sSum = static_cast<uint8_t>(sSum + b);
            sState = State::Len;
            break;
        case State::Len:
            sLen = b;
            sSum = static_cast<uint8_t>(sSum + b);
            sIndex = 0;
            if (sLen > kMaxPayload) {
                sBadFrames++;  // 不是命令帧，丢掉，别让状态机卡住
                resetFrame();
                return;
            }
            sState = (sLen == 0) ? State::Sum : State::Data;
            break;
        case State::Data:
            sPayload[sIndex++] = b;
            sSum = static_cast<uint8_t>(sSum + b);
            if (sIndex >= sLen) sState = State::Sum;
            break;
        case State::Sum:
            if (b == sSum) {
                dispatch();
            } else {
                sBadFrames++;
            }
            resetFrame();
            break;
    }
}

}  // namespace

void begin(uint32_t baud, Handler handler) {
    if (sReady) return;
    sHandler = handler;
    // 只给 RX 分配引脚，TX 设 -1：我们从不向模块发东西。
    sSerial.begin(baud, SERIAL_8N1, PIN_VOICE_UART_RX, -1);
    sReady = true;
    Serial.printf("[voice] uart1 rx=%u baud=%lu (frame AA 55 id len sum)\n",
                  static_cast<unsigned>(PIN_VOICE_UART_RX), static_cast<unsigned long>(baud));
}

void poll() {
    if (!sReady) return;

    while (sSerial.available() > 0) {
        const int value = sSerial.read();
        if (value < 0) break;
        sLastByteAt = millis();
        feed(static_cast<uint8_t>(value));
    }

    // 半截帧：20 ms 没有新字节就复位，避免一个错字节把后续所有帧都带偏。
    if (sState != State::Head0 && (millis() - sLastByteAt) > VOICE_FRAME_GAP_MS) {
        sBadFrames++;
        resetFrame();
    }
}

bool enabled() { return sReady; }
uint8_t lastCommand() { return sLastCmd; }
uint32_t badFrames() { return sBadFrames; }

}  // namespace voiceuart

#endif  // LAMP_VOICE_UART
