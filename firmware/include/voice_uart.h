#pragma once
// ---------------------------------------------------------------------------
// 离线语音模块串口（SU-03T / CI-03T 一类）。
//
// 模块把"命中了哪条命令"通过 UART 推给主控，帧格式沿用模块厂默认协议：
//   0xAA 0x55 <cmd> <len> <data...> <sum>
//   sum = (0xAA + 0x55 + cmd + len + sum(data)) & 0xFF
//
// 只用 RX 一根信号线：主控不反向配置模块，接三根（5V / GND / TX）就能跑。
// 5V 从开发板 5V 排针飞线，不上面包板 + 轨（见 docs/12 §6）。
//
// 默认 LAMP_VOICE_UART=0，模块没到货前这段代码不参与编译。
// ---------------------------------------------------------------------------

#include <Arduino.h>

#include "config.h"

namespace voiceuart {

// 命令 ID 与模块工具里配的"串口输出值"必须一一对应，改一边就得改另一边。
enum Command : uint8_t {
    kLightOn = 1,
    kLightOff = 2,
    kBrighter = 3,
    kDimmer = 4,
    kMax = 5,
    kNight = 6,
    kStudy = 7,
    kRest = 8,
};

using Handler = void (*)(uint8_t command);

// handler 在 poll() 内被调用，必须短、不许阻塞（会被主循环直接调用）。
void begin(uint32_t baud, Handler handler);
void poll();

bool enabled();
uint8_t lastCommand();
uint32_t badFrames();  // 校验失败 / 半截帧计数，接错线时先看这个

}  // namespace voiceuart
