# 会动的桌面台灯（暂定名 LAMP-1）

融合 **红外家电控制 + 智能照明 + 定时提醒 + 机械动作** 的桌面交互台灯。
本仓库当前处于 **概念设计与最小原型验证阶段**，不含量产结论。

## 一句话策略

> 先红外、后学习；先刚需、后情绪；先验证、后扩展。

## 仓库结构

```
lamp/
├── README.md                     本文件：索引与现状
├── docs/
│   ├── 01-商业计划书.md           BP 主文档（含单位经济与三年财务）
│   ├── 02-市场调研与竞品分析.md   TAM/SAM/SOM、竞品矩阵、调研脚本
│   ├── 03-技术方案.md             硬件选型、引脚、红外/照明/机械/定时/语音
│   ├── 04-MVP验证计划.md          三轮验证、指标阈值、测试协议
│   ├── 05-风险与里程碑.md         风险登记册、决策门、预算
│   ├── 06-面包板点亮验证.md       按键(GPIO4)开灯(GPIO5)最小通路与排错
│   └── 06-语音AI链路实施手册.md   任务一/二/三：INMP441 麦克风 → Vosk → DeepSeek
├── firmware/                     ESP32-S3 固件（Arduino/PlatformIO 与 ESP-IDF 两套）
│   ├── src/bringup/              面包板 bring-up：只跑按键->灯，不依赖库
│   ├── micropython/              面包板实测固件（板上当前运行的就是这份）
│   ├── idf/lamp_voice/           ESP-IDF 版语音客户端（WiFi + INMP441 I2S 麦克风 + TCP）
│   ├── idf/pdm_probe/            ESP-IDF 版麦克风探针（dBFS 电平 + 拍手检测，目录名沿用旧称）
│   └── include/secrets.h.example WiFi 与电脑 IP 模板（复制为 secrets.h）
├── server/                       局域网语音服务（Vosk ASR + 本地大模型）
│   ├── voice_server.py           WS 8765：PCM → ASR → DeepSeek → 回复（Arduino 版配套）
│   ├── lamp_server.py            TCP 8766 + 网页 8080：Vosk → LLM（ESP-IDF 版配套）
│   ├── ask.py                    终端直接问 DeepSeek（流式）
│   ├── asr_test.py               用 wav 单独验证 Vosk
│   ├── test_client.py            假 ESP32：不需要开发板即可验证整条链路
│   ├── test_client_voice.py      voice_server.py 的 WebSocket 测试客户端
│   └── models/                   vosk-model-small-cn-0.22（不入库）
├── web/                          LAMP-1 官网（纯静态，浏览器直接打开 index.html）
│   ├── index.html                概览首页
│   ├── product.html              产品叙事与交互细节
│   ├── simulator.html            交互数字样机
│   ├── technology.html           系统架构、规格与验证边界
│   └── assets/                   全站样式、脚本与产品渲染素材
└── tools/
    └── mvp_calc.py               工程/商业计算器（BOM、功耗、热、单台经济、IR 存储自检）
```

## 快速开始

```bash
# 重新生成文档中引用的全部数字（BOM / 功耗 / 热 / 单台经济 / IR 存储格式自检）
python tools/mvp_calc.py all

# 只跑固件 IR 存储格式的自检（9 项断言）
python tools/mvp_calc.py selftest

# 编译固件（需 PlatformIO + ESP32 平台）
cd firmware && pio run -e lamp_s3

# 面包板最小验证：按键 -> 灯（不依赖 IR/舵机/LittleFS）
cd firmware && pio run -e bringup -t upload && pio device monitor -b 115200
```

> 手上只有开发板 + HW-483 按键 + HW-479 RGB 模块时，**先跑 `-e bringup`**。
> `lamp_s3` 虽然也开了面包板引脚（按键在 GPIO4、暖白让位到 GPIO21），
> 但它把 **GPIO5 当冷白通道的 4 kHz PWM** 用，HW-479 会跟着照明亮度走而不是按键；
> 而且依赖 IRremote / ArduinoJson / LittleFS 一整串库，出问题时不好判断卡在哪一层。
> 先把最小闭环跑通，步骤与排错见 `docs/06-面包板点亮验证.md`。

### 官网与数字样机

```powershell
# 无需构建、无需服务器，直接打开静态站点
start web\index.html
```

站点包含 **概览 / 产品 / 数字样机 / 技术** 四个页面，共享导航与响应式布局。
数字样机支持拖动灯头、场景切换、色温/亮度/姿态控制、定时关闭，
并可从首页光配方直接深链进入，例如 `web/simulator.html?scene=night`。

### 语音链路（INMP441 麦克风 → 电脑 ASR → DeepSeek）

```powershell
# 1) 大脑：安装 Ollama 后拉模型
ollama pull qwen3.5:9b

# 2) PC 端依赖 + 自检（ASR + 大模型都 ok 才继续）
cd server
pip install -r requirements.txt
python voice_server.py --check

# 3) 不用开发板先验证整条链路
python voice_server.py            # 终端 A
python test_client.py             # 终端 B：wav → ASR → DeepSeek

# 4) ESP32：填 WiFi/IP 后烧录，串口执行
copy firmware\include\secrets.h.example firmware\include\secrets.h   # 再编辑
mic probe                         # 任务一：先确认麦克风在出数（全 0 = 线没接对）
mic test 10                       #        再看拍手/说话时 RMS 跳动
voice status                      # 任务三：应显示 ws=up
voice say 今天适合复习吗           # 跳过麦克风，直接测大脑
```

麦克风是 **INMP441（标准 I2S 四线）**：SCK→GPIO16、WS→GPIO18、SD→GPIO17、L/R→GND。
完整接线、预期串口输出与排障见 `docs/06-语音AI链路实施手册.md`。

> 仓库里目前有**两套并行的语音实现**：
> **Arduino/PlatformIO 版**（`firmware/src/pdm_mic.cpp` + `firmware/src/voice_client.cpp` + `server/voice_server.py`，已实机验证）；
> **ESP-IDF 版**（`firmware/idf/lamp_voice` + `server/lamp_server.py`，麦克风引脚已同步为 INMP441，编译通过、待上板）。
> 两者共用 `server/models/` 下的同一份 Vosk 模型，端口不冲突（8765 / 8766+8080）。

## 关键数字（由 `tools/mvp_calc.py` 计算，非拍脑袋）

| 项目 | 结果 |
|------|------|
| BOM（500 台批次） | 基础 84.90 / 进阶 96.60 / 旗舰 150.60 元 |
| BOM（5000 台批次） | 基础 69.62 / 进阶 79.21 / 旗舰 123.49 元 |
| 单台贡献毛利（5000 台批次） | 基础 30.99（20.8%）/ 进阶 65.90（33.1%）/ 旗舰 110.62（37.0%）元 |
| 5V 电源裕量 | 基础/进阶 55.0%，旗舰 58.8%（固件互锁后） |
| LED 结温（5W、30℃ 环境） | 52.8℃，余量 32.2℃ |
| 混合结构盈亏平衡 | 364 台/月（月固定成本 1.8 万元） |
| IR 存储格式 | 9/9 断言通过（含 CRC 校验、截断拒绝、越界拒绝） |

## 重要结论（与直觉相反）

1. **基础版在 500 台试点批次下只有 10.5% 贡献毛利**，不能作为主力 SKU 起量。
   首销应以 **进阶版（Plus）为主力**，基础版仅用于众筹低价引流或校园批量议价。
2. **旗舰版的舵机必须做固件互锁**：两个舵机同时堵转叠加 LED 满载会把 5V/4A 电源压到边界。
   `motion.cpp` 中的互斥 + 运动时降亮度是电源预算的组成部分，不是"锦上添花"。
3. **红外数据必须存 LittleFS 而不是 NVS blob**：空调整帧 602 条原始码约 1.2KB，
   多个槽位会撑爆 NVS 分区；详见 `mvp_calc.py selftest` 的第 2、7 项。

## 现状与边界（务必阅读）

- 本文档中所有市场规模、用户偏好、付费意愿均为 **待验证假设**，不是调研结论。
- 固件为 **参考骨架**：引脚号需按实际 PCB 修改；舵机与 LED 驱动电路未做 EMC/热实测。
- 红外兼容性只在协议层设计，**未对真实空调机型做覆盖测试**，这是第一轮验证的核心内容。
- 未做 3C/CE 认证，未做结构跌落与寿命测试。

## 文档阅读顺序

先读 `docs/01-商业计划书.md` 的执行摘要与"验证优先"章节，
再读 `docs/04-MVP验证计划.md` 了解如何用最小成本证伪，
最后读 `docs/03-技术方案.md` 安排具体实施。