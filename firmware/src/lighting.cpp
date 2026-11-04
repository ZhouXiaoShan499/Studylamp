#include "lighting.h"

#include "pwm.h"

namespace lighting {
namespace {

struct CalPoint {
    uint16_t kelvin;
    uint8_t warm_pct;  // share of the total optical output on the warm channel
    uint8_t cool_pct;
};

// Five calibrated presets (measure with a CCT meter and adjust in NVS-backed
// calibration later; these are the nominal starting values).
constexpr CalPoint kCal[] = {
    {2700, 100, 0},
    {3000, 96, 8},
    {3500, 82, 24},
    {4300, 58, 56},
    {5200, 32, 82},
    {6500, 0, 100},
};
constexpr size_t kCalCount = sizeof(kCal) / sizeof(kCal[0]);

constexpr uint16_t kDutyMax = (1u << LED_PWM_BITS) - 1;

// Gamma 2.2 lookup for 0..100 %.
uint16_t gammaLut[101];
bool lutReady = false;

float currentWarm = 0.0f;
float currentCool = 0.0f;
float scale = 1.0f;
uint8_t targetPct = 60;
uint16_t targetKelvin = 4300;
Preset currentPreset = Preset::Study;
bool powered = true;

void buildLut() {
    if (lutReady) return;
    for (uint16_t i = 0; i <= 100; ++i) {
        const float linear = static_cast<float>(i) / 100.0f;
        gammaLut[i] = static_cast<uint16_t>(powf(linear, 2.2f) * kDutyMax + 0.5f);
    }
    lutReady = true;
}

void channelsFor(uint16_t kelvin, uint8_t* warm_pct, uint8_t* cool_pct) {
#if LAMP_SINGLE_CHANNEL
    // 面包板只接了一路灯（HW-479 的 G 在 PIN_LED_COOL）：亮度全给冷通道。
    // 否则默认 4300K 混光会把 44% 分给根本没接的暖通道，看着像"灯不够亮"。
    (void)kelvin;
    *warm_pct = 0;
    *cool_pct = 100;
    return;
#else
    if (kelvin <= kCal[0].kelvin) {
        *warm_pct = kCal[0].warm_pct;
        *cool_pct = kCal[0].cool_pct;
        return;
    }
    if (kelvin >= kCal[kCalCount - 1].kelvin) {
        *warm_pct = kCal[kCalCount - 1].warm_pct;
        *cool_pct = kCal[kCalCount - 1].cool_pct;
        return;
    }
    for (size_t i = 1; i < kCalCount; ++i) {
        if (kelvin <= kCal[i].kelvin) {
            const uint16_t span = kCal[i].kelvin - kCal[i - 1].kelvin;
            const float t = static_cast<float>(kelvin - kCal[i - 1].kelvin) / span;
            const float warm = kCal[i - 1].warm_pct + (kCal[i].warm_pct - kCal[i - 1].warm_pct) * t;
            const float cool = kCal[i - 1].cool_pct + (kCal[i].cool_pct - kCal[i - 1].cool_pct) * t;
            *warm_pct = static_cast<uint8_t>(warm + 0.5f);
            *cool_pct = static_cast<uint8_t>(cool + 0.5f);
            return;
        }
    }
#endif
}

// Max change per 20 ms tick so a 0..100 % ramp takes about LIGHT_RAMP_MS.
float maxStep() { return (static_cast<float>(kDutyMax) * 20.0f) / LIGHT_RAMP_MS; }

}  // namespace

void begin() {
    buildLut();
    pwm::attach(PIN_LED_WARM, CH_LED_WARM, LED_PWM_HZ, LED_PWM_BITS);
    pwm::attach(PIN_LED_COOL, CH_LED_COOL, LED_PWM_HZ, LED_PWM_BITS);
    pwm::write(PIN_LED_WARM, CH_LED_WARM, 0);
    pwm::write(PIN_LED_COOL, CH_LED_COOL, 0);
    currentWarm = currentCool = 0.0f;
    Serial.printf("[light] warm=%u cool=%u pwm=%luHz bits=%u\n", PIN_LED_WARM, PIN_LED_COOL,
                  static_cast<unsigned long>(LED_PWM_HZ), LED_PWM_BITS);
}

void setBrightness(uint8_t pct) {
    if (pct > LIGHT_MAX_PCT) pct = LIGHT_MAX_PCT;
    targetPct = pct;
    powered = pct > 0;
    if (pct > 0 && pct < LIGHT_MIN_PCT) targetPct = LIGHT_MIN_PCT;
    currentPreset = Preset::Manual;
}

void setCct(uint16_t kelvin) {
    if (kelvin < 2700) kelvin = 2700;
    if (kelvin > 6500) kelvin = 6500;
    targetKelvin = kelvin;
    currentPreset = Preset::Manual;
}

void setPreset(Preset value) {
    switch (value) {
        case Preset::Study:
            targetKelvin = 4300;
            targetPct = 100;
            break;
        case Preset::Office:
            targetKelvin = 5200;
            targetPct = 80;
            break;
        case Preset::Rest:
            targetKelvin = 2700;
            targetPct = 35;
            break;
        case Preset::Night:
            targetKelvin = 2700;
            targetPct = LIGHT_MIN_PCT;
            break;
        case Preset::Manual:
            break;
    }
    powered = targetPct > 0;
    currentPreset = value;
}

void toggle() {
    if (isOn()) {
        setBrightness(0);
        currentPreset = Preset::Manual;
    } else {
        setBrightness(targetPct == 0 ? 60 : targetPct);
        powered = true;
        currentPreset = Preset::Manual;
    }
}

void setScale(float value) {
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    scale = value;
}

void clearScale() { scale = 1.0f; }

void flush() {
    buildLut();
    uint8_t warmPct = 0, coolPct = 0;
    channelsFor(targetKelvin, &warmPct, &coolPct);
    const uint16_t pct = powered ? targetPct : 0;
    currentWarm = static_cast<float>(gammaLut[pct]) * warmPct / 100.0f * scale;
    currentCool = static_cast<float>(gammaLut[pct]) * coolPct / 100.0f * scale;
    pwm::write(PIN_LED_WARM, CH_LED_WARM, static_cast<uint32_t>(currentWarm + 0.5f));
    pwm::write(PIN_LED_COOL, CH_LED_COOL, static_cast<uint32_t>(currentCool + 0.5f));
}

void tick(uint32_t now) {
    (void)now;
    buildLut();

    uint8_t warmPct = 0, coolPct = 0;
    channelsFor(targetKelvin, &warmPct, &coolPct);

    const uint16_t pct = powered ? targetPct : 0;
    float targetWarm = static_cast<float>(gammaLut[pct]) * warmPct / 100.0f * scale;
    float targetCool = static_cast<float>(gammaLut[pct]) * coolPct / 100.0f * scale;

    const float step = maxStep();
    const auto approach = [step](float cur, float target) -> float {
        if (cur < target) return (cur + step < target) ? cur + step : target;
        if (cur > target) return (cur - step > target) ? cur - step : target;
        return cur;
    };
    currentWarm = approach(currentWarm, targetWarm);
    currentCool = approach(currentCool, targetCool);

    pwm::write(PIN_LED_WARM, CH_LED_WARM, static_cast<uint32_t>(currentWarm + 0.5f));
    pwm::write(PIN_LED_COOL, CH_LED_COOL, static_cast<uint32_t>(currentCool + 0.5f));
}

uint8_t brightness() { return targetPct; }
uint16_t cct() { return targetKelvin; }
bool isOn() { return powered && targetPct > 0; }
Preset preset() { return currentPreset; }

const char* presetName(Preset value) {
    switch (value) {
        case Preset::Study: return "study";
        case Preset::Office: return "office";
        case Preset::Rest: return "rest";
        case Preset::Night: return "night";
        case Preset::Manual: return "manual";
    }
    return "unknown";
}

}  // namespace lighting