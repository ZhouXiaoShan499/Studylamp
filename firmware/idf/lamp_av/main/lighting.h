#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * 双通道（暖/冷）LEDC 调光。
 *
 * 算法与 Arduino firmware/src/lighting.cpp 逐项对齐：
 *   1. Gamma 2.2：线性 PWM 在 5 %→20 % 之间看起来"跳"，2.2 曲线才均匀；
 *   2. 2700–6500 K 的暖/冷配比表（kCal），色温在表上做线性插值；
 *   3. 600 ms 满量程斜坡，每 20 ms 走一步。
 * 这三样必须一起搬，否则"网页写 60 %"在两边看起来不是一样亮。
 */

void lighting_init(void);

/* 设定目标（0..100，0 = 关灯）。实际占空比由 50 Hz 斜坡任务逼近。 */
void lighting_set_target(int level_pct, uint16_t kelvin);

/* 立即跳到目标，不做斜坡（上电、断电前用）。 */
void lighting_flush(void);

/* 当前实际占空比（12 bit），供日志/调试。 */
void lighting_duty(uint32_t *warm, uint32_t *cool);
