# LAMP—1 · AI 机器人交互台灯

> 一盏会听、会答、会动的桌面台灯。<br>ESP32-S3 做身体，云端大模型做大脑。

[官网](https://zhouxiaoshan499.github.io/lamp/) · [语音仿真](https://zhouxiaoshan499.github.io/lamp/simulator.html?mode=voice) · [项目总结](docs/09-智能交互台灯项目总结.md) · [语音产品化方案](docs/07-语音产品化方案.md)

![LAMP—1 概念渲染](web/assets/lamp-wide.webp)

*概念渲染，非量产。*

## 项目简介

LAMP—1 是一盏把 AI 从屏幕里带到桌面上的台灯。它保留台灯的基础照明属性，同时用麦克风、双轴舵机和双色温 LED，让一次对话变成声音、光和动作。

它不把电脑塞进灯里，而是采用 **「ESP32-S3 做身体 + 云端大模型做大脑」** 的低成本架构：

- **身体**：ESP32-S3 负责拾音、灯光、舵机、按键、屏幕和本地定时；
- **大脑**：负责语音识别、对话生成和语音合成（TTS 为下一阶段）；
- **交互**：按键说话 → ASR/LLM 理解 → 语音、灯光和动作回应；开发期在电脑上跑，产品目标在云端。

当前项目处于 **MVP / 概念样机阶段**，重点是先跑通语音闭环，再验证「光 + 动作」的联动，最后迁入 RIG-Arm（ESP32-S3 + 总线舵机 + 圆屏 + 麦克风/喇叭套件）做整机整合。

> 一句话策略：**先能力、后硬件；先闭环、后场景；先验证、后承诺。**

## 目标用户与场景

| 用户 | 典型场景 |
|------|----------|
| 大学生 / 年轻学习者 | 学习计时、专注陪伴、桌面问答 |
| 居家办公人群 | 语音问答、桌面照明、任务提醒 |
| AI 硬件爱好者 | 可拆解、可扩展的桌面机器人 |

产品落地场景以 **学习 / 办公桌面** 为主；技术底座是一套与具体硬件解耦的 AI 交互能力。学习计时、任务显示和状态反馈是后续要验证的叠加能力，不是当前已完成的功能。

## 核心能力

1. **AI 语音交互**：按下对话键说话，设备拾音并交给 ASR / LLM，回答以文本返回；语音播报（TTS）为下一阶段。v1 用按键触发，避免误唤醒和常驻监听。

2. **机械动作反馈**：双轴云台让灯头转向、俯仰、点头。目标是把「回答结束」「进入休息」「完成任务」变成可感知的实体动作。

3. **智能照明**：双色温混光、场景预设、亮度和色温调节（双色温灯板为整机目标）；灯光同时承担交互状态反馈。

4. **学习 / 办公辅助**：当前已有倒计时；学习计时、任务提醒和状态显示是后续扩展。先验证「灯光 + 屏幕 + 动作」三通道联动是否真的比普通台灯 + 手机番茄钟更有价值。

5. **网页数字样机**：浏览器里可调光、转灯头、设倒计时；新增 **语音仿真台**，用脚本模拟「说话 → 回答 → 灯光/动作联动」，方便演示和 UX 验证。

## 当前状态

| 模块 | 状态 | 说明 |
|------|------|------|
| 语音闭环 | ✅ 已跑通 | 开发板 + INMP441 + PC 端 Vosk/Ollama，文本回复 |
| 照明 / 按键 | ✅ 已跑通 | 单通道调光、场景、按键、串口 CLI；双色温灯板待整机接入 |
| 网页数字样机 | ✅ 已上线 | 光控 + 语音仿真（脚本，不接真实模型） |
| 机械动作 | 🟡 代码就绪 | 舵机 PWM 代码已有，硬件未接；目标 RIG-Arm 总线舵机 |
| 屏幕 / 扬声器 | ⬜ 未接入 | 1.09″ 圆屏 + 2W 喇叭为下一阶段 |
| 云端 qwen | ⬜ 待接入 | 方案见 `docs/07`；当前是本地开发链路 |
| 红外家电控制 | 🟡 后备方向 | 代码保留，未上硬件；不作为当前主线 |

当前样机：**ESP32-S3-DEVKITC-1（N16R8）+ INMP441 + HW-479 + HW-483**。舵机、屏幕、喇叭尚未接入。

## 系统架构

```text
用户按下对话键说话
        │
        ▼
INMP441 麦克风 ──▶ ESP32-S3 ── Wi-Fi / WebSocket ──▶ ASR（Vosk / 云端）
                    │                                   │
                    │                                   ▼
                    │                              LLM（Ollama / qwen）
                    │                                   │
                    ▼                                   ▼
        双色温 LED / 双轴舵机 / OLED / 扬声器 ◀── 回复文本 / TTS
```

- **开发期链路**：ESP32 → PC `voice_server.py` → Vosk → Ollama → 文本回复；
- **产品目标**：设备主动外连云服务，云端完成 ASR / LLM / TTS，设备只负责身体；
- **本地保底**：断网时，灯光、按键和倒计时仍然可用（动作待硬件接入）；
- **隐私边界**：v1 设计为只有按下对话键才上传语音，不做常驻监听。

## 仓库结构

```text
lamp/
├── README.md                     本文件
├── docs/                         方案、验证与项目记录
│   ├── 01-商业计划书.md
│   ├── 02-市场调研与竞品分析.md
│   ├── 03-技术方案.md
│   ├── 04-MVP验证计划.md
│   ├── 05-风险与里程碑.md
│   ├── 06-面包板点亮验证.md
│   ├── 06-语音AI链路实施手册.md
│   ├── 07-语音产品化方案.md
│   ├── 08-对标AutonomousLamp模型接入方案.md
│   └── 09-智能交互台灯项目总结.md
├── firmware/                     ESP32-S3 固件
│   ├── src/                      Arduino / PlatformIO 主固件
│   │   ├── main.cpp
│   │   ├── voice_client.cpp
│   │   ├── pdm_mic.cpp           INMP441 I2S 采集
│   │   ├── lighting.cpp
│   │   ├── motion.cpp
│   │   ├── timer_task.cpp
│   │   ├── ui.cpp
│   │   └── ir_engine.cpp / ir_store.cpp   红外后备代码
│   ├── src/bringup/              按键 → 灯的最小验证
│   ├── idf/lamp_voice/           ESP-IDF 语音客户端
│   ├── idf/pdm_probe/            ESP-IDF 麦克风探针
│   ├── micropython/              开发板 bring-up 脚本
│   └── include/                  配置、引脚与 secrets 模板
├── server/                       语音服务（开发链路）
│   ├── voice_server.py           WS 8765：PCM → Vosk → Ollama
│   ├── lamp_server.py            TCP 8766 + HTTP 8080（ESP-IDF 版配套）
│   ├── ask.py                    终端直接问模型
│   ├── asr_test.py               用 wav 单独验证 ASR
│   ├── test_client.py            不用开发板验证完整链路
│   └── models/                   Vosk 模型，不入库
├── web/                          LAMP—1 官网与数字样机（纯静态）
│   ├── index.html
│   ├── product.html
│   ├── simulator.html            光控 + 语音仿真
│   ├── technology.html
│   └── assets/
├── tools/
│   └── mvp_calc.py               BOM / 功耗 / 热 / 经济性计算器
└── .github/workflows/pages.yml   GitHub Pages 自动部署
```

## 快速开始

### 1. 官网与数字样机

线上地址：

- 官网：<https://zhouxiaoshan499.github.io/lamp/>
- 语音仿真：<https://zhouxiaoshan499.github.io/lamp/simulator.html?mode=voice>

本地直接打开，无需构建：

```powershell
start web\index.html
```

语音仿真台是**前端脚本模拟**：不录音、不上传、不调用真实大模型。它演示的是「一句话如何变成灯光、计时和动作」，不是实际语音链路。

### 2. 编译固件

需要 PlatformIO + ESP32-S3 平台。

```powershell
cd firmware

# 面包板最小验证：按键 → 灯
pio run -e bringup -t upload
pio device monitor -b 115200

# 完整固件：语音 + 照明 + 舵机 + 定时
pio run -e lamp_s3 -t upload
```

注意：

- 当前主控为 ESP32-S3 N16R8（16 MB Flash / 8 MB PSRAM）；
- INMP441 必须使用 32-bit 槽宽；
- ESP32-S3 只有 LEDC 通道 0–7，主固件已改用 0–4；
- 面包板只接按键/灯时先跑 `bringup`，不要直接烧完整固件。

ESP-IDF 版本在 `firmware/idf/lamp_voice`，可用 `idf.py build` 单独构建；它不是当前主路径。

### 3. 启动语音服务（开发链路）

需要 Python 3.10+、Ollama，以及本地 Vosk 模型。

```powershell
cd server
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt

# 下载 Vosk 中文小模型到 server/models/
New-Item -ItemType Directory -Force models | Out-Null
Invoke-WebRequest https://alphacephei.com/vosk/models/vosk-model-small-cn-0.22.zip -OutFile models\vosk-model-small-cn-0.22.zip
Expand-Archive models\vosk-model-small-cn-0.22.zip models

# 大脑：默认 qwen3:14b，机器吃紧可换更小模型
ollama pull qwen3:14b

# 先自检 ASR + LLM
python voice_server.py --check

# 启动服务
python voice_server.py
```

另开一个终端，不用开发板也能验证整条链路：

```powershell
python test_client.py
python test_client.py --text "今天适合复习吗"
```

服务默认监听 `0.0.0.0:8765`。`lamp_server.py` 是 ESP-IDF 版的 TCP/HTTP 服务，端口 8766/8080，和 `voice_server.py` 不冲突。

### 4. 让 ESP32 连上来

1. 复制配置模板：

   ```powershell
   copy firmware\include\secrets.h.example firmware\include\secrets.h
   ```

2. 填入 Wi-Fi 名称、密码和运行 `voice_server.py` 的电脑 IP；
3. 保证 ESP32 和电脑在同一网段，放行 8765 端口；
4. 烧录后串口执行：

   ```text
   mic test 10
   voice status
   voice say 今天适合复习吗
   ```

更完整的接线、串口输出和排障见 `docs/06-语音AI链路实施手册.md`。

## 文档导航

| 文档 | 作用 |
|------|------|
| [09-智能交互台灯项目总结.md](docs/09-智能交互台灯项目总结.md) | **当前主文档**：定位、硬件演进、阶段目标、红外后备说明 |
| [07-语音产品化方案.md](docs/07-语音产品化方案.md) | 语音产品化：云端 ASR/LLM/TTS、按键触发、成本与延迟 |
| [08-对标AutonomousLamp模型接入方案.md](docs/08-对标AutonomousLamp模型接入方案.md) | 模型接入与 tool calling 迁移 |
| [06-语音AI链路实施手册.md](docs/06-语音AI链路实施手册.md) | 麦克风、服务端、串口联调步骤 |
| [06-面包板点亮验证.md](docs/06-面包板点亮验证.md) | 按键 → 灯的最小闭环 |
| [03-技术方案.md](docs/03-技术方案.md) | 硬件、引脚、照明、机械、定时（部分为早期红外优先口径） |
| [01-商业计划书.md](docs/01-商业计划书.md) | 早期商业与财务测算（历史口径） |
| [02-市场调研与竞品分析.md](docs/02-市场调研与竞品分析.md) | 市场与竞品（早期红外主线视角） |
| [04-MVP验证计划.md](docs/04-MVP验证计划.md) | 用户验证与指标阈值 |
| [05-风险与里程碑.md](docs/05-风险与里程碑.md) | 风险登记册与阶段门 |

> `docs/01–05` 保留早期「红外优先」的商业/技术口径，部分结论已被 `docs/07–09` 和当前实现取代。阅读时以 `docs/09` 为主。

## 边界与风险

- 项目处于 **概念 / MVP 阶段**，不构成量产承诺；
- 当前语音链路依赖电脑 + Ollama，**尚未做到「不插电脑」**；云端 qwen 是下一步；
- 舵机、屏幕、喇叭尚未接入，机械动作与整机形态未验证；
- 红外是后备方向：代码保留、硬件未接、未做真实机型兼容性测试；
- 市场规模、用户付费意愿、学习场景需求均为待验证假设；
- 未做 3C/CE、EMC、热、跌落和寿命测试；
- 部分历史文档/代码注释仍保留 V829/PDM 或红外优先表述，以 `docs/07–09` 和实际接线为准。

## 路线图

| 阶段 | 目标 | 状态 |
|------|------|------|
| 1 | 语音闭环：拾音 → ASR → LLM → 回复 | ✅ 已跑通 |
| 2 | 上云 + 配网 + tool calling，做到不依赖电脑 | 🟡 下一步 |
| 3 | 迁入 RIG-Arm：舵机、圆屏、喇叭、整机结构 | ⬜ 待开始 |
| 4 | 学习场景验证：计时、任务、状态反馈、用户测试 | ⬜ 待验证 |

现阶段最重要的三件事：

1. 把语音闭环做到「拔掉电脑也能用」；
2. 干净地迁入 RIG-Arm，打通舵机与屏幕；
3. 用真实用户验证「灯光 + 屏幕 + 动作」三通道联动是否值得做。

---

项目核心不是重新发明一款 AI 台灯，而是用可控的成本把「会回应的台灯」做出来。
