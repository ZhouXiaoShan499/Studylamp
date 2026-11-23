# lamp_av

**唯一的 ESP-IDF 灯固件**（[docs/42](../../../docs/42-固定ESP-IDF实施方案.md) 的固定工程，
[docs/43](../../../docs/43-全量迁移与CAM12咪头开关灯可执行方案.md) 的 M1/M2 落地）。

Arduino `firmware/src` 与 `firmware/idf/lamp_sr` 都保留，但**不再作为目标固件**：
前者是全功能对照与回退镜像，后者是最简离线命令词回滚镜像。

## 现在能做什么（M1 + M2）

| 能力 | 状态 | 说明 |
|---|---|---|
| 双通道 LEDC 调光 | ✅ | Gamma 2.2 + 2700–6500 K 色温表 + 600 ms 斜坡，算法与 Arduino 一致 |
| 按键 | ✅ | 短按开关（灭了回 60 %），长按预设循环 Study→Office→Rest→Night |
| 上电即亮 | ✅ | 60 % @ 4300 K，与 Arduino 行为一致 |
| 离线命令词 | ✅ | esp-sr 2.6.1 AFE + MultiNet7（中文），5 条命令 |
| 音频源抽象 | ✅ | `audio_in.h`：16 kHz / 单声道 / int16，I2S 与 UAC 二选一 |
| INMP441 音频源 | ✅ | I2S，GPIO16/18/17，+18 dB 前置增益 |
| CAM12 UAC 咪头 | ⛔ M2b | `uac_mic.c` 是"诚实失败"的空壳，需先 vendored `usb_stream` |
| WiFi / 网页 / 云语音 | ⛔ M3/M4 | 见 docs/43 §7 |
| 喇叭 / TTS / 摄像头 | ⛔ M5/M6 | 见 docs/43 §7 |

语音 → 灯的语义照搬 Arduino `main.cpp:265-310`：开灯回到关灯前亮度（无记录时 70）、
关灯记住当前亮度、亮一点 +20、暗一点 −20（下限 5）、最亮 100。

## 构建 / 烧录

必须用本目录的 `idf.bat`（它把 ESP-IDF 5.4.3 与 `idf5.4_py3.11_env` 钉死，
系统里的 `idf.py` 可能是别的版本）：

```powershell
cd firmware\idf\lamp_av

.\idf.bat set-target esp32s3     # 首次
.\idf.bat build
.\idf.bat -p COM5 flash          # 只走 COM5（CH343 UART）
```

看日志**不要**用 `idf.py monitor`（这台机器上会因 GBK 编码崩），用 miniterm：

```powershell
D:\Espressif\python_env\idf5.4_py3.11_env\Scripts\python.exe -m serial.tools.miniterm COM5 115200
```

`managed_components/`（esp-sr 等）不进仓库，首次 `build` 由组件管理器自动拉取；
离线机器可以从 `../lamp_sr/managed_components` 复制过来。

## 接线（面包板）

| 设备 | 接到 | 备注 |
|---|---|---|
| HW-479 灯 | G → **GPIO5**；R/B 悬空 | 面包板只有冷通道；暖通道 GPIO21 未接 |
| HW-483 按键 | S → **GPIO4**，+ → 3V3，− → GND | 上拉版；接反了改 Kconfig 的「按键低有效」 |
| INMP441 | VDD→3V3、GND→GND、**L/R→GND**、SCK→GPIO16、WS→GPIO18、SD→GPIO17 | L/R 不接 GND 会全 0，这是最常见的"麦克风没信号" |
| CAM12 | 红→5V、绿→GND、黄→GPIO19、黑→GPIO20 | USB Host；**不要同时把电脑插到 USB 口** |
| 烧录/日志 | **COM 口**（UART Type-C）→ COM5 | GPIO19/20 让给摄像头 |

## 验证清单（M1/M2 门禁）

1. 上电日志出现 `lamp_av M1+M2`、`source i2s ready`、`MultiNet model: ...`；
2. 灯上电即亮 60 %，串口出现 `>>> boot`；
3. 短按一次灭、再按一次亮 60 %（日志 `>>> key:short`）；
4. 长按 1.2 s → 预设前进（日志 `>>> key:long` + `preset=office`）；
5. 每 2 秒一行 `mic peak=... rms=...`，说话时明显变大；
6. 说「开灯 / 关灯」，日志出现 `MN id=1 prob=...`，灯跟着变。

第 6 项按 docs/43 §7 的 M2 门禁是 **≥ 6/10**（8/10 是 M7 的目标；
`lamp_sr` 的现状是 3/3、prob 0.14–0.47，余量本来就薄）。

## 已知偏差

| 项 | 现状 | 计划 |
|---|---|---|
| 音频源只有 I2S | UAC 是空壳，选中它会在启动时报 `not supported` | M2b |
| 夜间守卫 | Arduino 的 22:00–07:00 只放行关灯还没搬 | M3（要等 NTP） |
| 网页状态同步 | 无网页 | M3 |
| OTA | 分区表是单 `factory`，没有 OTA 双槽 | docs/43 §3.3 已认下来 |
