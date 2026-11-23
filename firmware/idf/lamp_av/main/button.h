#pragma once

/*
 * 按键任务（HW-483，GPIO4）。
 *
 * 语义与 Arduino main.cpp 的 onLightButton() 一致：
 *   短按（松开时判定） 开/关
 *   长按 1200 ms      预设循环
 * 独立成一个任务：麦克风或模型初始化失败时，按键仍然能开关灯。
 */
void button_start(void);
