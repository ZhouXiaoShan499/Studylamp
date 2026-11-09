// ---------------------------------------------------------------------------
// LAMP-SR: 板上离线命令词识别（ESP-SR AFE + MultiNet）
//
// 链路：INMP441 -> I2S(32 bit 槽) -> AFE(降噪/VAD) -> MultiNet -> GPIO5
// 完全不依赖 PC、不联网、不花钱。
//
// 引脚来源：firmware/include/config.h 的面包板模式，已实测过
//   BCLK=16, WS=18, SD=17（INMP441）
//   LED = GPIO5（HW-479 的 G 通道）
//
// 与 Arduino 主固件的差异：这里只有"识别 + 点灯"，没有红外/定时/色温。
// 详见 docs/18-今天脱离PC的实施方案.md
// ---------------------------------------------------------------------------
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "model_path.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_vad.h"

// ---- 硬件 -----------------------------------------------------------------
#define PIN_MIC_BCLK GPIO_NUM_16
#define PIN_MIC_WS   GPIO_NUM_18
#define PIN_MIC_SD   GPIO_NUM_17
#define PIN_LED      GPIO_NUM_5
#define PIN_BTN      GPIO_NUM_4     // 面包板 HW-483 的 S 脚
#define MIC_RATE     16000

// HW-483 有上拉版和下拉版两种，接反的表现是"按键永远按住"或"完全没反应"。
// 1 = 按下读到 0（模块上拉到 3V3，最常见）；0 = 按下读到 1（模块下拉到 GND）。
// 开机日志会打印 btn GPIO4 idle=...，松开手若显示 PRESSED，就把这个改成 0。
#define BTN_ACTIVE_LOW   1
#define BTN_DEBOUNCE_MS  30
#define BTN_LONGPRESS_MS 1200

// 1 = 唤醒词命中时闪一下灯并重开识别窗口（提示用）；0 = 连这个动作也省掉。
// 注意：它**不**门控识别。detect() 每帧都在跑，所以"直说开灯"本来就该能认，
// 之前不认是因为 6 秒窗口过期后没人重开（见循环里的 ESP_MN_STATE_TIMEOUT 分支）。
#define GATE_BY_WAKEUP 1

// MultiNet 的识别窗口。create() 的第二个参数：这么长时间内没听到命令就进入
// ESP_MN_STATE_TIMEOUT，这之后 detect() 不再出结果，必须 clean() 才能重开。
// 官方示例是"超时就停下等唤醒词"，我们要"直说就能开灯"，所以超时后立刻重开
// （见循环里的 ESP_MN_STATE_TIMEOUT 分支）。
// 取值理由：窗口越短，"正好说到一半被 clean() 截断"的概率越高。10 分钟够长，
// 实际使用中基本不会超时；万一超时也有分支兜住，功能不受影响。
#define MN_WINDOW_MS 600000

// 前置线性增益（AFE 之前，作用在原始 PCM 上）。
// INMP441 是裸麦，没有 AGC。150 秒实测（正常坐姿 ~50cm）：
//   说话 peak 40~419（约 -38~-58 dBFS），MultiNet 零结果；
//   凑到 10cm 说话 peak 1284~1679（约 -26 dBFS），能稳定识别。
// 也就是说差了约 20 dB —— 光靠 afe_linear_gain（只补了 12 dB，且作用在
// AFE 内部 NS/AGC 之后）不够。这里在源头补 8 倍（+18 dB）：
//   正常距离 150*8=1200 ≈ 首次成功的量级；
//   凑近 1284*8=10272，仍远低于满量程 32767，不会削顶。
// 必须钳位：不钳的话凑近说话会整数溢出翻转成反向尖峰，MultiNet 会彻底认不出。
#define MIC_PRE_GAIN 8

static const char *TAG = "lamp_sr";
static i2s_chan_handle_t s_rx = NULL;

// ---- 命令表 ---------------------------------------------------------------
// MultiNet 用拼音，不带声调，音节空格分隔。ID 必须 > 0。
typedef struct {
    int id;
    const char *pinyin;
    const char *label;
} cmd_entry_t;

static const cmd_entry_t kCmds[] = {
    {1, "da kai deng",   "开灯"},
    {2, "guan deng",     "关灯"},
    {3, "liang yi dian", "亮一点"},
    {4, "an yi dian",    "暗一点"},
    {5, "zui liang",     "最亮"},
};
#define CMD_COUNT (int)(sizeof(kCmds) / sizeof(kCmds[0]))

static int sBrightness = 0;  // 0 = 关，1..100 = 开
// 语音任务和按键任务都会改亮度，用互斥锁保护"读-改-写"，否则连按两下可能丢一次。
static SemaphoreHandle_t sStateMux = NULL;

// ---- 灯光 -----------------------------------------------------------------
// 今天只做开关：GPIO5 上没有 PWM 调光，亮度只记状态不复现。
// 真要调光需要把 lighting.cpp 的 LEDC 部分移植过来（docs/18 §七 第 1 项）。
static void applyLed(void) {
    gpio_set_level(PIN_LED, sBrightness > 0 ? 1 : 0);
}

// 唯一的亮度写入口。src 只用于日志，能一眼看出这次是语音还是按键触发的。
static void setLevel(int level, const char *src) {
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    if (sStateMux != NULL) xSemaphoreTake(sStateMux, portMAX_DELAY);
    sBrightness = level;
    applyLed();
    if (sStateMux != NULL) xSemaphoreGive(sStateMux);
    ESP_LOGW(TAG, ">>> %s level=%d LED=%s", src, level, level ? "ON" : "OFF");
}

static void handleCommand(int id) {
    const int level = sBrightness;
    switch (id) {
        case 1: setLevel(70, "voice:on"); break;
        case 2: setLevel(0, "voice:off"); break;
        case 3: setLevel(level + 20 > 100 ? 100 : level + 20, "voice:up"); break;
        case 4: setLevel(level < 25 ? 5 : level - 20, "voice:down"); break;
        case 5: setLevel(100, "voice:max"); break;
        default:
            ESP_LOGW(TAG, "unknown command id %d", id);
            return;
    }
}

// ---- 按键 -----------------------------------------------------------------
static inline bool btnRawPressed(void) {
    const int level = gpio_get_level(PIN_BTN);
#if BTN_ACTIVE_LOW
    return level == 0;
#else
    return level == 1;
#endif
}

// 短按 = 开/关；长按 = 最亮。和 Arduino 主固件 onLightButton() 的语义保持一致。
static void buttonTask(void *arg) {
    (void)arg;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BTN,
        .mode = GPIO_MODE_INPUT,
#if BTN_ACTIVE_LOW
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
#else
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
#endif
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_LOGI(TAG, "btn GPIO%d idle=%d (%s)", PIN_BTN, gpio_get_level(PIN_BTN),
             btnRawPressed() ? "PRESSED -- 松开手还读按下，说明极性反了" : "released");

    bool stable = btnRawPressed();
    bool lastRead = stable;
    bool longFired = false;
    int64_t pressedAt = 0;
    int64_t lastChange = esp_timer_get_time() / 1000;

    while (1) {
        const int64_t now = esp_timer_get_time() / 1000;
        const bool raw = btnRawPressed();

        if (raw != lastRead) {
            lastRead = raw;
            lastChange = now;
        }
        if ((now - lastChange) >= BTN_DEBOUNCE_MS && raw != stable) {
            stable = raw;
            if (stable) {
                pressedAt = now;
                longFired = false;
            } else if (!longFired) {
                // 松开的那一刻才认一次短按，避免按住不放连续翻转
                setLevel(sBrightness > 0 ? 0 : 70, "key:toggle");
            } else {
                longFired = false;  // 长按已经执行过，松手不再补一次短按
            }
        }
        if (stable && !longFired && (now - pressedAt) >= BTN_LONGPRESS_MS) {
            longFired = true;
            setLevel(100, "key:max");
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ---- 麦克风 ---------------------------------------------------------------
static esp_err_t micInit(void) {
    i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chanCfg.dma_desc_num = 8;
    chanCfg.dma_frame_num = 256;

    esp_err_t err = i2s_new_channel(&chanCfg, NULL, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(err));
        return err;
    }

    // 注意：I2S_STD_*_DEFAULT_CONFIG 都是 "{ ... }" 形式的**初始化器列表**，
    // 不是表达式，所以只能在定义时用，不能 cfg.xxx = MACRO。
    // Arduino 那边的 pdm_mic.cpp 能用是因为 C++ 允许聚合初始化当表达式。
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_MIC_BCLK,
            .ws = PIN_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = PIN_MIC_SD,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    // INMP441 输出 24 bit、MSB 对齐在 32 bit 槽里，且要求 WS 周期 64 BCLK
    // （每声道 32 bit）。用 16 bit 槽只有 32 BCLK，麦克风会错位输出低频漂移。
    // 这段和 firmware/src/pdm_mic.cpp:66-73 的已验证配置一致，别改。
    cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;  // INMP441 的 L/R 接 GND = 左声道

    err = i2s_channel_init_std_mode(s_rx, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode: %s", esp_err_to_name(err));
        return err;
    }

    err = i2s_channel_enable(s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "mic ready %d Hz (BCLK=%d WS=%d SD=%d)",
             MIC_RATE, PIN_MIC_BCLK, PIN_MIC_WS, PIN_MIC_SD);
    return ESP_OK;
}

// ---- 主任务 ---------------------------------------------------------------
static void srTask(void *arg) {
    (void)arg;

    // 1) 挂载 model 分区里的模型包（srmodels.bin 由 idf.py flash 自动烧到 model 分区）
    srmodel_list_t *models = esp_srmodel_init("model");
    if (models == NULL) {
        ESP_LOGE(TAG, "esp_srmodel_init failed: model partition missing or empty");
        ESP_LOGE(TAG, "check partitions.csv has a 'model' partition and flash included srmodels.bin");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "model partition: %d model(s)", models->num);

    // 2) 选中文 MultiNet
    char *mnName = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (mnName == NULL) {
        ESP_LOGE(TAG, "no Chinese MultiNet model found; enable CONFIG_SR_MN_CN_MULTINET7_QUANT");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "MultiNet model: %s", mnName);

    esp_mn_iface_t *multinet = esp_mn_handle_from_name(mnName);
    if (multinet == NULL) {
        ESP_LOGE(TAG, "esp_mn_handle_from_name failed");
        vTaskDelete(NULL);
        return;
    }
    model_iface_data_t *modelData = multinet->create(mnName, MN_WINDOW_MS);
    if (modelData == NULL) {
        ESP_LOGE(TAG, "multinet->create failed (out of PSRAM?)");
        vTaskDelete(NULL);
        return;
    }

    // 3) 注册命令词。2.6.x 必须先 alloc 链表，否则 add 返回 ESP_ERR_INVALID_STATE
    ESP_ERROR_CHECK(esp_mn_commands_alloc(multinet, modelData));
    esp_mn_commands_clear();
    for (int i = 0; i < CMD_COUNT; i++) {
        esp_err_t err = esp_mn_commands_add(kCmds[i].id, kCmds[i].pinyin);
        ESP_LOGI(TAG, "add [%d] %-14s (%s) -> %s",
                 kCmds[i].id, kCmds[i].pinyin, kCmds[i].label, esp_err_to_name(err));
    }
    esp_mn_error_t *errs = esp_mn_commands_update();
    if (errs != NULL) {
        ESP_LOGW(TAG, "some commands were rejected, check pinyin spelling");
    }
    esp_mn_commands_print();

    // 4) AFE
    afe_config_t *afeConfig = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (afeConfig == NULL) {
        ESP_LOGE(TAG, "afe_config_init failed");
        vTaskDelete(NULL);
        return;
    }
    // INMP441 是裸麦，没有自动增益。增益已经前移到 MIC_PRE_GAIN（AFE 之前），
    // 那里补了 8 倍。afe_linear_gain 保持 1.0，两级叠加会削顶。
    // （4.0 时代的实测见 MIC_PRE_GAIN 的注释。）
    afeConfig->afe_linear_gain = 1.0f;
    ESP_LOGI(TAG, "afe linear gain=%.1f", afeConfig->afe_linear_gain);
    const esp_afe_sr_iface_t *afeHandle = esp_afe_handle_from_config(afeConfig);
    esp_afe_sr_data_t *afeData = afeHandle->create_from_config(afeConfig);
    if (afeData == NULL) {
        ESP_LOGE(TAG, "afe create failed (PSRAM not enabled?)");
        vTaskDelete(NULL);
        return;
    }

    int feedChunk = afeHandle->get_feed_chunksize(afeData);
    int feedNch = afeHandle->get_feed_channel_num(afeData);
    int mnChunk = multinet->get_samp_chunksize(modelData);
    ESP_LOGI(TAG, "afe feed=%d nch=%d | mn chunk=%d", feedChunk, feedNch, mnChunk);
    if (feedChunk != mnChunk) {
        ESP_LOGE(TAG, "chunk mismatch: afe=%d mn=%d (MultiNet needs the same frame size)", feedChunk, mnChunk);
    }

    int32_t *raw = (int32_t *)malloc((size_t)feedChunk * feedNch * sizeof(int32_t));
    int16_t *pcm = (int16_t *)malloc((size_t)feedChunk * feedNch * sizeof(int16_t));
    if (raw == NULL || pcm == NULL) {
        ESP_LOGE(TAG, "buffer alloc failed");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGW(TAG, "==================================================");
    ESP_LOGW(TAG, "LAMP-SR ready (window=%dms, auto-reopen)", MN_WINDOW_MS);
    ESP_LOGW(TAG, "say directly: da kai deng / guan deng / liang yi dian / an yi dian / zui liang");
#if GATE_BY_WAKEUP
    ESP_LOGW(TAG, "wake word is optional: it only blinks the LED");
#endif
    ESP_LOGW(TAG, "==================================================");

    int loggedRms = 0;
    int sMnTimeouts = 0;
    int32_t sPeak = 0;
    int64_t sSqSum = 0;
    int64_t sSqCnt = 0;
    int64_t sLastVadLog = 0;
    while (1) {
        size_t bytesRead = 0;
        esp_err_t err = i2s_channel_read(s_rx, raw,
                                         (size_t)feedChunk * feedNch * sizeof(int32_t),
                                         &bytesRead, portMAX_DELAY);
        if (err != ESP_OK || bytesRead == 0) {
            ESP_LOGW(TAG, "i2s read: %s (%u bytes)", esp_err_to_name(err), (unsigned)bytesRead);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        int n = (int)(bytesRead / sizeof(int32_t));
        // INMP441 的有效位在高 16 bit，右移取回；同一循环里做前置增益 + 饱和钳位。
        // 钳位不能省：不钳的话凑近说话会整数溢出翻转成反向尖峰。
        for (int i = 0; i < n; i++) {
            int32_t v = (int32_t)(raw[i] >> 16) * MIC_PRE_GAIN;
            if (v > 32767) {
                v = 32767;
            } else if (v < -32768) {
                v = -32768;
            }
            pcm[i] = (int16_t)v;
        }

        // 电平统计：每 2 秒汇总一次。
        // 注意必须累计整段 60 帧，不能只看最后一帧 —— 一帧只有 32 ms，落在
        // 两个音节之间就是一片安静，读数会骗人（之前就因此误判"用户没说话"）。
        for (int i = 0; i < n; i++) {
            int32_t v = pcm[i] < 0 ? -pcm[i] : pcm[i];
            if (v > sPeak) sPeak = v;
        }
        {
            int64_t sum = 0;
            for (int i = 0; i < n; i++) {
                sum += (int64_t)pcm[i] * pcm[i];
            }
            sSqSum += sum;
            sSqCnt += n;
        }
        if (++loggedRms >= 60) {
            loggedRms = 0;
            ESP_LOGI(TAG, "mic peak=%d rms=%d", (int)sPeak,
                     sSqCnt ? (int)(sqrtf((float)sSqSum / sSqCnt)) : 0);
            sPeak = 0;
            sSqSum = 0;
            sSqCnt = 0;
        }

        afeHandle->feed(afeData, pcm);
        afe_fetch_result_t *res = afeHandle->fetch(afeData);
        if (res == NULL || res->ret_value == ESP_FAIL) {
            continue;
        }

        // [DIAG] 临时诊断，定位"电平够但 MultiNet 零结果"。
        // 关键怀疑：AFE 输出的字节数是否等于 MultiNet 期望的 1024（512*2）。
        // 若不等，detect() 会一直读错位数据，永远不出结果。
        {
            static int sDiagCnt = 0;
            if (++sDiagCnt % 30 == 0) {
                // 关键对比：AFE 输入(pcm) 与 输出(res->data) 的峰值比。
                // 约等于 1 -> AFE 基本直通，问题不在 AFE 的增益链；
                // 远小于 1 -> AFE 在衰减，MultiNet 拿到的是被压过的信号。
                int16_t *afOut = (int16_t *)res->data;
                int half = res->data_size / 2;
                int32_t outPeak = 0, inPeak = 0;
                for (int i = 0; i < half; i++) {
                    int32_t a = afOut[i] < 0 ? -afOut[i] : afOut[i];
                    if (a > outPeak) outPeak = a;
                }
                for (int i = 0; i < feedChunk && i < half; i++) {
                    int32_t a = pcm[i] < 0 ? -pcm[i] : pcm[i];
                    if (a > inPeak) inPeak = a;
                }
                ESP_LOGI(TAG, "diag: afe_out=%d bytes (mn wants %d), ret=%d, vol=%.1f",
                         res->data_size, mnChunk * (int)sizeof(int16_t),
                         (int)res->ret_value, res->data_volume);
                ESP_LOGI(TAG, "diag: inPeak=%d outPeak=%d ratio=%.2f",
                         (int)inPeak, (int)outPeak, inPeak ? (float)outPeak / inPeak : 0.0f);
            }
            // 前 4 帧把原始波形打出来（每帧 160 点 = 10ms）。
            // 语音的相邻采样强相关（差分小）；若看到大幅跳变/大量 0，说明
            // I2S 取位或接线有问题，而不是"声音太小"。
            static int sDumpLeft = 4;
            if (sDumpLeft > 0) {
                sDumpLeft--;
                char buf[720];
                int p = 0;
                for (int i = 0; i < 160 && p < 700; i++) {
                    p += snprintf(buf + p, sizeof(buf) - p, "%d,", pcm[i]);
                }
                ESP_LOGI(TAG, "DUMP %s", buf);
            }
        }

        // VAD 是判断"声音有没有进到 AFE"的分水岭：
        //   VAD 认成语音但没有 MN 结果 -> 词表/发音问题（换个说法或加别名）
        //   VAD 始终 silence            -> 麦克风信号没进来（接线/距离/增益）
        if (res->vad_state == VAD_SPEECH) {
            int64_t now = esp_timer_get_time();
            if (now - sLastVadLog > 1000000) {
                sLastVadLog = now;
                ESP_LOGW(TAG, "vad=speech dbfs=%.1f", res->data_volume);
            }
        }

#if GATE_BY_WAKEUP
        if (res->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGW(TAG, "*** wake word detected, listening for command ***");
            // 闪一下表示"听到了"，否则用户不知道唤醒成功没有
            gpio_set_level(PIN_LED, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            applyLed();
            multinet->clean(modelData);
        }
#endif

        esp_mn_state_t mnState = multinet->detect(modelData, res->data);
        // [DIAG] 0=DETECTING 1=DETECTED 2=TIMEOUT。
        // 一直 0 说明数据在正常喂但声学上不匹配；出现 2 说明窗口在反复过期。
        {
            static int sMnCnt = 0;
            if (++sMnCnt % 30 == 0) {
                ESP_LOGI(TAG, "diag: mnState=%d", (int)mnState);
            }
        }
        if (mnState == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *r = multinet->get_results(modelData);
            if (r != NULL) {
                for (int i = 0; i < r->num; i++) {
                    ESP_LOGW(TAG, "MN id=%d prob=%.3f text=%s",
                             r->command_id[i], r->prob[i], r->string);
                    handleCommand(r->command_id[i]);
                }
            }
            multinet->clean(modelData);
        } else if (mnState == ESP_MN_STATE_TIMEOUT) {
            // 识别窗口过期。官方示例在这里停下、等唤醒词重开；
            // 本项目的目标是"直说就能开灯"，所以立刻 clean() 重开下一轮窗口。
            // 不写这段的话，开机 6 秒后就不再有任何识别结果 —— 表现就是
            // "先说你好小智再开灯可以，直接说开灯没反应"。
            multinet->clean(modelData);
            if (sMnTimeouts < 3) {
                ESP_LOGI(TAG, "mn window expired, reopened (normal when idle)");
            }
            sMnTimeouts++;
        }
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "booting LAMP-SR (offline command words)");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_LED,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(PIN_LED, 0);

    sStateMux = xSemaphoreCreateMutex();
    if (sStateMux == NULL) {
        ESP_LOGW(TAG, "mutex alloc failed, button and voice may race");
    }

    if (micInit() != ESP_OK) {
        ESP_LOGE(TAG, "mic init failed, abort");
        return;
    }

    // 给识别任务足够的栈和 PSRAM 之上的优先级；固定绑 core 1 避开 WiFi 任务
    xTaskCreatePinnedToCore(srTask, "sr_task", 8 * 1024, NULL, 5, NULL, 1);

    // 按键独立成一个任务：麦克风或模型初始化失败时，按键仍然能开关灯。
    // 优先级低于识别任务，每 10 ms 轮询一次，占不了多少 CPU。
    xTaskCreatePinnedToCore(buttonTask, "btn_task", 3 * 1024, NULL, 3, NULL, 1);
}
