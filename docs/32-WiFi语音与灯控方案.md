# 32 WiFi 语音与灯控（2026-11-16）

> **文档性质**：在 `firmware/src`（B 线 Arduino）上补一条无线人机通道之后的实施记录。
>
> **一句话**：设备自带一个网页控制台，手机连上同一个 WiFi（或设备自己的热点）就能说话、看回答、开关灯；USB 从此只是开发者诊断口。
>
> **边界**：这是台架功能，不是产品功能。没有鉴权、没有 HTTPS、没有喇叭；"回答出声"靠手机浏览器的语音合成，不是灯在说话。

---

## 0. 先澄清一件事：语音本来就走 WiFi

原来的"USB 连接能语音对话"容易被理解成"音频从 USB 传给电脑识别"。实际不是：

| 环节 | 走哪条链路 | 说明 |
|------|-----------|------|
| 拾音 | INMP441 → I2S | 板上 |
| 识别（ASR） | 设备 → 腾讯云 `asr.tencentcloudapi.com`（HTTPS） | 板内直连，TC3 签名 + SNTP |
| 语义（LLM） | 设备 → TokenHub（HTTPS） | 板内直连，Bearer |
| 结果展示 | 设备 → USB 串口 → 浏览器 `live.html` | **只有这一段依赖 USB** |
| 发命令 | 浏览器 → USB 串口 → 设备 | 同上 |

所以"改成 WiFi"不是把音频改道，而是把**人机界面**从串口搬到网络上。缺的东西很具体：设备上没有 HTTP 服务，浏览器除了 USB 没有别的门可以敲。

---

## 1. 为什么页面要放在设备上

| 方案 | 问题 |
|------|------|
| 继续用 GitHub Pages 的 `live.html`，连设备的 WebSocket | Pages 是 **https**，页面连 `ws://` 会被浏览器按混合内容拦掉；设备做 `wss://` 要证书和域名，台架阶段不值 |
| 电脑跑一个中继服务 | 又把"必须开电脑"请回来了，与"脱离 PC"的路线相反 |
| **设备自己提供 http 页面** | 没有混合内容问题，不依赖外网，热点模式下也能打开 ✅ |

代价是页面得编进固件。`web/device.html` 由 `tools/embed_console.py` 压成两个 C 数组（原始 27 KB / gzip 9 KB），烧录时一起进 Flash，不需要 LittleFS 上传。

---

## 2. 三种网络形态

| 形态 | 触发条件 | 地址 | 用途 |
|------|----------|------|------|
| **STA** | NVS 或 `secrets.h` 里有凭据，且连上了路由 | `http://<设备IP>/`、`http://lamp1.local/` | 日常使用；和手机同一网段 |
| **AP** | 从没配过网；或有凭据但 25 秒还没拿到 IP | 热点 `LAMP-1-XXXX`（密码 `lamp12345`）→ `http://192.168.4.1/` | 首次配网、路由器换了/密码改了 |
| **AP+STA** | 热点开着的时候配网成功 | 两个地址都能进 | 配网过程页面不断线 |

热点名取 MAC 后 3 字节，桌上有两台也不会撞。`AP+STA` 是刻意选的：如果配网时把热点关掉，手机页面会在填完密码的瞬间失去连接，用户以为设备坏了。

```
        ┌──────────────── 手机 / 电脑浏览器 ────────────────┐
        │  http://192.168.1.23/  或  http://192.168.4.1/     │
        └───────────────┬───────────────────┬───────────────┘
                        │ HTTP :80          │ WebSocket :81
                        ▼                   ▼
              ┌─────────────────────────────────────┐
              │ ESP32-S3                            │
              │  net_console                        │
              │   ├─ WebServer  /  /api/state       │
              │   ├─ WebSocketsServer  事件 / 命令   │
              │   ├─ ESPmDNS  lamp1.local           │
              │   └─ DNSServer（仅 AP 模式，做 captive portal）
              │  ui::logf ──► publish ──► 所有客户端 │
              │  voice / lighting / timers          │
              └──────────────┬──────────────────────┘
                             │ HTTPS（原本就有）
                             ▼
                    腾讯云 ASR / TokenHub LLM
```

---

## 3. 接口

### 3.1 HTTP

| 方法 | 路径 | 作用 |
|------|------|------|
| GET | `/` | 控制台页面；带 `Accept-Encoding: gzip` 时发压缩版 |
| GET | `/api/state` | 一份状态快照，页面每 15 秒对一次 |
| GET/POST | `/api/cmd?c=<命令>` | 执行一行命令（WebSocket 断线时的兜底） |
| GET | `/favicon.ico` | 204，避免 404 刷事件 |

`/api/state` 的字段：

```json
{
  "fw": "0.1.0-mvp",
  "uptime": 128,
  "heap": 214336,
  "clients": 1,
  "net":   { "mode": "sta|ap|ap+sta|down", "ssid": "...", "ip": "...", "rssi": -54, "ap": false, "host": "lamp1.local" },
  "light": { "on": true, "pct": 70, "cct": 4300, "preset": "study" },
  "voice": { "provider": "tencent", "ready": true, "listening": false, "mic": true }
}
```

### 3.2 WebSocket（端口 81）

服务端 → 客户端，**原样转发**串口上那几类行。这样网页解析逻辑和 `live-parser.js` 是同一套语义，不需要为网页再造一套协议：

| 行 | 含义 |
|----|------|
| `{"type":"hello","fw":"...","host":"lamp1.local"}` | 握手 |
| `{"type":"tick","up":128}` | 20 秒心跳，用来发现死连接 |
| `[ASR] 开灯` | 识别到的原句（**新增**：以前只有意图，没有原句） |
| `[REPLY] ……` | 大模型回答（自由问答时才有） |
| `[voice] recording (rms=420)` | 正在录音 |
| `{"ev":"light_set","pct":70,"src":"voice"}` | 灯状态变化 |
| `{"ev":"light_preset","name":"study"}` | 预设切换 |
| `{"ev":"console_up","ip":"..."}` / `{"ev":"ap_up","ssid":"..."}` | 网络就绪 |
| `{"ev":"timer_fire","id":3}` / `{"ev":"boot","fw":"..."}` | 其它既有事件 |

客户端 → 服务端，两种写法都收：

```text
{"cmd":"light 60"}
light 60
```

裸命令是为了 `curl` / `websocat` 调试时不用包一层 JSON。

### 3.3 命令

网页命令和串口命令走**同一个** `ui::runCommand()` → `dispatch()` → `onCommand()`，没有第二套实现。所以下面这些在两边行为完全一致：

```text
light <0-100> | light on | light off | light toggle     ← 新增三个词
mode study|office|rest|night
voice listen | voice say <text>
wifi connect <ssid> <pass> | wifi clear
timer <seconds> | daily HH:MM | tasks | del <id>
status | mic on|off|test | vad <rms> | asr | log
```

`light on` 的语义是"亮着就别动，灭着才点 70%"——页面上的电源按钮直接发 `light 0` / `light <上次亮度>`，不依赖它。

---

## 4. 页面

`web/device.html`，单文件、无外部依赖、移动优先：

| 区块 | 内容 |
|------|------|
| 语音 | 大字识别结果、回答文本、`对灯说话`（发 `voice listen`）、`朗读回答`开关 |
| 灯 | 电源、亮度滑杆（1–100%）、学习/办公/休息/夜灯四个预设 |
| 状态 | 网络形态、RSSI、云端 provider、麦克风、网页端数量、空闲堆 |
| WiFi 设置 | 当前 SSID/IP；填 SSID + 密码 → `wifi connect`；清除 → `wifi clear` |
| 事件 | 最近 30 条，时间 + 一句中文 |

### 4.1 为什么"朗读"放在浏览器

板子还没有 MAX98357 和扬声器（`docs/11` 的"会说"这一环）。WiFi 通道恰好让回答到了**一个有喇叭的设备**——手机。页面收到 `[REPLY]` 后用 `speechSynthesis`（zh-CN）念出来，于是不用等功放也能完成"问 → 答 → 听到"的闭环。

这是演示手段，不是产品方案：声音从手机出，不从灯出；锁屏、切后台都会停。

---

## 5. 固件改动

| 文件 | 改动 |
|------|------|
| `include/net_console.h` | 新增：`begin/tick/publish/apActive/url` |
| `src/net_console.cpp` | 新增：STA 兜底 AP、mDNS、HTTP 三路由、WebSocket、事件广播 |
| `include/ui.h` / `src/ui.cpp` | 新增 `setPublisher()`（事件旁路）、`emitf()`（只上串口+网页，不落日志）、`runCommand()`（串口/网页共用分发） |
| `src/voice_client.cpp` | `[ASR]`/`[REPLY]`/`recording` 改走 `ui::emitf`；灯动作后补 `light_set`/`light_preset` 事件；provider≥2 也上报原句 |
| `src/main.cpp` | 接 `netconsole::begin/tick`；`light on|off|toggle`；`status` 增加控制台地址；AP 在跑时不再 `WiFi.disconnect(true)` |
| `include/console_html.h` | **生成文件**：页面 raw + gzip 两个数组 |
| `tools/embed_console.py` | 新增：生成上面那个头文件，`--check` 可做 CI 校验 |
| `web/device.html` | 新增：控制台页面 |

### 5.1 和既有 `wifiTick()` 的分工（容易踩）

WiFi 生命周期仍归 `main.cpp`：NVS 凭据、10 秒重试、扫描、SNTP 对时都在那里。`net_console` 只做两件事：读状态、在"确实连不上"时兜底开热点。

**关键约束**：`WiFi.disconnect(true)` 会停掉整个射频。热点模式下一执行，页面立刻掉线，而用户正拿它配网。所以三处（`wifiStartConnect` / 重试扫描 / `wifiScanNow`）都改成：

```cpp
WiFi.disconnect(!netconsole::apActive());   // 热点在跑时只断 STA
```

---

## 6. 怎么用

1. 烧录（腾讯云版，语义问答需要它）：

   ```powershell
   cd firmware
   pio run -e lamp_s3_tencent -t upload
   pio device monitor -b 115200
   ```

2. 看串口：

   - 已有凭据：`[wifi] connected ...` 之后出现 `[net] console ready: http://192.168.x.x/`
   - 没凭据：`[net] AP up ssid="LAMP-1-XXXX" pass="lamp12345" ip=192.168.4.1`

3. 手机/电脑连同一个 WiFi（或直连热点），浏览器打开上面的地址（也可以试 `http://lamp1.local/`）。

4. 点「对灯说话」→ 对着灯说一句 → 约 2 秒后页面出字；自由问答会多一段回答并朗读。

5. 亮度滑杆 / 电源 / 预设直接控制灯；改 WiFi 在「WiFi 设置」里填。

> 页面第一次打开时状态是猜的，`/api/state` 回来后才对齐；WS 断了会自动重连，最多 5 秒一次。

---

## 7. 验证

| 项 | 方法 | 结果 |
|----|------|------|
| `lamp_s3_tencent` 编译 | `pio run -e lamp_s3_tencent` | SUCCESS，RAM 41.3%（135 328 B / 320 KB）/ Flash 17.9%（1 174 261 B / 6.25 MB 分区） |
| `lamp_s3` / `lamp_s3_bailian` / `lamp_s3_lan` | `pio run -e ...` | 三个环境均 SUCCESS（`LAMP_CLOUD_ASR=0` 也没被新代码拆坏） |
| 页面脚本语法 | `node --check`（抽出内联 `<script>`） | 通过 |
| 页面体积 | `tools/embed_console.py` | HTML 27 413 B，gzip 9 086 B |
| 真机 | — | **未做**：本轮没有接设备，串口/浏览器实测待补 |

---

## 8. 边界与风险

| 项 | 现状 | 产品化要做什么 |
|----|------|----------------|
| 鉴权 | 无。同网段任何人打开 IP 就能开灯、发命令 | 设备生成 token，页面首次配对；或按键确认 |
| 传输 | 明文 http / ws；`wifi connect` 的密码在局域网里明文可见 | HTTPS + wss（自签或配网时下发证书） |
| 热点密码 | 固定 `lamp12345` | 每台独立，印在机身/包装 |
| 回答出声 | 手机 TTS，不是灯 | MAX98357 + 扬声器到位后改板载 TTS |
| 手机当麦克风 | **没做**。`getUserMedia` 要求 secure context，`http://192.168.4.1` 不算 | 上 HTTPS 后再做，代码路径已想好（POST PCM → `cloudasr::transcribe`） |
| mDNS | 部分路由器开了 AP 隔离/组播过滤就解析不到 `lamp1.local` | 页面里显示 IP 兜底，文档写明 |
| AP 模式扫描 | 射频被热点占着，`WiFi.scanNetworks()` 可能返回 -2 | 页面不做扫描，手工填 SSID |
| 25 秒兜底 | 密码错也会开热点，看起来像"设备自己变热点了" | 预期行为；串口 `wifi status` 能看到真实原因 |
| 端口 | 80 / 81 被占用；USB 串口仍可同时用 | — |

---

## 9. 后续

1. **HTTPS + 手机麦克风**：手机录音 → 上传 16 kHz PCM → 复用 `cloudasr::transcribe`。这样没有 INMP441 也能对话，也是唯一能把"手机贴嘴边说"做对的路径。
2. **访问 token**：NVS 里生成一次性配对码，页面首次输入，之后存 localStorage。
3. **TTS 上板**：功放到货后，`[REPLY]` 同时在板端合成，页面朗读变成可选。
4. **按键进配网**：长按组合键清 NVS 并开热点，替代"必须连网页才能 `wifi clear`"。
5. **`live.html` 定位**：保留为开发者工具（麦克风电平、VAD、原始事件流）；`device.html` 是用户口。两者共用同一套事件语义，不再各写一份解析。
