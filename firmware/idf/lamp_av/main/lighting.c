#include "lighting.h"

#include <math.h>
#include <stddef.h>

#include "board.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "light";

/* ---- 参数：全部对应 Arduino config.h / lighting.cpp -------------------- */
#define PWM_BITS 12
#define PWM_HZ 4000
#define PWM_DUTY_MAX ((1u << PWM_BITS) - 1)
#define LIGHT_RAMP_MS 600 /* config.h:103 */
#define RAMP_TICK_MS 20   /* lighting.cpp tick 周期 */
#define LIGHT_MIN_PCT 1   /* config.h:101 */
#define LIGHT_MAX_PCT 100 /* config.h:102 */

#define CH_WARM 0
#define CH_COOL 1

/* ---- 色温配比表：抄自 lighting.cpp:16-23 ------------------------------ */
typedef struct {
    uint16_t kelvin;
    uint8_t warm_pct;
    uint8_t cool_pct;
} cal_point_t;

static const cal_point_t kCal[] = {
    {2700, 100, 0}, {3000, 96, 8},  {3500, 82, 24},
    {4300, 58, 56}, {5200, 32, 82}, {6500, 0, 100},
};
#define CAL_COUNT (sizeof(kCal) / sizeof(kCal[0]))

static uint16_t sGamma[101];

static float sCurrentWarm = 0.0f;
static float sCurrentCool = 0.0f;
static int sTargetPct = 0;
static uint16_t sTargetKelvin = 4300;
static bool sPowered = false;
static float sScale = 1.0f;

/* ---- 纯函数 ------------------------------------------------------------ */

static void build_gamma(void)
{
    for (int i = 0; i <= 100; ++i) {
        const float linear = (float)i / 100.0f;
        sGamma[i] = (uint16_t)(powf(linear, 2.2f) * (float)PWM_DUTY_MAX + 0.5f);
    }
}

/* 照搬 lighting.cpp:49-80 的分段线性插值。 */
static void channels_for(uint16_t kelvin, uint8_t *warm_pct, uint8_t *cool_pct)
{
    if (kelvin <= kCal[0].kelvin) {
        *warm_pct = kCal[0].warm_pct;
        *cool_pct = kCal[0].cool_pct;
        return;
    }
    if (kelvin >= kCal[CAL_COUNT - 1].kelvin) {
        *warm_pct = kCal[CAL_COUNT - 1].warm_pct;
        *cool_pct = kCal[CAL_COUNT - 1].cool_pct;
        return;
    }
    for (size_t i = 1; i < CAL_COUNT; ++i) {
        if (kelvin <= kCal[i].kelvin) {
            const uint16_t span = kCal[i].kelvin - kCal[i - 1].kelvin;
            const float t = (float)(kelvin - kCal[i - 1].kelvin) / (float)span;
            const float warm = (float)kCal[i - 1].warm_pct + (float)(kCal[i].warm_pct - kCal[i - 1].warm_pct) * t;
            const float cool = (float)kCal[i - 1].cool_pct + (float)(kCal[i].cool_pct - kCal[i - 1].cool_pct) * t;
            *warm_pct = (uint8_t)(warm + 0.5f);
            *cool_pct = (uint8_t)(cool + 0.5f);
            return;
        }
    }
}

static void targets(float *warm, float *cool)
{
    uint8_t warm_pct = 0;
    uint8_t cool_pct = 0;
#if CONFIG_LAMP_SINGLE_CHANNEL
    /* 面包板只接了一路：亮度全给冷通道，否则配比会给没接线的暖通道分走一半。 */
    (void)sTargetKelvin;
    warm_pct = 0;
    cool_pct = 100;
#else
    channels_for(sTargetKelvin, &warm_pct, &cool_pct);
#endif
    const uint16_t pct = sPowered ? (uint16_t)sTargetPct : 0;
    *warm = (float)sGamma[pct] * (float)warm_pct / 100.0f * sScale;
    *cool = (float)sGamma[pct] * (float)cool_pct / 100.0f * sScale;
}

/* ---- 硬件 -------------------------------------------------------------- */

static void write_duty(uint32_t warm, uint32_t cool)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, CH_WARM, warm);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, CH_WARM);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, CH_COOL, cool);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, CH_COOL);
}

static void ledc_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_12_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = PWM_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t warm = {
        .gpio_num = PIN_LED_WARM,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = CH_WARM,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&warm));

    ledc_channel_config_t cool = {
        .gpio_num = PIN_LED_COOL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = CH_COOL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&cool));
}

static float approach(float cur, float target, float step)
{
    if (cur < target) {
        return (cur + step < target) ? cur + step : target;
    }
    if (cur > target) {
        return (cur - step > target) ? cur - step : target;
    }
    return cur;
}

/* ---- 斜坡任务：替代 Arduino loop() 里的 lighting::tick() ---------------- */

static void ramp_task(void *arg)
{
    (void)arg;
    const float step = (float)PWM_DUTY_MAX * (float)RAMP_TICK_MS / (float)LIGHT_RAMP_MS;

    while (true) {
        float tw = 0.0f;
        float tc = 0.0f;
        targets(&tw, &tc);
        sCurrentWarm = approach(sCurrentWarm, tw, step);
        sCurrentCool = approach(sCurrentCool, tc, step);
        write_duty((uint32_t)(sCurrentWarm + 0.5f), (uint32_t)(sCurrentCool + 0.5f));
        vTaskDelay(pdMS_TO_TICKS(RAMP_TICK_MS));
    }
}

/* ---- 对外 -------------------------------------------------------------- */

void lighting_init(void)
{
    build_gamma();
    ledc_init();
    write_duty(0, 0);
    sCurrentWarm = 0.0f;
    sCurrentCool = 0.0f;

#if CONFIG_LAMP_SINGLE_CHANNEL
    const char *mode = "single-channel (brightness all on cool)";
#else
    const char *mode = "dual-channel (CCT mix)";
#endif
    ESP_LOGI(TAG, "warm=%d cool=%d pwm=%dHz bits=%d gamma=2.2 cal=%d points | %s", PIN_LED_WARM,
             PIN_LED_COOL, PWM_HZ, PWM_BITS, (int)CAL_COUNT, mode);

    /* 斜坡任务优先级低于识别与按键：掉几帧只是"渐变慢一点"，不影响功能。 */
    xTaskCreatePinnedToCore(ramp_task, "light_ramp", 3 * 1024, NULL, 2, NULL, 1);
}

void lighting_set_target(int level_pct, uint16_t kelvin)
{
    if (level_pct < 0) {
        level_pct = 0;
    }
    if (level_pct > LIGHT_MAX_PCT) {
        level_pct = LIGHT_MAX_PCT;
    }
    /* 与 Arduino 一致：>0 但太小时抬到最小值，避免"要点着了却看不见"。 */
    if (level_pct > 0 && level_pct < LIGHT_MIN_PCT) {
        level_pct = LIGHT_MIN_PCT;
    }
    if (kelvin < 2700) {
        kelvin = 2700;
    }
    if (kelvin > 6500) {
        kelvin = 6500;
    }
    sTargetPct = level_pct;
    sTargetKelvin = kelvin;
    sPowered = level_pct > 0;
}

void lighting_flush(void)
{
    float tw = 0.0f;
    float tc = 0.0f;
    targets(&tw, &tc);
    sCurrentWarm = tw;
    sCurrentCool = tc;
    write_duty((uint32_t)(sCurrentWarm + 0.5f), (uint32_t)(sCurrentCool + 0.5f));
}

void lighting_duty(uint32_t *warm, uint32_t *cool)
{
    if (warm != NULL) {
        *warm = (uint32_t)(sCurrentWarm + 0.5f);
    }
    if (cool != NULL) {
        *cool = (uint32_t)(sCurrentCool + 0.5f);
    }
}
