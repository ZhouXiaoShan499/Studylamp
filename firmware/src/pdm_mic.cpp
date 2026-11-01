#include "pdm_mic.h"

#include <math.h>

#if !LAMP_HAS_VOICE

namespace pdmmic {
bool begin() { return false; }
void end() {}
bool ready() { return false; }
size_t read(int16_t*, size_t, uint32_t) { return 0; }
float rms(const int16_t*, size_t) { return 0.0f; }
void diagnose() { Serial.println("[mic] disabled (LAMP_HAS_VOICE=0)"); }
}  // namespace pdmmic

#else

#include <esp_idf_version.h>
#include <freertos/FreeRTOS.h>

// INMP441 是标准 I2S 麦克风。IDF 5.x 用新的 channel API；更早的核心退回 legacy i2s。
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#define PDMMIC_USE_STD 1
#include <driver/i2s_std.h>
#else
#define PDMMIC_USE_STD 0
#include <driver/i2s.h>
#endif

namespace pdmmic {
namespace {

bool sReady = false;
uint32_t sLastErrorLog = 0;

#if PDMMIC_USE_STD
i2s_chan_handle_t sRx = nullptr;
#else
constexpr i2s_port_t kPort = I2S_NUM_0;
#endif

void logError(const char* what, int code) {
    const uint32_t now = millis();
    if (now - sLastErrorLog < 3000) return;  // do not flood the console
    sLastErrorLog = now;
    Serial.printf("[mic] %s failed: %d\n", what, code);
}

}  // namespace

bool begin() {
    if (sReady) return true;

#if PDMMIC_USE_STD
    i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chanCfg.dma_desc_num = 8;
    chanCfg.dma_frame_num = 256;

    esp_err_t err = i2s_new_channel(&chanCfg, nullptr, &sRx);
    if (err != ESP_OK) {
        logError("i2s_new_channel", static_cast<int>(err));
        sRx = nullptr;
        return false;
    }

    i2s_std_config_t cfg = {};
    cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE);
    // INMP441 输出 24 bit、MSB 对齐在 32 bit 槽里，且要求 WS 周期 64 BCLK
    // （每声道 32 bit）。用 16 bit 槽只有 32 BCLK，麦克风会错位输出低频漂移，
    // 所以这里必须用 32 bit 槽，读到 int32 后右移 16 位取高 16 bit。
    cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
    cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;  // L/R 接 GND = 左声道
    cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    cfg.gpio_cfg.bclk = static_cast<gpio_num_t>(PIN_VOICE_TX);   // INMP441 SCK
    cfg.gpio_cfg.ws = static_cast<gpio_num_t>(PIN_VOICE_WS);     // INMP441 WS
    cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    cfg.gpio_cfg.din = static_cast<gpio_num_t>(PIN_VOICE_RX);    // INMP441 SD
    cfg.gpio_cfg.invert_flags.mclk_inv = false;
    cfg.gpio_cfg.invert_flags.bclk_inv = false;
    cfg.gpio_cfg.invert_flags.ws_inv = false;

    err = i2s_channel_init_std_mode(sRx, &cfg);
    if (err != ESP_OK) {
        logError("i2s_channel_init_std_mode", static_cast<int>(err));
        i2s_del_channel(sRx);
        sRx = nullptr;
        return false;
    }

    err = i2s_channel_enable(sRx);
    if (err != ESP_OK) {
        logError("i2s_channel_enable", static_cast<int>(err));
        i2s_del_channel(sRx);
        sRx = nullptr;
        return false;
    }
#else
    i2s_config_t cfg = {};
    cfg.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
    cfg.sample_rate = MIC_SAMPLE_RATE;
    // 和 idf/lamp_voice 的已验证配置保持一致：INMP441 的 24 bit 数据左对齐在
    // 32 bit 槽里，且要求 WS 周期 64 BCLK（每声道 32 bit）。用 16 bit 槽只有
    // 32 BCLK，麦克风会错位输出、读数极小甚至全 0。
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
    // 按左右双声道成帧（每声道 32 bit -> WS 周期 64 BCLK，INMP441 必需），
    // 读取时只取左声道那一路。legacy API 的 mono 会缩短 WS 周期，不能用。
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = 8;
    cfg.dma_buf_len = 256;
    cfg.use_apll = false;
    cfg.tx_desc_auto_clear = false;
    cfg.fixed_mclk = 0;

    esp_err_t err = i2s_driver_install(kPort, &cfg, 0, nullptr);
    if (err != ESP_OK) {
        logError("i2s_driver_install", static_cast<int>(err));
        return false;
    }

    i2s_pin_config_t pins = {};
    pins.mck_io_num = I2S_PIN_NO_CHANGE;
    pins.bck_io_num = PIN_VOICE_TX;
    pins.ws_io_num = PIN_VOICE_WS;
    pins.data_out_num = I2S_PIN_NO_CHANGE;
    pins.data_in_num = PIN_VOICE_RX;
    err = i2s_set_pin(kPort, &pins);
    if (err != ESP_OK) {
        logError("i2s_set_pin", static_cast<int>(err));
        i2s_driver_uninstall(kPort);
        return false;
    }
    i2s_set_clk(kPort, MIC_SAMPLE_RATE, I2S_BITS_PER_SAMPLE_32BIT, I2S_CHANNEL_STEREO);
#endif

    sReady = true;
    Serial.printf("[mic] INMP441 I2S ready: %u Hz mono, BCLK=%u WS=%u SD=%u\n",
                  static_cast<unsigned>(MIC_SAMPLE_RATE), PIN_VOICE_TX, PIN_VOICE_WS, PIN_VOICE_RX);

    // 启动自检：立刻读一批。全 0 通常意味着 WS/BCLK 没接、L/R 悬空或模块没供电。
    // 统一走 read()，这样 32 bit 槽的两种实现都能得到同一份 16 bit 样本。
    int16_t probe[320];
    const size_t n = read(probe, 320, 300);
    if (n > 0) {
        int16_t mn = probe[0];
        int16_t mx = probe[0];
        for (size_t i = 1; i < n; ++i) {
            if (probe[i] < mn) mn = probe[i];
            if (probe[i] > mx) mx = probe[i];
        }
        Serial.printf("[mic] selftest n=%u min=%d max=%d raw=%04x %04x %04x %04x\n", static_cast<unsigned>(n),
                      mn, mx, static_cast<uint16_t>(probe[0]), n > 1 ? static_cast<uint16_t>(probe[1]) : 0,
                      n > 2 ? static_cast<uint16_t>(probe[2]) : 0, n > 3 ? static_cast<uint16_t>(probe[3]) : 0);
    } else {
        Serial.println("[mic] selftest read failed: 0 samples");
    }
    return true;
}

void end() {
    if (!sReady) return;
#if PDMMIC_USE_STD
    if (sRx != nullptr) {
        i2s_channel_disable(sRx);
        i2s_del_channel(sRx);
        sRx = nullptr;
    }
#else
    i2s_driver_uninstall(kPort);
#endif
    sReady = false;
}

bool ready() { return sReady; }

size_t read(int16_t* samples, size_t count, uint32_t timeout_ms) {
    if (!sReady || samples == nullptr || count == 0) return 0;
    if (count > 512) count = 512;  // 静态缓冲上限

#if PDMMIC_USE_STD
    // 32 bit 槽：INMP441 的 24 bit 数据在 int32 的高位，右移 16 位就是高 16 bit。
    static int32_t raw[512];
    size_t bytes_read = 0;
    const esp_err_t err =
        i2s_channel_read(sRx, raw, count * sizeof(int32_t), &bytes_read, pdMS_TO_TICKS(timeout_ms));
    if (err != ESP_OK) return 0;
    const size_t n = bytes_read / sizeof(int32_t);
    for (size_t i = 0; i < n; ++i) {
        int32_t value = raw[i] >> 16;
        if (MIC_GAIN > 1) {
            value *= MIC_GAIN;
            if (value > 32767) {
                value = 32767;
            } else if (value < -32768) {
                value = -32768;
            }
        }
        samples[i] = static_cast<int16_t>(value);
    }
    return n;
#else
    // 32 bit 立体声帧：[左, 右] 两个 int32 一帧，只取左声道（L/R 接 GND）。
    static int32_t raw[512 * 2];
    size_t bytes_read = 0;
    const size_t bytes_wanted = count * 2 * sizeof(int32_t);
    const esp_err_t err =
        i2s_read(kPort, raw, bytes_wanted, &bytes_read, pdMS_TO_TICKS(timeout_ms));
    if (err != ESP_OK) return 0;
    const size_t frames = bytes_read / (2 * sizeof(int32_t));
    for (size_t i = 0; i < frames; ++i) {
        int32_t value = raw[i * 2] >> 16;  // 24 bit 左对齐 -> 高 16 bit
        if (MIC_GAIN > 1) {
            value *= MIC_GAIN;
            if (value > 32767) {
                value = 32767;
            } else if (value < -32768) {
                value = -32768;
            }
        }
        samples[i] = static_cast<int16_t>(value);
    }
    return frames;
#endif
}

float rms(const int16_t* samples, size_t count) {
    if (samples == nullptr || count == 0) return 0.0f;

    // 先减帧均值：I2S 麦克风也有 DC 偏置，直接算 RMS 会把底噪抬高。
    int32_t sum = 0;
    for (size_t i = 0; i < count; ++i) sum += samples[i];
    const float mean = static_cast<float>(sum) / static_cast<float>(count);

    float acc = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        const float value = static_cast<float>(samples[i]) - mean;
        acc += value * value;
    }
    return sqrtf(acc / static_cast<float>(count));
}

void diagnose() {
    Serial.println("[mic] ---------- INMP441 诊断 ----------");
    if (!sReady) {
        Serial.println("[mic] 通道没打开");
        return;
    }

    // 采 1 秒（50 帧 × 20 ms），看有没有跳变
    int32_t sum = 0;
    int mn = 32767;
    int mx = -32768;
    int32_t nonzero = 0;
    size_t total = 0;
    int16_t buf[320];
    for (int i = 0; i < 50; ++i) {
        const size_t got = read(buf, 320, 200);
        if (got == 0) continue;
        for (size_t k = 0; k < got; ++k) {
            const int v = buf[k];
            sum += v;
            if (v < mn) mn = v;
            if (v > mx) mx = v;
            if (v != 0) ++nonzero;
        }
        total += got;
    }
    if (total == 0) {
        Serial.println("[mic] 1 秒内一个样本都没读到：查 I2S 通道/引脚");
        return;
    }
    Serial.printf("[mic] n=%u 均值=%ld min=%d max=%d 非零=%d%%\n", static_cast<unsigned>(total),
                  static_cast<long>(sum / static_cast<int32_t>(total)), mn, mx,
                  static_cast<int>(100 * nonzero / static_cast<int32_t>(total)));
    if (mn == 0 && mx == 0) {
        Serial.println("[mic] 全是 0：优先查 WS(GPIO18)/BCLK(GPIO16) 是否接好、L/R 是否接 GND、VDD 是否 3.3V");
    } else if (mx - mn < 16) {
        Serial.println("[mic] 幅度几乎没有变化：麦克风可能没在出数（或环境极安静）");
    } else {
        Serial.println("[mic] 有跳变：麦克风在出数，正常");
    }
    Serial.println("[mic] ---------- 诊断结束 ----------");
}

}  // namespace pdmmic

#endif  // LAMP_HAS_VOICE
