# 42 固定 ESP-IDF 实施方案

> **文档性质**：工程/运维方案。**本次不改代码**；后续按此固定到 ESP-IDF。
>
> **决策**：从今天起，板子上**只维护、只烧录 ESP-IDF 的 `lamp_av`**；Arduino `firmware/src` 归档为参考，不再作为在跑固件；`lamp_sr` 作为离线/回滚镜像。
>
> **关联**：[38 目标](38-摄像头语音一体化改造方案.md)、[39 两阶段](39-第一阶段与第二阶段实施计划.md)、[40 P0 实测](40-第二阶段连接路线与P0实施.md)、[41 迁移方案](41-Arduino固件迁移IDF与CAM12语音控制方案.md)。

---

## 0. 结论

| 问题 | 结论 |
|---|---|
| 固定什么框架 | **ESP-IDF 5.4.3** |
| 固定哪个工程 | **`firmware/idf/lamp_av`**（唯一在跑） |
| Arduino 怎么办 | `firmware/src` 归档参考，不再烧录 |
| P0 怎么办 | 只作摄像头探测/回滚，不作为产品固件 |
| 烧录口 | **COM5（CH343 UART）**，115200 |
| 摄像头口 | 板载 **USB**（GPIO19/20），ESP32-S3 当 Host |
| 控制台 | **UART0**；USB-Serial-JTAG/CDC **关闭** |
| 分区 | app 3 MB + model 6 MB + NVS/PHY（16 MB flash） |
| 现有卡点 | 摄像头 `GPIO19=0/GPIO20=0`；M2/M4 等它 |

> 一句话：**一个框架（IDF）、一个工程（`lamp_av`）、一个烧录口（COM5）、一个分区表。**

---

## 1. 为什么要固定

现在板子上来回换：

```text
Arduino lamp_s3_tencent（WiFi/网页/云语音，无摄像头）
IDF lamp_sr（离线命令词，无 WiFi/摄像头）
IDF P0（USB Host + WiFi AP，无灯业务）
```

每次换都覆盖前一个，功能互相“消失”。固定 IDF 的理由：

1. **USB Host + UVC + UAC 只有 IDF 有**；Arduino-ESP32 2.0.17 没有 `usb_stream`。
2. 灯的 WiFi/网页/云语音可以移植到 IDF；反过来把 USB Host 塞进 Arduino 更难。
3. 只有一张镜像，才谈得上“按键、灯、WiFi、网页、云语音、摄像头”同时在线。

---

## 2. 固定后的仓库结构

```text
firmware/
├── idf/
│   ├── lamp_av/                 # ★ 唯一在跑：IDF 5.4.3
│   │   ├── CMakeLists.txt
│   │   ├── idf.bat              # 固定 IDF 路径/Python venv
│   │   ├── sdkconfig.defaults   # PSRAM octal / UART0 / USJ off / 16MB
│   │   ├── partitions.csv       # app 3MB + model 6MB
│   │   ├── components/
│   │   │   └── usb_stream/      # vendored 1.4.0 + IDF 5.4.3 补丁
│   │   └── main/
│   │       ├── app_main.c
│   │       ├── wifi_link.c
│   │       ├── web_console.c
│   │       ├── lighting.c
│   │       ├── button.c
│   │       ├── usb_camera.c     # UVC + UAC
│   │       └── voice.c          # VAD + ASR/LLM/TTS
│   ├── lamp_sr/                 # 离线命令词/回滚，不主动烧
│   └── ...
├── src/                         # Arduino，归档参考，不再烧
└── platformio.ini               # 只作历史参考
```

**原则**：`lamp_av` 不依赖 `D:\esp-iot-solution` 这个外部克隆；`usb_stream` 复制进 `components/` 并打补丁，保证换机器也能编。

---

## 3. 工具链与版本（钉死）

| 项 | 值 |
|---|---|
| ESP-IDF | **5.4.3**（`D:\Espressif\frameworks\esp-idf-v5.4.3`） |
| Python venv | `D:\Espressif\python_env\idf5.4_py3.11_env` |
| 目标 | `esp32s3` |
| PSRAM | **Octal**（N16R8） |
| Flash | 16 MB |
| esp-sr | **2.6.1**（离线命令词要用） |
| `usb_stream` | **1.4.0 + 4 处 IDF 5.4.3 补丁**（见 §4） |
| 控制台 | UART0 @115200 |
| USB CDC/JTAG | **关闭**（`CONFIG_USJ_ENABLE_USB_SERIAL_JTAG=n`、`CONFIG_ESP_CONSOLE_SECONDARY_NONE=y`） |

`usb_stream` 1.4.0 的 4 处补丁（P0 已验证）：

1. 删除 `fifo_bias` 相关 6 处（IDF 5.4.3 的 `hcd_port_config_t` 没有该字段）；
2. `hcd_config_t` 增加 `.peripheral_map = 1 << USB_PORT_NUM`；
3. `USB_PORT_NUM` 由 1 改为 **0**（S3 只有 1 个 OTG 口）；
4. 编译配置关闭 USB-Serial-JTAG/CDC（否则 HCD 装不上）。

---

## 4. 固定烧录流程

```powershell
cd D:\study lamp\lamp\lamp\firmware\idf\lamp_av

# 首次/换目标
..\lamp_sr\idf.bat set-target esp32s3

# 编译
..\lamp_sr\idf.bat build

# 烧录（COM5 = CH343 UART）
..\lamp_sr\idf.bat -p COM5 flash

# 看日志（避免 idf.py monitor 的 GBK 崩溃）
D:\Espressif\python_env\idf5.4_py3.11_env\Scripts\python.exe -m serial.tools.miniterm COM5 115200
```

### 4.1 烧录规则

- **只走 COM5**；板载 USB 口让给摄像头，不接电脑。
- `flash` 默认**不清 NVS**，WiFi/Key 保留；只有换分区表/彻底重置才 `erase-flash`。
- 摄像头在 USB 口时，不要用原生 USB 烧录/看日志。
- `idf.py monitor` 在这台机器会因 GBK 编码崩；用 miniterm。
- 每次发版前 `idf.py size` 看 app 余量；3 MB 分区留余量。

### 4.2 分区表

`partitions.csv` 与 `lamp_sr` 对齐：

```csv
# Name,   Type, SubType, Offset,   Size
nvs,      data, nvs,     0x9000,   0x6000
phy_init, data, phy,     0xf000,   0x1000
factory,  app,  factory, 0x10000,  0x300000
model,    data, 0x82,    0x310000, 0x600000
```

- `factory` 3 MB：WiFi + 网页 + 云 + UVC/UAC；
- `model` 6 MB：esp-sr 模型（离线命令词）；
- 16 MB flash 有余量。

---

## 5. 版本、回滚与恢复

### 5.1 Git

- 分支：`feat/lamp-av`；
- 标签：`av-m1`、`av-m2`、`av-m3`、`av-m4`；
- 每次能烧、能跑的节点打 tag，附 `sdkconfig.defaults`/`dependencies.lock`。

### 5.2 回滚镜像

| 镜像 | 用途 | 烧录 |
|---|---|---|
| `lamp_av` | 唯一目标 | `idf.bat -p COM5 flash` |
| `lamp_sr` | 离线命令词/最简回滚 | `cd firmware/idf/lamp_sr; idf.bat -p COM5 flash` |
| Arduino `lamp_s3_tencent` | WiFi/网页/云语音回滚 | `cd firmware; pio run -e lamp_s3_tencent -t upload --upload-port COM5` |
| P0 `usb_camera_mic_spk` | 摄像头探测 | `cd D:\esp-iot-solution\...; idf.bat -p COM5 flash` |

> 回滚镜像只用于救砖/对照，不作为日常固件。

### 5.3 NVS

- WiFi 凭据、云 Key 在 NVS；
- `idf.py flash` 保留 NVS；`erase-flash` 会清空；
- 清空前先记录 WiFi/Key，或重新配网。

---

## 6. 固定后的分阶段

| 阶段 | 内容 | 依赖 | 时间 |
|---|---|---|---|
| **M0** | 摄像头枚举 `GPIO20=1` | 5V/线序 | 现在–1 天 |
| **M1** | `lamp_av` 骨架：WiFi + 网页 + 按键/LED | 无 | 约 1 周 |
| **M2** | UAC 咪头 + 离线命令词 → 灯 | M0 | 约 1 周 |
| **M3** | 云 ASR/LLM/TTS，半双工 | M2/Key | 1–2 周 |
| **M4** | UVC 视频 + 网页流 | M0 | 约 1 周 |
| **M5** | IR/舵机/OLED/定时对齐 | — | 1–2 周 |
| **M6** | 稳定性/回声/内存 | — | 约 1 周 |

**固定门禁**：

- M1 完成 → 日常只烧 `lamp_av`，P0/Arduino 不再主动烧；
- M2 完成 → UAC 咪头替代 INMP441；
- M4 完成 → 摄像头流进网页；
- M6 完成 → 打 `av-1.0` tag。

---

## 7. 硬件固定

| 项 | 固定值 |
|---|---|
| 主控 | YD-ESP32-S3 N16R8 |
| 供电 | COM 口 5V/2A；摄像头 5V 从 5V 排针/有源 Hub |
| 按键 | GPIO4（低有效，内部上拉） |
| 灯 | GPIO5（HW-479 G，调试负载） |
| 摄像头 | CAM12 UVC + UAC；红 5V、绿 GND、黄 D−→GPIO19、黑 D+→GPIO20 |
| 咪头/喇叭 | CAM12 白座；UAC |
| INMP441 | **UAC 验证通过后移除** |
| 控制台 | UART0/COM5；USB CDC/JTAG 关闭 |
| 烧录 | 只走 COM5，USB 口给摄像头 |

---

## 8. 风险

| 风险 | 影响 | 处理 |
|---|---|---|
| 摄像头一直 `0/0` | M2/M4 卡住 | 先 PC 测试；5V/线序；换 2MP FS UVC |
| `usb_stream` 补丁丢失 | 换机器编不过 | vendored 组件 + 补丁说明 + tag |
| USB Host + WiFi + TLS 内存/CPU | 重启/不稳 | PSRAM、任务栈、限帧、`heap_caps` |
| 回声/AEC | 自触发 | 半双工优先；AEC 后做 |
| 云 Key/费用 | 不可用 | 离线命令词优先；Key 配置 |
| 分区/NVS 变更 | 配置丢失 | 记录 WiFi/Key；`flash` 不清 NVS |
| 单镜像回滚 | 救砖 | 保留 `lamp_sr`/Arduino/P0 镜像与命令 |

---

## 9. 立即执行清单

- [ ] 定下“固定 IDF”决策，建 `feat/lamp-av` 分支；
- [ ] 把 `usb_stream` 1.4.0 + 4 处补丁 vendored 到 `firmware/idf/lamp_av/components/usb_stream`；
- [ ] 建 `lamp_av` M1 骨架（WiFi + 网页 + 按键/LED）；
- [ ] 编译烧录 M1，确认 `http://192.168.31.91/` 是 `lamp_av` 页面；
- [ ] 摄像头 PC 测试/量 5V，目标 `GPIO19=0/GPIO20=1`；
- [ ] M2 UAC 咪头 + 离线命令词；
- [ ] M3 云语音；M4 视频；M6 打 `av-1.0`。

---

## 10. 参考

| 内容 | 位置 |
|---|---|
| 目标架构 | [38](38-摄像头语音一体化改造方案.md) |
| 两阶段 | [39](39-第一阶段与第二阶段实施计划.md) |
| P0 实测/补丁/WiFi | [40](40-第二阶段连接路线与P0实施.md) |
| Arduino 迁移映射 | [41](41-Arduino固件迁移IDF与CAM12语音控制方案.md) |
| 两条固件线 | [23](23-技术栈清单.md#L119-L142) |
| 网页/WS | [32](32-WiFi语音与灯控方案.md#L151-L156) |
| 引脚 | [config.h](../firmware/include/config.h#L50-L78) |
