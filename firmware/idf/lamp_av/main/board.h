/*
 * 引脚表。面包板模式，与 firmware/include/config.h 的 LAMP_BOARD_BREADBOARD 一致。
 *
 * 和 Arduino 版的一处关键区别：GPIO19/20 在这里**留给 CAM12 的 USB**（Host 模式），
 * 所以日志只能走 UART0（COM5），不能再当 USB-Serial-JTAG/CDC 用。
 */
#pragma once

#include "soc/gpio_num.h"

/* ---- 灯（HW-479，G 通道接 GPIO5；暖通道面包板未接） ---------------------- */
#define PIN_LED_WARM GPIO_NUM_21
#define PIN_LED_COOL GPIO_NUM_5

/* ---- 按键（HW-483） ------------------------------------------------------ */
#define PIN_BTN GPIO_NUM_4

/* ---- INMP441（M2 默认音频源） -------------------------------------------
 * 标准 I2S 麦，不是 PDM：BCLK=16、WS/LRCLK=18、SD=17，L/R 接 GND 选左声道。
 * 这三根和 USB 的 19/20 不冲突，所以"UAC 咪头"和"INMP441"可以同时接在板上做 A/B。
 */
#define PIN_MIC_BCLK GPIO_NUM_16
#define PIN_MIC_WS GPIO_NUM_18
#define PIN_MIC_SD GPIO_NUM_17
