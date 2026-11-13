# 31 网页配网与运行时 WiFi（2026-11-13）

> **文档性质**：在 `live.html` 实时监测台上加「连接 WiFi」之后写的实施记录。
>
> **一句话**：浏览器通过已有的 Web Serial 通道把 SSID / 密码发给设备，固件写进 NVS 并立即重连；换路由器、改密码不再需要重新烧固件。
>
> **边界**：这是**开发者工具**里的配网，不是产品说明书第 3.4 节的 SoftAP 手机配网。终端用户的「长按 3 秒进配网热点」仍按 `docs/07` / `docs/09` 的路线走。

---

## 0. 为什么放在 `live.html`

| 约束 | 结论 |
|------|------|
| 网页在 GitHub Pages（https），设备在 USB 上 | 只有 Web Serial 这条链是现成的，不需要服务器 |
| 换 WiFi 现在要改 `secrets.h` 再烧录 | 违背 `docs/09` 的 E3（换 WiFi 不重烧），调试成本高 |
| 页面已经有串口命令框 | 缺的不是通道，是「不用手打十六进制/转义」的 UI |
| 手机配网（SoftAP）还没做 | 先用桌面开发者工具把设备侧能力打通，产品化时复用同一套 NVS 前缀 |

页面上的入口是顶栏「连接 WiFi」。它和「连接设备」「播放录制」是同一级操作，不改变原有布局：窄屏时 `deck-actions` 自动换行。

---

## 1. 交互

| 控件 | 行为 |
|------|------|
| 顶栏「连接 WiFi」 | 打开 `<dialog>`；圆点灰色 = 未连接设备，琥珀色 = 已连接串口但设备未联网，绿色 = 设备已联网 |
| 「扫描」 | 发 `wifi scan`，把固件扫到的 2.4 GHz AP 按 RSSI 排序列出来（最多 20 个，隐藏 SSID 不显示） |
| 点扫描结果 | 把 SSID 填进输入框，焦点跳到密码框 |
| SSID / 密码 + 「保存并连接」 | 发 `wifi connect <ssid> <pass>`，等待 `[wifi] saved` → `[wifi] connected` 或失败原因 |
| 「清除已保存」 | 二次确认后发 `wifi clear`；NVS 清空后回落到 `secrets.h`，没有编译期凭据则保持未配置 |

面板里的状态条优先显示设备真实回执，不靠猜：

- `设备已联网 · SSID · IP · RSSI`
- `设备未联网 · 已保存「SSID」· 状态 N`
- `当前固件不认识 wifi 命令`（旧固件回 `unknown command: wifi`）
- 发送后 20 秒没有结果 → 提示用 `status` 或「扫描」进一步定位

---

## 2. 串口协议

网页 → 设备：

```text
wifi status
wifi scan
wifi connect <ssid> <pass>
wifi clear
```

- `<ssid>` / `<pass>` 用 `encodeURIComponent()` 编码，空格、引号、中文都不会把命令行拆坏；固件只解 `%XX`，不把 `+` 当空格。
- 密码为**单独一个 `-`** 表示开放网络（空密码）。密码本身就是 `-` 时，网页编码成 `%2D`。
- 命令行走 `ui.cpp` 的 `line[384]`，最长的合法输入（32 字节 SSID + 63 字节密码全中文）约 300 字符，留了余量。

设备 → 网页（`live-parser.js` 逐行识别）：

| 行 | 页面行为 |
|----|----------|
| `[wifi] credentials ssid="…" source=nvs\|secrets.h password=set\|empty` | 同步已保存的 SSID，不泄露密码 |
| `[wifi] connecting ssid="…"` | 进入「正在连接」 |
| `[wifi] connected ssid="…" ip=… rssi=…` | 绿色状态 + IP + RSSI，顶栏圆点变绿 |
| `[wifi] status=N <原因>` | 断线/重连失败写进事件流与面板提示（沿用 `wifiStatusName()` 的中文原因） |
| `[wifi] saved ssid="…" password=set\|empty` | 事件流记一条，进入等待连接 |
| `[wifi] cleared` | 清空页面上的 SSID / IP |
| `[wifi] scan: N AP(s) visible` + 逐行 AP + `scan done` | 扫描列表 |
| `[wifi] scan failed code=-2` | 扫描失败提示（旧固件的 `scan: -2` 也按失败处理） |
| `[wifi] bad ssid\|password\|nvs: …` | 参数错误提示 |
| `unknown command: wifi` | 旧固件检测，禁用按钮并提示重新烧录 |

---

## 3. 固件改动

### 3.1 凭据优先级

```text
NVS（lampwifi/ssid, lampwifi/pass）  >  secrets.h 的 LAMP_WIFI_SSID / LAMP_WIFI_PASS
```

- `wifiLoadCredentials()` 在 `setup()` 里先读 NVS；NVS 空才回落到编译期常量。
- `wifi connect` 写 NVS 后立即 `WiFi.disconnect(true)` + `WiFi.begin()`。
- `wifi clear` 删掉 NVS 两个键，再按上面的优先级重新决定连谁。
- `WiFi.persistent(false)`：凭据由我们自己存，不让 Arduino core 每 10 秒重试时反复写 flash。

### 3.2 不再依赖 `secrets.h` 才能编译联网代码

`config.h` 在 `secrets.h` 缺省时补空字符串：

```cpp
#ifndef LAMP_WIFI_SSID
#define LAMP_WIFI_SSID ""
#endif
#ifndef LAMP_WIFI_PASS
#define LAMP_WIFI_PASS ""
#endif
```

这带来两个变化：

1. CI 没有 `secrets.h` 也能编出**完整**的 WiFi / 语音代码（以前 `voice_client.cpp` 会退化成空实现）；
2. 串口配完网，语音链路立即可用，不需要为了「让固件认识 WiFi」再烧一次。

`main.cpp` 因此无条件包含 `<WiFi.h>` / `<Preferences.h>`，`wifiTick()` 不再被 `#if LAMP_WIFI_ENABLED` 包住。

### 3.3 连接、扫描与对时

- 开机先发一次 `WiFi.begin()`，之后 `wifiTick()` 每 10 秒重试并打印失败原因。
- 第一次重试前扫一次空口（先 `WiFi.disconnect(true)` + `delay(80)`），把「扫不到 SSID」和「密码错」分开。
- 手动 `wifi scan` 时：未连接先断射频再扫；扫完若还没连上，立即 `wifiStartConnect()`，不让页面停在断网状态。
- 连上的那一刻补一次 `configTime()`。运行时配网时 `cloudasr::begin()` 早已跑过，不补 SNTP 的话 TC3 签名会因时钟偏差失败（`docs/26`）。
- 扫描结果最多打印 20 个 AP，避免事件流被刷屏。

---

## 4. 安全与边界

| 项目 | 现状 |
|------|------|
| 密码回显 | 不回显。串口只打 `password=set\|empty`，页面输入框是 `type=password` |
| 传输 | 只在 USB 串口上，不经过任何服务器 |
| 存储 | 设备 NVS 明文（ESP32 没有安全芯片时的常规做法）；能物理接触设备的人可以读 |
| 热点频段 | 只支持 2.4 GHz；5 GHz 扫不到，面板已写明 |
| 多设备 | 串口一次只能连一台，页面同时只配置当前设备 |
| 旧固件 | 通过 `unknown command: wifi` 检测并提示重新烧录，不会静默失败 |
| 开放网络 | 密码留空 → 发 `-`，固件用空密码 `WiFi.begin()` |

---

## 5. 验证

| 项 | 方法 | 结果 |
|----|------|------|
| 解析与状态机 | `node tools/test-live-deck.js` | 38 项全通过（含扫描排序、保存/连接/清除、旧固件检测） |
| 真浏览器交互 | Chrome headless + CDP，注入假 Web Serial：连接 → 打开面板 → 扫描 → 填表 → 保存 → 连接成功/旧固件 | 23 项全通过，1440 / 500 宽截图无溢出 |
| 固件（带 `secrets.h`） | `pio run -e lamp_s3_tencent` | SUCCESS |
| 固件（CI 路径，无 `secrets.h`） | 临时移走 `secrets.h`，`pio run -e lamp_s3 -e lamp_s3_bailian -e lamp_s3_tencent -e lamp_s3_lan` | 以 CI 为准，见 PR 检查 |
| 真机 | 未做（本轮没有接设备） | 串口日志预期：`[wifi] saved …` → `[wifi] connecting …` → `[wifi] connected …` |

> 页面上的「扫描」会阻塞固件主循环约 2 秒（同步 `scanNetworks()`）。这是开发者工具可以接受的代价；产品化时应改成异步扫描或 SoftAP 配网。

---

## 6. 后续

1. **SoftAP 配网**（产品向）：设备开热点 + 网页填表，复用同一个 NVS 命名空间，手机可用。
2. **扫描结果带加密方式**：现在只有 RSSI / 信道；加 `WiFi.encryptionType(i)` 后可以在 UI 上区分开放/加密网络。
3. **按键重置**：长按组合键清 NVS，替代必须连网页才能 `wifi clear`。
4. **多 SSID**：NVS 存 2~3 组，按信号自动选，解决「家里/办公室两头带」。
