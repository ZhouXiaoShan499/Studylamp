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

// WiFi 凭据也可以运行时通过串口/网页写入 NVS（`wifi connect <ssid> <pass>`）。
// 这里补空默认值有两个作用：
//   1) 没有 secrets.h 也能编出带完整联网路径的固件，CI 覆盖到真实代码；
//   2) voice_client 不会因为「没有编译期 SSID」退化成空实现，网页配完网就能直接用。
#ifndef LAMP_WIFI_SSID
#define LAMP_WIFI_SSID ""
#endif
#ifndef LAMP_WIFI_PASS
#define LAMP_WIFI_PASS ""
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

// ---- 面包板台架（阶段 0，见 docs/12）----------------------------------------
// 按键有效电平。1 = 按下读到 LOW（模块上拉到 3V3，HW-483 最常见）；
// 0 = 按下读到 HIGH（模块下拉到 GND）。接反的表现是"按键永远按住"或"完全没反应"，
// 主固件此前把低有效写死了，换模块就得改代码，这里开个口子。
#ifndef LAMP_BTN_ACTIVE_LOW
#define LAMP_BTN_ACTIVE_LOW 1
#endif

// 单通道灯。面包板上只有 HW-479 的 G 接在 PIN_LED_COOL：
// 打开后暖通道恒 0、亮度全给冷通道。否则默认 4300K 混光只给冷通道 56% 占空比，
// 看着像"灯不够亮"，其实是色温配比，不是故障。
#ifndef LAMP_SINGLE_CHANNEL
#define LAMP_SINGLE_CHANNEL 0
#endif

// 离线语音模块（SU-03T / CI-03T 一类）。默认关：模块没到货前不参与编译。
// 供电走开发板 5V 飞线，绝不接面包板 + 轨；模块 TX 是 3.3V 电平，直连即可。
#ifndef LAMP_VOICE_UART
#define LAMP_VOICE_UART 0
#endif
constexpr uint8_t PIN_VOICE_UART_RX = 47;
constexpr uint32_t VOICE_UART_BAUD = 115200;
constexpr uint16_t CMD_DEDUP_MS = 1500;   // 同一命令去重窗口，防模块重复上报
constexpr uint16_t VOICE_FRAME_GAP_MS = 20;  // 半截帧的超时，超时即复位状态机
constexpr bool VOICE_NIGHT_GUARD = true;  // 22:00-07:00 只放行"关灯"

// ---- 云端 ASR（阶段 0 语音闭环，见 docs/12）--------------------------------
// 打开后 voice_client 不再连 PC 的 WebSocket：sendUtterance() 把整段 PCM 交给
// cloud_asr，一次 HTTPS 换回文本，再走本地关键词匹配点灯。
// 关掉则回到原来的"局域网 Vosk"路径，两条路互斥。
#ifndef LAMP_CLOUD_ASR
#define LAMP_CLOUD_ASR 1
#endif
// 语音语义后端，编译期四选一（LAMP_CLOUD_ASR_PROVIDER）：
//   1 = 百度短语音（默认）：语音 → 文字，本地 8 条关键词匹配点灯
//   2 = 百度转文字 + 国产大模型（OpenAI 兼容协议，DeepSeek/通义/智谱/豆包）：
//       支持自然说法，但要两家凭据（百度 AK/SK + LLM Key）
//   3 = 阿里云百炼一家：qwen3-asr-flash 转文字 + deepseek-v4.1-flash 判动作，
//       一个 API Key 走完两跳，不需要百度账号
//   4 = 腾讯云：一句话识别（TC3 签名，SecretId + SecretKey）
//       + TokenHub 大模型（Bearer sk-）。三套凭据、强依赖系统时钟。
// 四家都是国内可直连、无需翻墙。
#ifndef LAMP_CLOUD_ASR_PROVIDER
#define LAMP_CLOUD_ASR_PROVIDER 1
#endif

// 派生语义宏：代码里只读这三个，**不再直接比较 PROVIDER**。
// 起因是旧代码用 `#if LAMP_CLOUD_ASR_PROVIDER != 3` 这种否定式判断，
// 一加 provider=4 就为真，百度的 fetchToken/recognizeOnce 会被重新编进来
// （docs/13 坑 1 已经因为 `== 2` 栽过一次）。改成肯定式，加第 5 家也不会误伤。
#if LAMP_CLOUD_ASR_PROVIDER == 1
#define LAMP_BACKEND_BAIDU 1
#elif LAMP_CLOUD_ASR_PROVIDER == 2
#define LAMP_BACKEND_BAIDU 1
#define LAMP_BACKEND_LLM 1
#elif LAMP_CLOUD_ASR_PROVIDER == 3
#define LAMP_BACKEND_ALIYUN 1
#define LAMP_BACKEND_LLM 1
#elif LAMP_CLOUD_ASR_PROVIDER == 4
#define LAMP_BACKEND_TENCENT 1
#define LAMP_BACKEND_LLM 1
#else
#error "LAMP_CLOUD_ASR_PROVIDER 只能取 1/2/3/4（见 docs/14、docs/26）"
#endif
constexpr uint16_t CLOUD_ASR_TIMEOUT_MS = 8000;
constexpr uint16_t CLOUD_ASR_DEV_PID = 1537;  // 1537 = 普通话纯中文；方言/极速版改这里
constexpr const char* CLOUD_ASR_CUID = "lamp01";

// 国产大模型（provider=2）参数。base 只要 host（不带 https:// 和 /v1），
// 各家填各自的：
//   DeepSeek: api.deepseek.com                 model: deepseek-flash
//   通义千问: dashscope.aliyuncs.com/compatible-mode/v1   model: qwen-plus
//   智谱:     open.bigmodel.cn/api/paas/v4     model: glm-4-flash
//   豆包:     ark.cn-beijing.volces.com/api/v3  model: <你的 endpoint id>
constexpr const char* CLOUD_LLM_HOST = "api.deepseek.com";
constexpr const char* CLOUD_LLM_PATH = "/v1/chat/completions";
// 旧名 deepseek-chat / deepseek-reasoner 是 V3.x 别名，已下线，请求会返回 404。
// 现行模型名 deepseek-flash。路径 /v1/chat/completions 与 /chat/completions
// 实测都可用（无效 Key 均返回 401 而非 404），保留 /v1 与 OpenAI 兼容写法一致。
constexpr const char* CLOUD_LLM_MODEL = "deepseek-flash";

// 阿里云百炼（provider=3）：两跳走同一个域名、同一个 Key，串口只写一个 sk-xxx。
//   第一跳 ASR：qwen3-asr-flash。OpenAI 兼容模式下把 44 字节 WAV 头 + 16k PCM
//               整段 base64 塞进 messages[0].content[0].input_audio.data，
//               stream=false 一次性返回 choices[0].message.content。
//   第二跳 LLM：同一个 /chat/completions，模型换成 deepseek-v4.1-flash。
// 域名：老控制台是 dashscope.aliyuncs.com；新控制台若返回 404/403，换成
//       https://<WorkspaceId>.cn-beijing.maas.aliyuncs.com，Key 不变。
constexpr const char* CLOUD_BAILIAN_HOST = "dashscope.aliyuncs.com";
constexpr const char* CLOUD_BAILIAN_PATH = "/compatible-mode/v1/chat/completions";
constexpr const char* CLOUD_BAILIAN_ASR_MODEL = "qwen3-asr-flash";
// 百炼的 LLM 模型名：deepseek-v3 / deepseek-r1 等已于 2026-10-10 下架。
// 当前账号实际可用的是 deepseek-v4.1-flash（注意 .1，不是 deepseek-v4-flash）。
// 官方也推荐 qwen3.7-plus / qwen3.6-flash。报 "Model not exist." 就换备选。
constexpr const char* CLOUD_BAILIAN_LLM_MODEL = "deepseek-v4.1-flash";
// 2 s 音频 base64 后约 86 KB，上传 + 推理比百度的裸 PCM 慢，单独放宽窗口。
constexpr uint16_t CLOUD_BAILIAN_TIMEOUT_MS = 15000;

// ---- 腾讯云（provider=4）----------------------------------------------------
// 听和想是两套鉴权、两个域名，这是和百炼最大的结构差别：
//   听：asr.tencentcloudapi.com，Action=SentenceRecognition，TC3-HMAC-SHA256 现算，
//       要求 X-TC-Timestamp 与服务器相差 ≤5 分钟 —— 所以必须先 SNTP 对时；
//   想：TokenHub，OpenAI 兼容，Bearer sk-，不签名、不对时。
// 一句话识别不接受裸 PCM：VoiceFormat=wav 时给完整 WAV（含 44 字节头），
// 再 base64 且**不能带换行**（mbedtls 默认每 76 字符插 \n，必须原地压掉）。
constexpr const char* CLOUD_TC_ASR_HOST = "asr.tencentcloudapi.com";
constexpr const char* CLOUD_TC_ASR_SERVICE = "asr";
constexpr const char* CLOUD_TC_ASR_ACTION = "SentenceRecognition";
constexpr const char* CLOUD_TC_ASR_VERSION = "2019-06-14";
constexpr const char* CLOUD_TC_ASR_ENGINE = "16k_zh";  // 方言改 16k_zh_dialect
constexpr uint16_t CLOUD_TC_ASR_TIMEOUT_MS = 8000;
// 热词表：短句场景提升识别率最明显的一招。格式「词|权重」，逗号分隔。
constexpr const char* CLOUD_TC_HOTWORDS = "开灯|10,关灯|10,亮一点|8,暗一点|8,夜灯|8";
// 2 = 服务端砍掉标点，响应体更小；0 = 保留。
constexpr uint8_t CLOUD_TC_FILTER_PUNC = 2;
// 系统时钟的「有效」门槛（2023-11-14）。低于它说明还没对时，
// 此时发请求必然 AuthFailure.SignatureExpire，不如直接拒发并打日志。
constexpr uint32_t CLOCK_MIN_VALID_EPOCH = 1700000000UL;
constexpr uint16_t CLOUD_TC_LLM_TIMEOUT_MS = 8000;

// 云端 Key 的两个来源，优先级：NVS（串口 key 命令） > 编译期常量（secrets.h）。
// 把下面两个宏写进 include/secrets.h（git-ignored，和 WiFi 密码放在一起），
// 就能一次烧板、不用再连串口敲命令；留空则只认串口写入的 NVS。
#ifndef LAMP_CLOUD_API_KEY
#define LAMP_CLOUD_API_KEY ""
#endif
#ifndef LAMP_CLOUD_SECRET_KEY
#define LAMP_CLOUD_SECRET_KEY ""
#endif

// ---- 第二跳（语义）可以单独换一家，不吃 ASR 那把 Key -------------------------
// 腾讯云 TokenHub / 混元 走 OpenAI 兼容协议，只要一个 Bearer `sk-...`：
// 不签名、不对时，所以「模型换成腾讯 DeepSeek」是一项独立的小改动，
// 和 ASR 那条 TC3 路线（SecretId + SecretKey + SNTP）完全解耦。
//
// 填了 LAMP_CLOUD_LLM_KEY 之后，classifyIntent() 自动把第二跳改走腾讯，
// 第一跳仍按 LAMP_CLOUD_ASR_PROVIDER 走百度/百炼，两家各用各的 Key。
// 留空 = 沿用旧行为（provider=3 一个百炼 Key 打两跳）。
#ifndef LAMP_CLOUD_LLM_KEY
#define LAMP_CLOUD_LLM_KEY ""
#endif
// 腾讯有两套入口，Key 不通用。2026-11-12 实测：控制台「TokenHub → API Key」
// 生成的那把 sk- 只认 tokenhub.tencentmaas.com；混元入口会返回 invalid_api_key，
// 而 TokenHub 旧域名 tokenhub.tencentcloudmaas.com 返回 401002。
//   tokenhub.tencentmaas.com            （TokenHub，当前账号实测可用）
//   api.hunyuan.cloud.tencent.com       （混元，用另一套 Key）
#ifndef LAMP_TENCENT_LLM_HOST
#define LAMP_TENCENT_LLM_HOST "tokenhub.tencentmaas.com"
#endif
#ifndef LAMP_TENCENT_LLM_PATH
#define LAMP_TENCENT_LLM_PATH "/v1/chat/completions"
#endif
// 模型名是易耗品（docs/13、docs/26 都栽过）。必须与控制台「API Key」页显示的一致，
// 大小写和版本后缀都算数。报 Model not exist 再依次试：
//   deepseek-v4.1-flash / deepseek-v3 / deepseek-r1 / hunyuan-turbos-latest
#ifndef LAMP_TENCENT_LLM_MODEL
#define LAMP_TENCENT_LLM_MODEL "DeepSeek-V4.1-Flash-1017"
#endif

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
// 触发阈值 = max(底噪 × 1.5, 这个下限)，底噪估计封顶 220。
// 2026-11-12 实测：安静 RMS 40~90；有人/风扇在旁时底噪能到 150~190；
// 正常坐姿（约 50 cm）说「开灯」只有 200~400。下限取 180：低于它的多半是噪声，
// 再低会把空调/风扇的起伏当成说话（实测会连续空发 ASR 请求）。
constexpr float MIC_VAD_RMS = 180.0f;
constexpr uint8_t MIC_VAD_FRAMES = 3;             // 连续 3 帧（96 ms）超阈值才算开口
constexpr float MIC_VAD_FACTOR = 1.5f;            // 自适应阈值 = max(底噪 × 1.5, 下限)
constexpr float MIC_NOISE_FLOOR_MAX = 220.0f;     // 底噪估计封顶，避免被环境噪声带飞
constexpr uint32_t MIC_RETRIGGER_GUARD_MS = 800;  // 说完后的冷却，避免句尾余音再触发
constexpr uint32_t MIC_SILENCE_MS = 700;       // trailing silence ends a phrase
constexpr uint32_t MIC_MAX_UTTERANCE_MS = 8000;
// 触发后固定录这么久再整段发送：边录边发在弱网下会把主循环拖死（实测 10 s 只发出 0.16 s 音频）。
constexpr uint32_t MIC_RECORD_MS = 2000;
constexpr size_t MIC_RECORD_SAMPLES = MIC_RECORD_MS * MIC_SAMPLE_RATE / 1000;
constexpr uint32_t MIC_WAIT_REPLY_MS = 45000;  // LLM may think for a while
constexpr uint32_t MIC_MONITOR_PERIOD_MS = 120;
constexpr uint16_t MIC_RETRY_MS = 5000;        // re-open the I2S channel