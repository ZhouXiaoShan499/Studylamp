#pragma once
// 多网络自动联网：把「一条写死的 SSID」换成一个能记住多张网络、开机自己挑的表。
//
// 目标和笔记本一致：插上电就该有网，而不是先打开某个网页点「连接设备」。
//   1) 开机先试上次连上的那张 —— 家里/办公室来回搬，这一步通常 1~3 秒完成；
//   2) 不行就扫一遍空口，在「看得见的已保存网络」里按信号强弱依次试；
//   3) 全都不行退到 30 秒一轮的静默重试，netconsole 同时开热点兜底；
//   4) 用户可以从设备页或串口的扫描结果里挑一张，密码只写一次。
//
// 凭据存 NVS（命名空间 lampwifi）：
//   nets = [{"s":"ssid","p":"pass"}, ...]   最多 kMaxNetworks 张
//   last = 上次连上的 SSID（开机快路径）
// 老固件只写 ssid/pass 两个键，begin() 会把它们迁移进 nets，同时继续维护
// ssid/pass = 最近成功的那张，回刷旧固件也还能联网。

#include <Arduino.h>

namespace wifilink {

constexpr size_t kMaxNetworks = 5;

// 读 NVS、迁移旧格式、没凭据时用 secrets.h 播种。只做准备，不碰射频。
void begin();

// 外设初始化完之后调用，开始第一轮自动联网。
// 和 begin() 分开是为了避开开机瞬间的电流尖峰（I2S/射频抢资源）。
void start();

// loop() 里每轮调用，推进状态机。
void tick(uint32_t now);

bool hasCredentials();
size_t savedCount();
String savedSsid(size_t index);
int lastIndex();  // -1 = 没有记录

// 保存并立即连接。已存在的 SSID 只更新密码；表满时替换最久没用的那张。
bool connect(const String& ssid, const String& pass);

// 连接表里第 index 张（0 起），不动密码。给 `wifi use <n>` / 网页列表用。
bool connectSaved(size_t index);

// 删掉一张。删的是当前在用的那张时会断开并重新选网。
bool forget(const String& ssid);

// 清空所有凭据；secrets.h 里有编译期 SSID 时会回落到它。
void clearAll();

// 人为触发一轮重新挑选（网页/串口「换一个网络」）。
void autoConnectNow();

// 同步扫描，最多等 timeoutMs。返回可见 AP 数，失败返回 -1。
// 会短暂占用射频：正在连接时先中断，扫完由 tick() 自己恢复。
int scan(uint32_t timeoutMs = 6000);

int scanCount();
String scanSsid(int index);
int scanRssi(int index);
int scanChannel(int index);
bool scanSecure(int index);
// 命中的已保存网络下标；没保存过返回 -1。
int scanKnownIndex(int index);

bool online();
String activeSsid();
String ip();
int rssi();

// idle / connecting / scanning / online / retry
const char* phase();

// 正在扫描或连接。netconsole 用它决定「现在开热点会不会打断一次正在进行的连接」。
bool busy();

// 给 status 命令用的一行摘要（不带 [wifi] 前缀）。
String summary();

}  // namespace wifilink
