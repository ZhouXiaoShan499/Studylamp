#include "lamp_state.h"

#include <stddef.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lighting.h"

static const char *TAG = "state";

/* 上电默认值：与 Arduino lighting.cpp:35-38 的初值一致（60 % / 4300 K / 亮）。 */
#define BOOT_LEVEL 60
#define BOOT_KELVIN 4300

static SemaphoreHandle_t sMux = NULL;
static int sLevel = BOOT_LEVEL;
static uint16_t sKelvin = BOOT_KELVIN;
static lamp_preset_t sPreset = LAMP_PRESET_STUDY;
/* 「开灯」要能回到关灯前的亮度：任何一次 level>0 的写入都会刷新它。 */
static int sLastOnLevel = 70;

static void lock(void)
{
    if (sMux != NULL) {
        xSemaphoreTake(sMux, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (sMux != NULL) {
        xSemaphoreGive(sMux);
    }
}

static void push(const char *src)
{
    if (sLevel > 0) {
        sLastOnLevel = sLevel;
    }
    lighting_set_target(sLevel, sKelvin);
    ESP_LOGW(TAG, ">>> %-10s level=%3d kelvin=%u preset=%s", src, sLevel, (unsigned)sKelvin,
             lamp_state_preset_name(sPreset));
}

void lamp_state_init(void)
{
    sMux = xSemaphoreCreateMutex();
    if (sMux == NULL) {
        ESP_LOGW(TAG, "mutex alloc failed, three writers may race");
    }
    sLevel = BOOT_LEVEL;
    sKelvin = BOOT_KELVIN;
    sPreset = LAMP_PRESET_STUDY;
    lighting_set_target(sLevel, sKelvin);
    lighting_flush(); /* 上电直接亮，不从 0 斜坡上来 */
    ESP_LOGW(TAG, "boot %d%% @ %uK (%s)", sLevel, (unsigned)sKelvin, lamp_state_preset_name(sPreset));
}

int lamp_state_level(void)
{
    lock();
    const int v = sLevel;
    unlock();
    return v;
}

uint16_t lamp_state_kelvin(void)
{
    lock();
    const uint16_t v = sKelvin;
    unlock();
    return v;
}

lamp_preset_t lamp_state_preset(void)
{
    lock();
    const lamp_preset_t v = sPreset;
    unlock();
    return v;
}

bool lamp_state_is_on(void)
{
    return lamp_state_level() > 0;
}

int lamp_state_last_on_level(void)
{
    lock();
    const int v = sLastOnLevel;
    unlock();
    return v;
}

void lamp_state_set_level(int level, const char *src)
{
    lock();
    sLevel = level;
    sPreset = LAMP_PRESET_MANUAL;
    push(src);
    unlock();
}

void lamp_state_set_kelvin(uint16_t kelvin, const char *src)
{
    lock();
    sKelvin = kelvin;
    sPreset = LAMP_PRESET_MANUAL;
    push(src);
    unlock();
}

void lamp_state_set_preset(lamp_preset_t preset, const char *src)
{
    lock();
    switch (preset) {
        case LAMP_PRESET_STUDY:
            sKelvin = 4300;
            sLevel = 100;
            break;
        case LAMP_PRESET_OFFICE:
            sKelvin = 5200;
            sLevel = 80;
            break;
        case LAMP_PRESET_REST:
            sKelvin = 2700;
            sLevel = 35;
            break;
        case LAMP_PRESET_NIGHT:
            sKelvin = 2700;
            sLevel = 1; /* Arduino LIGHT_MIN_PCT */
            break;
        case LAMP_PRESET_MANUAL:
        default:
            break;
    }
    sPreset = preset;
    push(src);
    unlock();
}

void lamp_state_toggle(const char *src)
{
    lock();
    sLevel = (sLevel > 0) ? 0 : 60; /* 与 Arduino toggle() 一致：灭了就回到 60 % */
    sPreset = LAMP_PRESET_MANUAL;
    push(src);
    unlock();
}

void lamp_state_next_preset(const char *src)
{
    lock();
    /* 只在四个实体预设之间循环，Manual 不参与（和 main.cpp:246-250 一致）。 */
    lamp_preset_t next;
    switch (sPreset) {
        case LAMP_PRESET_STUDY:
            next = LAMP_PRESET_OFFICE;
            break;
        case LAMP_PRESET_OFFICE:
            next = LAMP_PRESET_REST;
            break;
        case LAMP_PRESET_REST:
            next = LAMP_PRESET_NIGHT;
            break;
        default:
            next = LAMP_PRESET_STUDY;
            break;
    }
    unlock();
    lamp_state_set_preset(next, src);
}

const char *lamp_state_preset_name(lamp_preset_t preset)
{
    switch (preset) {
        case LAMP_PRESET_STUDY:
            return "study";
        case LAMP_PRESET_OFFICE:
            return "office";
        case LAMP_PRESET_REST:
            return "rest";
        case LAMP_PRESET_NIGHT:
            return "night";
        case LAMP_PRESET_MANUAL:
            return "manual";
    }
    return "unknown";
}
