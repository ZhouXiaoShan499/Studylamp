# 41 Arduino 灯固件迁移 ESP-IDF 与 CAM12 语音控制方案

> **文档性质**：迁移方案。**本次只写方案，不改代码**；迁移是后续实施项。
>
> **关联**：[38 摄像头+语音一体化](38-摄像头语音一体化改造方案.md)、[39 两阶段计划](39-第一阶段与第二阶段实施计划.md)、[40 P0 实施](40-第二阶段连接路线与P0实施.md)、[23 技术栈](23-技术栈清单.md)、[32 网页控制台](32-WiFi语音与灯控方案.md)、[35 上板记录](35-上板验证与合并记录.md)。
>
> **后续**：[43 号](43-全量迁移与CAM12咪头开关灯可执行方案.md) 给出了执行级细化（逐文件映射、`audio_in` 抽象、M0–M7 门禁），并**修正本文 §0 的一条过严依赖**：M2（咪头→灯）只需 USB 设备能枚举出 UAC 输入，**不依赖 UVC 出图**。

---

## 0. 结论

| 问题 | 结论 |
|---|---|
| Arduino 灯固件能全部搬到 ESP-IDF 吗？ | **功能可以对齐，代码不能复制**；Arduino API/库要换成 IDF 等价物 |
| 要改多少？ | 核心（WiFi + 网页 + 云语音 + 按键/灯 + UAC）约 **60–80 小时**；全功能（IR/舵机/OLED）**+25–40 小时** |
| CAM12 麦克风能控制灯吗？ | **能**：UAC 咪头 → VAD → ASR/命令词 → GPIO5；前提是**USB 设备能枚举出 UAC 输入**（不需要摄像头出图，见 [43 号 §2](43-全量迁移与CAM12咪头开关灯可执行方案.md)） |
| 现在能开始吗？ | 先修摄像头（GPIO19=0/GPIO20=0）；摄像头通了再动 `lamp_av` |
| 推荐做法 | **增量迁移**：M1 骨架 → M2 离线命令词 → M3 云语音 → M4 视频 → M5 全功能 |

> 一句话：**搬得过去，但不是“复制粘贴”，是“按模块重写 + 复用业务逻辑”。**

---

## 1. 现在的两条固件线

| | Arduino `firmware/src` | ESP-IDF `firmware/idf/*` |
|---|---|---|
| 构建 | PlatformIO（`pio run`） | `idf.py`（idf.bat） |
| 代表固件 | `lamp_s3` / `lamp_s3_tencent` | `lamp_sr`（离线命令词）、P0 `usb_camera_mic_spk` |
| 已有能力 | WiFi 配网、网页控制台、云 ASR/LLM、按键/灯、定时、IR、PWM 舵机、OLED（可选） | esp-sr 离线命令词、按键/LED/NVS；P0 有 USB Host/UVC/UAC |
| 缺什么 | 没有 USB Host/UVC/UAC | 没有 WiFi/网页/云语音（`lamp_voice` 只是早期 TCP 客户端参考） |
| 运行状态 | 当前**不在**板上 | P0 **在**板上 |

迁移目标：新建 `firmware/idf/lamp_av`，把 Arduino 的业务能力搬到 IDF，并加上 P0 的 USB Host。

---

## 2. “全部搬”到底搬什么

### 2.1 可以直接复用的

- `local_intent.cpp`：本地意图表，纯 C++，几乎原样搬。
- `lighting.cpp`：PWM 逻辑；把 Arduino `ledc` 换成 IDF `ledc`。
- `timer_task.cpp`：定时逻辑；用 `esp_timer`/FreeRTOS。
- `tc3_sign.cpp`：TC3 签名；mbedtls 在 IDF 里同样可用。
- `config.h` 里的引脚/参数：改成 IDF 头文件或 Kconfig。
- `web/device.html`：页面本身可保留；换 HTTP/WS 服务端。
- `cloud_asr.cpp` 的业务逻辑（腾讯 ASR/LLM、WAV、base64、本地词表优先）：可复用逻辑，HTTP/TLS/JSON API 要换。

### 2.2 不能直接复制的

| Arduino 东西 | 为什么不能复制 | IDF 替代 |
|---|---|---|
| `WiFi.h` | Arduino 封装 | `esp_wifi` + `esp_netif` |
| `WebServer` / `WebSocketsServer` | Arduino 库 | `esp_http_server` + `esp_websocket_server` / `httpd_ws` |
| `HTTPClient` / `WiFiClientSecure` | Arduino 封装 | `esp_http_client` + `esp-tls`/mbedtls |
| `Preferences` | Arduino 封装 | `nvs_flash` / `nvs` |
| `ledcSetup/ledcWrite` | Arduino API | IDF `ledc` 驱动 |
| `IRremoteESP8266` | Arduino 专用库 | RMT + 自写/换 IR 库（最费事） |
| `Adafruit_SSD1306` / `Wire` | Arduino 库 | `esp_lcd` / I2C 驱动 / LVGL |
| `ArduinoJson` | 头文件库，理论上可在 IDF 用 | 可复用，或换 cJSON |

> 结论：**功能能对齐，代码不能整包搬。** 迁移是“按模块重写 + 复用业务逻辑”。

---

## 3. 模块映射与工作量

| Arduino 模块 | IDF 替代 | 难度 | 预计 |
|---|---|---|---|
| `main.cpp`（应用/命令/状态） | `app_main` + FreeRTOS 任务/`esp_timer` | 中 | 16–24h |
| `net_console.cpp`（HTTP/WS/mDNS/配网） | `esp_http_server` + `esp_websocket_server` + `mdns` + `esp_netif` DNS | 中高 | 16–24h |
| `wifi_link.cpp`（多网络/扫描/自动连） | `esp_wifi` + `esp_netif` + NVS | 中 | 12–16h |
| `cloud_asr.cpp`（百度/腾讯/百炼 ASR+LLM） | `esp_http_client` + `esp-tls` + cJSON/ArduinoJson | 高 | 20–30h |
| `tc3_sign.cpp`（TC3 签名） | mbedtls SHA256/HMAC | 低 | 4–6h |
| `voice_client.cpp`（VAD/录音/状态机） | UAC 咪头回调 + VAD/状态机 | 中 | 12–16h |
| `pdm_mic.cpp`（INMP441 I2S） | 删除；改用 UAC 咪头 | 低 | 0.5–1h |
| `lighting.cpp`（PWM 调光） | IDF `ledc` | 低 | 2–4h |
| `motion.cpp`（PWM/总线舵机） | `ledc`/`mcpwm`；总线舵机 UART | 中 | 6–10h |
| `timer_task.cpp`（定时任务） | `esp_timer`/FreeRTOS | 低 | 2–4h |
| `ir_engine.cpp` / `ir_store.cpp` | RMT + 自写/换库 | 高 | 10–16h |
| `ui.cpp`（OLED） | `esp_lcd`/I2C/SSD1306 | 中 | 6–8h |
| `voice_uart.cpp`（离线模块 UART） | IDF UART 驱动 | 低 | 2–4h |
| `local_intent.cpp` | 原样复用 | 低 | 1–2h |
| `web/device.html` + `console_html.h` | `EMBED_FILES` 或 gzip 数组 | 低 | 4–6h |
| `platformio.ini` → IDF | `CMakeLists.txt` + `idf_component.yml` + `sdkconfig.defaults` | 低 | 4–6h |

**核心（不含 IR/舵机/OLED）**：约 **60–80 小时**。  
**全功能**：约 **85–120 小时**。  
按 16h/周：核心 **4–5 周**，全功能 **6–8 周**。

---

## 4. 分阶段迁移

### M0：摄像头枚举（先决条件，1–2 天）

- 现在 `GPIO19=0 / GPIO20=0`，摄像头侧没有 D+ 上拉。
- 先量 5V、GND、D+/D−；目标 `GPIO19=0 / GPIO20=1`。
- 通过后才值得做 `lamp_av`；否则先换 2MP Full-Speed UVC 摄像头或保留 INMP441。

### M1：`lamp_av` 骨架（约 1 周）

- 新建 `firmware/idf/lamp_av`；WiFi STA/AP、HTTP/WS 控制台、按键/LED/灯。
- 不带摄像头/语音，先证明“IDF 版灯”能跑。
- 验收：网页能开、`/api/cmd?c=light%2060` 生效、按键/LED 正常。

### M2：UAC 咪头 + 离线命令词（约 1 周）

- `usb_stream` UAC 咪头 → 16k PCM → VAD → esp-sr MultiNet（开灯/关灯/亮一点/暗一点/最亮）→ GPIO5。
- 先不接云；证明“摄像头咪头能控制灯”。
- 验收：说“开灯/关灯” 8/10；TTS 未接，无回声问题。

### M3：云语音 + TTS（1–2 周）

- UAC 咪头 → 云 ASR → 本地词表/LLM → TTS → UAC 喇叭。
- 半双工：TTS 播放时静音咪头；按键触发优先。
- 验收：3/3 问答；TTS 可听清；播放后不自触发。

### M4：UVC 视频 + 网页（约 1 周）

- MJPEG HTTP 流；网页显示摄像头 + ASR/LLM 文本。
- 验收：浏览器看到流；10 分钟不崩。

### M5：全功能对齐（1–2 周）

- IR、PWM/总线舵机、OLED、`voice_uart`、定时任务。
- 验收：与 Arduino 版功能对齐；不要求一次全上。

### M6：稳定性（约 1 周）

- 2h/8h 连续运行；WiFi 重连；回声/AEC；内存/CPU。
- 验收：8/10、2h 不重启、页面不断线。

---

## 5. CAM12 麦克风控制灯：具体路径

### 5.1 数据链

```text
CAM12 UAC 咪头
  → usb_stream mic_frame_cb（PCM）
  → 16 kHz 单声道（若 48k 则降采样）
  → VAD（能量/帧数）
  → ASR：
       A. 离线：esp-sr MultiNet 5 条命令词
       B. 云端：腾讯 ASR → 本地词表/LLM
  → 意图：on / off / up / down / max
  → lighting::setBrightness() / GPIO5
```

### 5.2 两种控制方式

| 方式 | 依赖 | 延迟 | 适合 |
|---|---|---|---|
| **A. 离线命令词** | esp-sr，无需网络/Key | <500 ms | 先证明“咪头 → 灯” |
| **B. 云 ASR + LLM** | 网络 + 腾讯 Key | 1–3 s | 自然语言、问答 |

建议：**先做 A，再做 B**。A 通了就说明摄像头咪头、VAD、GPIO5 整条链路没问题。

### 5.3 采样率/格式

- `usb_stream` 用 `UAC_FREQUENCY_ANY` 枚举摄像头支持的采样率；`mic_frame_t.samples_frequence` 会打印出来。
- 常见是 16 kHz 或 48 kHz；esp-sr/云 ASR 要 16 kHz 单声道 16 bit。
- 48 kHz → 3:1 降采样；多声道 → 取 1 声道；24/32 bit → 转 16 bit。
- 这些在 UAC 回调里做，不阻塞回调（拷贝到环形缓冲，另开任务处理）。

### 5.4 回声（必须处理）

- 咪头和喇叭在**同一个 CAM12 模块**上；TTS 一播放，咪头就听到。
- **先半双工**：按键触发录音；TTS 播放时静音咪头；播完再开。
- 后做 AEC：esp-sr AFE 的 AEC 需要参考通道，把 TTS PCM 作为参考，或软件 AEC。
- 验收：TTS 播放后 5 秒内不出现第二次 `[ASR]`。

### 5.5 验收

| # | 项 | 目标 |
|---|---|---|
| 1 | 咪头有数据 | `mic peak` 随说话变化 |
| 2 | 离线命令词 | 说“开灯/关灯” 8/10 |
| 3 | 灯响应 | GPIO5 变化，网页同步 |
| 4 | 云语音 | 3/3 问答 |
| 5 | TTS | 喇叭可听清 |
| 6 | 回声 | 播放后不自触发 |

---

## 6. 风险与回退

| 风险 | 影响 | 处理 |
|---|---|---|
| 摄像头不枚举（HS-only/3.0V/线序） | M2–M4 全卡住 | 先 M0；不行换 2MP FS UVC 摄像头 |
| USB Host + WiFi + TLS 内存/CPU | 不稳定/重启 | PSRAM 8MB；任务优先级/栈；限帧 |
| UVC + UAC 并发 | 带宽/丢帧 | 480×320/800×480；MJPEG；限帧 |
| 回声/AEC | 自触发 | 半双工；AEC 后做 |
| 云 Key/费用/延迟 | 不可用 | 离线命令词优先；Key 配置；超时降级 |
| IR 库是 Arduino 专用 | 迁移最费事 | 先不做 IR；或 RMT 重写 |
| 总线舵机协议 | 迁移复杂 | 先 PWM 舵机；总线协议另做 |
| IDF 移植回归 | 功能不一致 | 保留 Arduino 固件作对照/回退 |
| 一次性大改 | 难定位 | 按 M1–M6 增量 |

**回退**：摄像头始终不枚举 → 保留 Arduino 灯固件，摄像头接电脑/第二块 ESP32-S3；或换 2MP FS UVC 摄像头。

---

## 7. 建议

1. **先修摄像头枚举**（M0），这是所有后续工作的前提。
2. 摄像头通过后，**先做 M1 骨架**，不要一上来就搬云语音。
3. **M2 用离线命令词**证明“CAM12 咪头 → 灯”；这一步不依赖云，最快看到效果。
4. **M3 再接云 ASR/LLM/TTS**；先半双工，AEC 后做。
5. **IR/舵机/OLED 放 M5**；不要为了“全搬”阻塞核心。
6. 保留 Arduino 固件作为对照和回退；迁移用独立分支/工程 `lamp_av`。

---

## 8. 参考

| 内容 | 位置 |
|---|---|
| 两条固件线 | [docs/23](23-技术栈清单.md#L119-L142) |
| 网页控制台/WS/TTS | [docs/32](32-WiFi语音与灯控方案.md#L151-L156) |
| 上板 WiFi/ASR | [docs/35](35-上板验证与合并记录.md#L56-L76) |
| 摄像头+语音目标 | [docs/38](38-摄像头语音一体化改造方案.md) |
| 两阶段计划 | [docs/39](39-第一阶段与第二阶段实施计划.md) |
| P0 实测/补丁 | [docs/40](40-第二阶段连接路线与P0实施.md) |
| 现有云语音逻辑 | [cloud_asr.cpp](../firmware/src/cloud_asr.cpp)、[tc3_sign.cpp](../firmware/src/tc3_sign.cpp) |
| 现有网页/WS | [net_console.cpp](../firmware/src/net_console.cpp) |
| 现有 WiFi/配网 | [wifi_link.cpp](../firmware/src/wifi_link.cpp) |
| IDF 离线命令词底座 | [lamp_sr/main.c](../firmware/idf/lamp_sr/main/main.c) |
| IDF WiFi/VAD 参考 | [lamp_voice.c](../firmware/idf/lamp_voice/main/lamp_voice.c) |
| 引脚 | [config.h](../firmware/include/config.h#L50-L78) |
