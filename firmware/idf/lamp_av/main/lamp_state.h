#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * 灯的"业务状态"：亮度 / 色温 / 预设。所有写操作都从这里走，下游只认
 * lighting_set_target()，这样按键、语音、网页三条路径不会互相覆盖。
 *
 * 状态语义与 Arduino firmware/src/main.cpp + lighting.cpp 对齐：
 *   上电          60 % @ 4300 K，灯亮（插电即亮，和 Arduino 一致）
 *   短按          亮 → 灭；灭 → 60 %
 *   长按          预设循环 Study → Office → Rest → Night
 *   预设          Study 4300K/100%、Office 5200K/80%、Rest 2700K/35%、Night 2700K/1%
 */

typedef enum {
    LAMP_PRESET_STUDY = 0,
    LAMP_PRESET_OFFICE,
    LAMP_PRESET_REST,
    LAMP_PRESET_NIGHT,
    LAMP_PRESET_MANUAL,
} lamp_preset_t;

void lamp_state_init(void);

int lamp_state_level(void); /* 0..100，0 = 灭 */
uint16_t lamp_state_kelvin(void);
lamp_preset_t lamp_state_preset(void);
bool lamp_state_is_on(void);

/* 最近一次「亮着」时的亮度，供「开灯」回到原亮度（Arduino voiceLastOnPct 的等价物）。
 * 从未亮过时返回 70。 */
int lamp_state_last_on_level(void);

/* 唯一写入口。src 只用于日志，一眼看出这次是谁改的（key / voice / web）。 */
void lamp_state_set_level(int level, const char *src);
void lamp_state_set_kelvin(uint16_t kelvin, const char *src);
void lamp_state_set_preset(lamp_preset_t preset, const char *src);
void lamp_state_toggle(const char *src);
void lamp_state_next_preset(const char *src);

const char *lamp_state_preset_name(lamp_preset_t preset);
