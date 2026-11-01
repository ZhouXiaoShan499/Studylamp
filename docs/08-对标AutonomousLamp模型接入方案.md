# 08 - 对标 Autonomous Lamp 的模型接入方案

> 参考对象：https://www.autonomous.ai/lamp （Autonomous Lamp，$999）
> 目的：把它的「模型接入」思路移植到本项目（YD-ESP32-S3-N16R8 + 双通道 LED + 双舵机 + 红外）

---

## 0. 先说三个结论

**1. 它做到「不插电脑」，靠的不是云，是灯里塞了一台电脑。**
规格写明：8 核 ARM64、6GB 内存、跑 Autonomous OS（开源 Linux）。这是整台灯卖 $999 的底气——它本质是一台带身体的迷你主机。你的 ESP32-S3 是 8MB PSRAM，**拿不走这一层**，不要试图模仿。

**2. 它真正值钱的不是硬件，是「身体 / 大脑」彻底解耦的接口设计。**
官方原话是 *"Give Hermes, Claude Code, Codex, and other supported agents eyes, ears, a voice, and a body they can control"*，以及 *"Use the physical APIs from your scripts, services, or independently built agent"*。
翻成人话：**灯只负责声明「我会做什么动作」，模型负责决定「现在做哪个」。换模型不用改灯。** 这一层可以 100% 抄过来，而且它恰好是你现在最缺的一层。

**3. 你现在的接入方式，是这套思路的「反模式」。**
`voice_server.py` 靠 prompt 里教模型输出 `<<LIGHT:on:80>>`，再用正则把它抠出来（第 115 行 `parse_directives`）。为了弥补这种脆弱性，又补了两块补丁：`wants_control()` 语气门（第 320 行）和 `fallback_command()` 关键词兜底。
这不是你的问题——在没有 tool calling 的年代大家都这么干。但 2026 年了，**换成标准 tool calling，这两块补丁可以一起删掉**，而且换模型只改一行配置。

---

## 1. Autonomous Lamp 的技术拆解

| 维度 | 它的做法 | 技术含义 |
|---|---|---|
| 算力 | 8 核 ARM64 / 6GB RAM / Linux | 模型、agent、TTS 全跑在灯里 |
| 模型 | 「Choose your AI」：Claude Code、Codex、Hermes、OpenClaw、OpenCode、PicoClaw | **大脑是插槽，不是焊死的** |
| 接入方式 | physical APIs，脚本/服务/自建 agent 都能调 | **设备是服务器，不是外设** |
| 扩展 | Skills 浏览安装、可改指令、Blender 做动作 | 能力是数据，不是固件 |
| 身体 | 5 舵机 + 广角摄像头 + 麦克风阵列 + 立体声 | 有表情、有视线、会说话 |
| 感知 | CO₂ / PM1.0-10 / 温湿度 | 环境数据进对话 |
| 网络 | 内置 WiFi，云和本地 agent 都能连 | 双向可选 |

它同时支持「连云端 agent」和「连本地 agent」，这恰好证明了一件事：**它的设备接口是模型无关的**。否则 Claude Code 和本地 OpenClaw 不可能共用一套物理 API。

---

## 2. 你和它的真实差距

| 维度 | Autonomous Lamp | 你的台灯 | 差距性质 |
|---|---|---|---|
| 大脑位置 | 灯内（ARM64/6GB） | 现在：局域网 PC；目标：云端 | **可绕过**（换位置，不换架构） |
| 身体 | 5 舵机 + 摄像头 + 立体声 | 2 舵机 + 红外 + 蜂鸣器 | **改不了**（硬件代差） |
| 说话能力 | 立体声扬声器 | 只有蜂鸣器 | **可以补**（加 I2S 功放 ~8 元） |
| 视觉 | 广角摄像头 + 人脸跟踪 | 无 | 放弃 |
| 环境感知 | CO₂/PM/温湿度 | 无（可扩展） | 后续可加 |
| 模型接入 | physical API，模型可插拔 | prompt 正则抠指令，绑定单模型 | **必须改造** ← 本文重点 |
| 部署 | 上电即用，不依赖任何电脑 | 依赖 `192.168.100.100:8765` | **必须改造** |
| 价格 | $999 | — | 不同赛道，不要对标定价 |

一句话：**硬件差距承认，软件架构补齐。** 你要的是「同样的接入理念」，不是「同样的机器」。

---

## 3. 该抄的三样东西

### 3.1 能力协议：让设备自己「报菜名」

现在的逻辑是「人写 prompt 教模型怎么说话」。正确的逻辑是「设备告诉模型我有哪些动词」。

```json
{
  "device": "lamp-s3",
  "proto": 1,
  "tools": [
    { "name": "light_set",  "args": ["on", "brightness", "cct", "preset"] },
    { "name": "motion_look","args": ["yaw", "tilt"] },
    { "name": "ir_send",    "args": ["key"] },
    { "name": "timer_set",  "args": ["minutes", "action"] },
    { "name": "status_get", "args": [] }
  ]
}
```

**关键发现：这 5 个能力，你的固件里已经全部实现了，一个都不缺。**

| 协议工具 | 现有实现 | 缺口 |
|---|---|---|
| `light_set` | `voice_client.cpp:154 applyCommand()` 已支持 light/brightness/cct/preset | **零改动** |
| `motion_look` | `motion.cpp` + `PIN_SERVO_TILT/YAW`，范围 10~55° / -60~60° | 加一个角度入口函数 |
| `ir_send` | `ir_store.cpp` 存码 + `ir_engine.cpp` 发射 | 加一个按 key 调用的入口 |
| `timer_set` | `timer_task.cpp`，最多 8 条，10 秒~24 小时 | 加一个创建任务入口 |
| `status_get` | `lighting::brightness()/cct()/preset()/isOn()` | 拼一个 JSON 返回 |

**所以你不缺能力，缺的只是一张给模型看的「说明书」。** 这是整个方案里最省力、收益最大的一步。

### 3.2 大脑可插拔：一套契约，三家都能接

它支持 Claude Code / Codex / Hermes 并存，说明内部一定是标准工具协议。你照做即可：

| 大脑 | base_url | 模型名 | 工具协议 |
|---|---|---|---|
| 阿里云百炼 | `https://dashscope.aliyuncs.com/compatible-mode/v1` | `qwen-flash` / `qwen-plus` | OpenAI tools |
| 本地 Ollama | `http://127.0.0.1:11434/v1` | `qwen3.5:4b` | OpenAI tools |
| Anthropic | `https://api.anthropic.com` | `claude-*` | 需适配（`input_schema`） |

**三家都用 OpenAI 兼容的 tools 格式，所以只要按这个格式写一次，换模型改两行配置。** 这就是「Choose your AI」在你这里的可实现版本，而且不用换硬件。

### 3.3 能力是数据，不是固件

它能在 App 里装 skill、改指令。你的对应做法：把 system prompt、工具白名单、场景定义放进一个 `skills/*.json`，网关读文件，**加一句话术不用重刷固件**。

这一步可以在 v2 做，但目录结构现在就该留出来。

---

## 4. 不能抄的三样

| 不抄 | 原因 |
|---|---|
| 灯内塞 Linux 主机 | 8MB PSRAM 差 3 个数量级；成本、散热、供应链全部不成立 |
| 摄像头视觉 | 板子没有摄像头接口预算，PSRAM 也不够跑推理 |
| 5 舵机表情系统 | 你有 2 个舵机，够做「转头看人」，不必追求表情 |

另外**不要抄它的商业模式**：$999 的开发者平台卖的是「折腾的乐趣」，你的台灯是消费品，卖的是「插上就能用」。抄协议，别抄定位。

---

## 5. 方案设计

### 5.1 三层架构

```
┌──────────────────────────────────────────────┐
│  Brain（可插拔，改配置就换）                    │
│  百炼 qwen-flash │ 本地 Ollama │ Claude        │
│  职责：听懂人话 → 决定调哪个 tool                │
└──────────────────┬───────────────────────────┘
                   │ tool_calls: light_set{brightness:70}
┌──────────────────▼───────────────────────────┐
│  Gateway（厂商侧，一台小服务器代理所有设备）        │
│  ASR 转写 → 组装 tools → 调模型 → 校验参数       │
│  → 设备路由 / 鉴权 / 限流 / 日志                 │
└──────────────────┬───────────────────────────┘
                   │ 设备主动外连（WSS 长连接，可穿透 NAT）
┌──────────────────▼───────────────────────────┐
│  Body（ESP32-S3，只做「身体」）                  │
│  听：INMP441 麦克风   │  做：灯光 / 舵机 / 红外    │
│  报：亮度 / 色温 / 场景 / 定时任务状态             │
└──────────────────────────────────────────────┘
```

### 5.2 为什么必须改成「设备主动外连」

这是从「开发板」变成「产品」的分水岭，比接大模型重要。

| | 现在（设备当服务端） | 改成（设备当客户端） |
|---|---|---|
| 连接方式 | ESP32 拨 `ws://192.168.100.100:8765` | ESP32 拨 `wss://api.你的域名/v1/device` |
| 换路由器 | IP 一变就断 | 不受影响 |
| 换个房间/城市 | 完全不能用 | 一样能用 |
| 需要端口映射 | 不需要（同局域网） | 不需要（出站长连接） |
| 多台设备 | 每台都要配一个 IP | 设备码自动注册 |
| 密钥安全 | 本机无所谓 | 设备只存 device_secret，模型 key 在服务端 |

顺带解决了你之前那个「ESP32 连不上电脑」的所有问题——**以后根本不需要知道电脑 IP**。

### 5.3 一次完整交互（tool calling 版）

```python
# 1. 定义一次，全模型通用
LAMP_TOOLS = [{
    "type": "function",
    "function": {
        "name": "light_set",
        "description": "控制台灯：开/关、亮度、色温、场景。用户说'太亮了''暗一点'都调 brightness。",
        "parameters": {
            "type": "object",
            "properties": {
                "on":         {"type": "boolean", "description": "true 开灯，false 关灯"},
                "brightness": {"type": "integer", "minimum": 0, "maximum": 100},
                "cct":        {"type": "integer", "minimum": 2700, "maximum": 6500},
                "preset":     {"type": "string", "enum": ["study", "office", "rest", "night"]}
            }
        }
    }
}]

# 2. 调模型
resp = client.chat.completions.create(
    model=cfg.model,              # qwen-flash / qwen3.5:4b / claude-*
    messages=[{"role": "system", "content": SKILL_PROMPT},
              {"role": "user", "content": asr_text}],
    tools=LAMP_TOOLS,
    tool_choice="auto",
)

# 3. 有 tool_call 就下发，没有就只是聊天
msg = resp.choices[0].message
if msg.tool_calls:
    for tc in msg.tool_calls:
        args = json.loads(tc.function.arguments)
        await device.send({"type": "cmd", "tool": tc.function.name, "args": args})
```

对比现在的链路：

| 环节 | 现在 | 改造后 |
|---|---|---|
| 让模型「想动作」 | prompt 里教它写 `<<LIGHT:on:80>>` | 原生 `tool_calls` 结构化 JSON |
| 提取动作 | 正则 `CMD_RE.sub` | 直接读字段，无需解析 |
| 防误触发 | `wants_control()` 语气门 + `fallback_command()` 兜底 | 可保留语气门做二次保险，但兜底逻辑可删 |
| 换模型 | 改 prompt、改解析、重测 | 改一行 `model=` |
| 加新能力 | 改 prompt + 改正则 + 改固件 | 加一个 tool 定义 + 一个固件入口 |

固件侧的改动极小——现在收到的是 `{"type":"cmd","light":"on","brightness":80}`，改造后是 `{"type":"cmd","tool":"light_set","args":{"brightness":80}}`，写个 10 行的 `dispatchTool()` 把 `args` 摊平后喂给现有 `applyCommand()` 即可。

---

## 6. 改造清单

| # | 文件 | 改什么 | 工作量 |
|---|---|---|---|
| 1 | `server/lamp_tools.py`（新） | 工具定义 + JSON Schema 校验 | 半天 |
| 2 | `server/brain.py`（新） | OpenAI 兼容客户端，provider 可配 | 半天 |
| 3 | `server/voice_server.py` | 删 `parse_directives` 正则路径，改走 tool_calls | 半天 |
| 4 | `firmware/src/voice_client.cpp` | 加 `dispatchTool()`，兼容新旧两种 cmd 格式 | 半天 |
| 5 | `firmware/src/motion.cpp` | 暴露 `look(yaw, tilt)` 入口 | 2 小时 |
| 6 | `firmware/src/ir_store.cpp` | 暴露按名称发码入口 | 2 小时 |
| 7 | `firmware/src/timer_task.cpp` | 暴露创建任务入口 | 2 小时 |
| 8 | `server/gateway/`（新） | 设备外连网关 + 设备鉴权 | 2~3 天 |
| 9 | 配网 | SoftAP + 网页填密码（替代 `secrets.h` 硬编码） | 2 天 |
| 10 | TLS | `wss://` 证书校验（或首次绑定 pin 证书） | 半天 |

**顺序不能乱**：1→5 是「让模型会说话」，6→9 是「让灯能出厂」。前者能让你这两天就看到效果，后者决定能不能卖。

---

## 7. 需要你拍板的三件事

### ① 要不要加喇叭（决定产品是「听话的灯」还是「会说话的伙伴」）

Autonomous Lamp 的核心体验是**对话**，因为它有立体声。你现在只有蜂鸣器——模型说了什么，用户听不到。

| | 不加喇叭 | 加喇叭 |
|---|---|---|
| 用户感知 | 说「开灯」→ 灯亮，没有回应 | 说「开灯」→ 灯亮 + 「好的，调到 70%」 |
| 硬件成本 | 0 | ~13 元（MAX98357A + 小喇叭） |
| 云成本 | 4 元/年/台 | ~40 元/年/台（TTS 占 85%） |
| 交互完整度 | 只能说命令，无法对话 | 真对话 |

**我的判断：v1 不加，v2 加。** 先用「灯光 + 舵机动作」做反馈（转头看你、亮度渐变都是天然反馈），验证用户是否真的需要它开口。如果需要，再加——ESP32-S3 有第二路 I2S，8MB PSRAM 放流式音频够用，硬件上随时能补。

### ② 网关放哪

| 选项 | 月成本 | 适合 |
|---|---|---|
| 阿里云函数计算 | 近乎 0（免费额度内） | 先跑通，1~20 台 |
| 轻量应用服务器 2核2G | ~60 元 | 要长连接 + 多设备，推荐 |

外连长连接需要常驻进程，**函数计算不适合挂 WSS 长连接**，所以上了 5 台以上就该换轻量服务器。

### ③ 本地 Ollama 要不要保留

保留有实际价值：断网时降级到本地模型（只对同一局域网的设备生效），或者给你自己开发调试用。**建议保留，但作为「可选插件」，不写进主链路。**

---

## 8. 分阶段路线

| 阶段 | 目标 | 关键交付 | 周期 |
|---|---|---|---|
| **v1** | 模型接入现代化 | tool calling 替换正则、5 个工具全通、本地 Ollama 可跑 | 2~3 天 |
| **v2** | 真正不插电脑 | 设备外连网关 + SoftAP 配网 + 云端 qwen | 4~6 天 |
| **v3** | 体验补齐 | 本地命令词秒回（ESP-SR / KWS）、可选 TTS 回话 | 5~7 天 |
| **v4** | 可卖 | 多设备管理、OTA、误唤醒调优、成本看板 | 持续 |

**v1 就能验证「这套接入理念对不对」，而且完全不依赖云和硬件改动。建议从这里开始。**

---

## 9. 风险

| 风险 | 影响 | 对策 |
|---|---|---|
| 模型乱调工具（说「灯好亮啊」被当成命令） | 误操作，最伤体验 | 保留 `wants_control` 语气门做二次确认；高风险动作（红外发码）需二次确认 |
| `wss` 证书校验失败 | 设备连不上 | 首次配网时下发 CA，不要用 `setInsecure()` 上生产 |
| 云端 ASR 中文识别不如本地 Vosk 准 | 指令理解错 | 领域词表热词（「暖光」「阅读模式」）；v3 本地命令词兜底 |
| 设备密钥被提取 | 盗刷 token | 每设备独立 secret + 服务端限流 + 异常行为封禁 |
| 舵机噪音干扰麦克风 | 录音质量差 | 录音期间禁止舵机动作（现有 `lighting::setScale` 思路同理） |

---

## 10. 一句话总结

Autonomous Lamp 证明了「灯 = 身体，模型 = 可换的大脑」这条路成立，但它用 $999 的硬件绕过了算力问题。
你绕不过硬件，但可以**把接口做对**：让灯自己声明能力（5 个工具），让任何模型通过标准 tool calling 驱动它。
**固件里那 5 个能力已经写好了，你现在缺的只是一张说明书和一个能让灯连出去的云网关。**
