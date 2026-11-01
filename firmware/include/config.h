#pragma once
// ---------------------------------------------------------------------------
// Compile-time configuration: pins, limits, feature flags.
// Every pin here must be re-checked against the real PCB before flashing:
// GPIO 0/3/45/46 are strapping pins, GPIO 26..32 are used by flash/PSRAM,
// GPIO 19/20 are USB D-/D+. Do not reuse them.
// ---------------------------------------------------------------------------

#include <Arduino.h>

// ---- optional local secrets (copy include/secrets.h.example -> secrets.h) ---
// WiFi credentials and the LAN PC address live here, never in the repo.
#if defined(__has_include)
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#endif

#ifndef LAMP_VOICE_SERVER_HOST
#define LAMP_VOICE_SERVER_HOST "192.168.1.100"
#endif
#ifndef LAMP_VOICE_SERVER_PORT
#define LAMP_VOICE_SERVER_PORT 8765
#endif

// ---- feature flags (override with build_flags) -----------------------------
#ifndef LAMP_USE_OLED
#define LAMP_USE_OLED 0
#endif
#ifndef LAMP_HAS_SERVOS
#define LAMP_HAS_SERVOS 1
#endif
// V829 PDM microphone + LAN ASR/LLM link (tasks 1-3 of the voice spike).
#ifndef LAMP_HAS_VOICE
#define LAMP_HAS_VOICE 1
#endif

// ---- pins -----------------------------------------------------------------
#ifdef LAMP_BOARD_BREADBOARD
// 面包板模式：HW-483 按键接 GPIO4、HW-479 的 G 通道接 GPIO5。
// 正式 PCB 的暖白通道正好在 GPIO4，直接烧会"按键踩在输出脚上"，又没反应又危险。
// 这里把暖白挪到没人用的 GPIO21（面包板上不接），按键回到 GPIO4 —— 不改一根线，
// 按键和语音就能同时工作。
constexpr uint8_t PIN_LED_WARM = 21;
constexpr uint8_t PIN_BTN_LIGHT = 4;
// INMP441 的 WS(LRCLK) 接在 GPIO18，状态灯让位到 GPIO14
constexpr uint8_t PIN_STATUS_LED = 14;
#else
constexpr uint8_t PIN_LED_WARM = 4;
constexpr uint8_t PIN_BTN_LIGHT = 14;
constexpr uint8_t PIN_STATUS_LED = 18;
#endif
constexpr uint8_t PIN_LED_COOL = 5;
constexpr uint8_t PIN_SERVO_TILT = 6;
constexpr uint8_t PIN_SERVO_YAW = 7;
constexpr uint8_t PIN_BUZZER = 8;
constexpr uint8_t PIN_IR_RECV = 9;
constexpr uint8_t PIN_IR_SEND = 10;
constexpr uint8_t PIN_OLED_SDA = 11;
constexpr uint8_t PIN_OLED_SCL = 12;
constexpr uint8_t PIN_BTN_MODE = 13;
constexpr uint8_t PIN_BTN_LEARN = 15;
// INMP441 是标准 I2S 麦克风（不是 PDM）：BCLK -> GPIO16，WS/LRCLK -> GPIO18，SD -> GPIO17。
// L/R 接 GND 选左声道；旧名 PIN_VOICE_TX/RX 是历史遗留，别按 TX/RX 去接串口。
constexpr uint8_t PIN_VOICE_TX = 16;  // BCLK
constexpr uint8_t PIN_VOICE_RX = 17;  // SD (mic -> ESP32)
constexpr uint8_t PIN_VOICE_WS = 18;  // WS / LRCLK

// ---- LEDC channels ---------------------------------------------------------
// ESP32-S3 只有 8 个 LEDC 通道（SOC_LEDC_CHANNEL_NUM = 8，通道号 0..7），
// arduino-esp32 在 chan >= 8 时直接 log_e 后返回 0 —— PWM 根本不会建立，
// ledcWrite 静默无效。之前照抄经典 ESP32 的 8..12 号通道，导致灯/舵机/蜂鸣器
// 全都没有输出（按键能记日志、但灯不亮），这里必须改用 0..4。
// timer 绑定规则：timer = (chan / 2) % 4，同一 timer 的两个通道必须同频同分辨率。
//   chan 0,1 -> timer 0 (4 kHz, 12 bit)  : 暖白、冷白
//   chan 2,3 -> timer 1 (50 Hz, 14 bit)  : 俯仰、水平舵机
//   chan 4   -> timer 2 (4 kHz, 10 bit)  : 蜂鸣器
constexpr uint8_t CH_LED_WARM = 0;
constexpr uint8_t CH_LED_COOL = 1;
constexpr uint8_t CH_SERVO_TILT = 2;
constexpr uint8_t CH_SERVO_YAW = 3;
constexpr uint8_t CH_BUZZER = 4;

constexpr uint32_t LED_PWM_HZ = 4000;
constexpr uint8_t LED_PWM_BITS = 12;   // 4096 steps
constexpr uint32_t SERVO_PWM_HZ = 50;
constexpr uint8_t SERVO_PWM_BITS = 14; // 16384 steps -> 1.22 us per tick

// ---- lighting -------------------------------------------------------------
constexpr uint8_t LIGHT_MIN_PCT = 1;
constexpr uint8_t LIGHT_MAX_PCT = 100;
constexpr uint16_t LIGHT_RAMP_MS = 600;   // full 0..100 % ramp time

// ---- infrared ------------------------------------------------------------
constexpr uint16_t IR_CAPTURE_BUFFER = 1024;  // must equal irstore::kMaxRawLen
constexpr uint8_t IR_CAPTURE_TIMEOUT_MS = 50; // IRrecv gap timeout
constexpr uint16_t IR_MIN_RAW_LEN = 20;
constexpr uint16_t IR_LEARN_TIMEOUT_MS = 15000;
constexpr uint8_t IR_TOLERANCE_PCT = 5;       // two captures must agree within 5 %
constexpr uint8_t IR_SEND_REPEATS = 2;
constexpr uint16_t IR_SEND_GAP_MS = 40;
constexpr uint8_t IR_BLOCK_RX_MS = 120;       // do not hear our own transmission

// ---- timers --------------------------------------------------------------
constexpr uint8_t TASK_MAX = 8;
constexpr uint16_t TASK_MIN_DURATION_S = 10;
constexpr uint32_t TASK_MAX_DURATION_S = 24UL * 3600UL;

// ---- motion --------------------------------------------------------------
constexpr float SERVO_TILT_MIN_DEG = 10.0f;
constexpr float SERVO_TILT_MAX_DEG = 55.0f;
constexpr float SERVO_TILT_HOME_DEG = 30.0f;
constexpr float SERVO_YAW_MIN_DEG = -60.0f;
constexpr float SERVO_YAW_MAX_DEG = 60.0f;
constexpr float SERVO_YAW_HOME_DEG = 0.0f;
constexpr float SERVO_STEP_DEG = 1.5f;    // per 20 ms tick
constexpr uint32_t MOTION_TICK_MS = 20;

// ---- buttons -------------------------------------------------------------
constexpr uint16_t BTN_DEBOUNCE_MS = 20;
constexpr uint16_t BTN_LONGPRESS_MS = 1200;
constexpr uint16_t BTN_LEARN_LONGPRESS_MS = 3000;

// ---- misc ----------------------------------------------------------------
constexpr const char* FW_VERSION = "0.1.0-mvp";
constexpr const char* NVS_NAMESPACE = "lamp1";
constexpr const char* IR_DIR = "/ir";
constexpr const char* LOG_DIR = "/log";
constexpr size_t LOG_MAX_BYTES = 32 * 1024;
constexpr bool DEFAULT_QUIET_AT_NIGHT = true;

// ---- voice / microphone ----------------------------------------------------
// 16 kHz mono s16le is what the Vosk small-cn model on the PC expects.
constexpr uint32_t MIC_SAMPLE_RATE = 16000;
constexpr uint16_t MIC_FRAME_SAMPLES = 512;    // 32 ms per frame
constexpr uint16_t MIC_PREROLL_FRAMES = 4;     // 128 ms kept before the trigger
// INMP441 在 16 bit 槽里只有几十 LSB（实测说话峰值 ~56），先做数字增益再判断。
// 增益后的值会同时用于 VAD 和送给 Vosk 的音频，两头都受益。
constexpr uint16_t MIC_GAIN = 4;
// 触发阈值 = max(底噪 × 3, 这个下限)。实测静音 RMS≈13，增益后≈200，所以下限取 450。
constexpr float MIC_VAD_RMS = 450.0f;
constexpr uint32_t MIC_SILENCE_MS = 700;       // trailing silence ends a phrase
constexpr uint32_t MIC_MAX_UTTERANCE_MS = 8000;
// 触发后固定录这么久再整段发送：边录边发在弱网下会把主循环拖死（实测 10 s 只发出 0.16 s 音频）。
constexpr uint32_t MIC_RECORD_MS = 2000;
constexpr size_t MIC_RECORD_SAMPLES = MIC_RECORD_MS * MIC_SAMPLE_RATE / 1000;
constexpr uint32_t MIC_WAIT_REPLY_MS = 45000;  // LLM may think for a while
constexpr uint32_t MIC_MONITOR_PERIOD_MS = 120;
constexpr uint16_t MIC_RETRY_MS = 5000;        // re-open the I2S channel