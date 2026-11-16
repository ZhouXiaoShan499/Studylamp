#pragma once
// WiFi 控制台：把原来只在 USB 串口上存在的「看识别结果 + 发命令」搬到网络上。
//
// 三条链路的分工：
//   - 语音识别 / LLM 一直在设备内部走 WiFi 上云（cloud_asr），USB 从来不是必需；
//   - USB 串口（web/live.html + Web Serial）保留为开发者诊断口；
//   - 本模块给终端用户一条无线通道：浏览器打开设备 IP 就能说话、看回答、开关灯。
//
// 联网本身由 wifi_link 负责（多网络表、开机自动选网）。从没配过网、或连了 25 秒
// 还没起来时，本模块自动开一个 SoftAP 兜底，手机直连热点即可配网和操控，不需要电脑。
//
// 页面本身编译进固件（web/device.html → include/console_html.h），不依赖外网，
// 也不依赖 GitHub Pages 的 https（https 页面连不上 ws://，这正是设备自带页面的理由）。

#include <Arduino.h>

namespace netconsole {

// hasCredentials 只是启动时的提示；运行期一律查 wifilink::hasCredentials()，
// 因为用户可能在热点页面里刚配完网。
void begin(bool hasCredentials);

// 必须在 loop() 里每轮调用：处理 HTTP / WebSocket / DNS。
void tick(uint32_t now);

// ui::Publisher 的实现：把一行事件推给所有网页客户端。
// 没有客户端时直接返回，不占内存。
void publish(const char* line);

// SoftAP 是否正在服务。main.cpp 据此决定能不能用 WiFi.disconnect(true)：
// 那会把射频整体停掉，热点连同页面一起断。
bool apActive();

// 页面地址（http://<ip>/）；网络还没起来时返回空串。
String url();

}  // namespace netconsole
