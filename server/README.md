# server —— 局域网 ASR + DeepSeek 大脑

对应语音验证的 **任务二（大脑）** 与 **任务三（闭环）** 的电脑侧。
ESP32 只负责采集 16 kHz 单声道 s16le PCM 并通过 WebSocket 推过来；
本目录负责离线语音识别（Vosk）、调用本地 Ollama 大模型、把回答文字回传。

```
ESP32 ──WS(binary PCM)──▶ voice_server.py ──▶ Vosk ASR ──▶ Ollama(qwen3.5:9b)
   ▲                                                              │
   └───────────────── WS(JSON text reply) ◀───────────────────────┘
```

## 1. 安装

```powershell
# Ollama（已装可跳过）
winget install Ollama.Ollama          # 或 https://ollama.com/download
ollama pull qwen3.5:9b              # 6.6 GB，12 GB 显存可全量加载；deepseek-r1:1.5b 更小但容易胡说
ollama list

# Python 依赖（Python 3.10+）
cd server
pip install -r requirements.txt
```

Vosk 中文小模型已经放在 `server/models/vosk-model-small-cn-0.22`（约 40 MB），
无需再下载。可以用 `--vosk-model` 指向别的模型目录。

## 2. 由浅入深的三步验证

```powershell
# ① 大脑：终端直接问 DeepSeek（任务二）
python ask.py "用一句话介绍你自己"
python ask.py --stream                # 流式输出 + 交互式提问

# ② 耳朵：用现成 wav 验证 ASR
python asr_test.py                    # 识别 test_zh.wav
python asr_test.py D:\record\me.wav   # 任意 wav，自动转 16k 单声道

# ③ 整条链路自检（不需要 ESP32）
python voice_server.py --check        # Ollama + Vosk 都 ok 才返回 0
```

启动服务器，并在另一个终端模拟 ESP32：

```powershell
python voice_server.py                # 默认 0.0.0.0:8765
python test_client.py                 # test_zh.wav -> ASR -> DeepSeek
python test_client.py --text "今天适合复习吗"
python test_client.py --url ws://192.168.1.23:8765
```

## 3. 让 ESP32 连上来

1. `ipconfig` 查**与 ESP32 同网段**那块网卡的 IPv4（例如 `192.168.1.23`）。
   如果电脑只有有线网卡（本机就是：10.1.41.14/24），需要一个把 Wi-Fi 桥接到该网段的 AP，
   否则 ESP32 根本到不了这台机器。
2. 复制 `firmware/include/secrets.h.example` 为 `firmware/include/secrets.h`，
   填入 WiFi 名称、密码和上面的 IP。
3. 管理员 PowerShell 放行入站端口：
   ```powershell
   netsh advfirewall firewall add rule name="LAMP voice" dir=in action=allow protocol=TCP localport=8765
   ```
4. `cd firmware && pio run -e lamp_s3 -t upload`，串口输入 `voice status`，
   应看到 `wifi=up ip=192.168.1.x ws=up mic=ready`。

## 4. WebSocket 协议

控制帧是 JSON 文本，音频帧是二进制（未经 base64，省带宽省 CPU）。

| 方向 | 帧 | 内容 |
|------|----|------|
| ESP32 → PC | `{"type":"audio_start","sample_rate":16000,"format":"s16le","channels":1}` | 一句话开始 |
| ESP32 → PC | 二进制帧 | 16 kHz / 16 bit / 单声道 PCM，每帧约 32 ms |
| ESP32 → PC | `{"type":"audio_end","frames":N,"reason":"silence"}` | 一句话结束 |
| ESP32 → PC | `{"type":"text","content":"你好"}` | 跳过 ASR，直接问大脑 |
| PC → ESP32 | `{"type":"ready","model":"qwen3.5:9b","asr":true}` | 连接建立 |
| PC → ESP32 | `{"type":"asr","text":"..."}` | 识别结果（先发，便于观察） |
| PC → ESP32 | `{"type":"cmd","light":"on","brightness":80}` | **执行动作**：开关/亮度/色温/场景 |
| PC → ESP32 | `{"type":"reply","text":"...","asr":"...","elapsed_ms":1234}` | 最终回答 |
| PC → ESP32 | `{"type":"error","message":"..."}` | 出错 |

`cmd` 是"说开灯 → 灯真的亮"的关键：模型在回答末尾输出 `<<LIGHT:on>>` 这类指令，
服务器用 `parse_directives()` 摘出来、转成 `cmd` 先发给 ESP32，再把去掉指令的文字作为
`reply` 发出。模型忘记输出指令时，`fallback_command()` 用关键词（开灯/关灯）兜底。

服务器把 Vosk 当流式解码器用：二进制帧到达即 `AcceptWaveform()`，
`audio_end` 时 `FinalResult()` 取最终文本，所以长句也不会爆内存。
单句上限由 `--max-seconds`（默认 15 s）限制。

## 5. 常用参数

| 参数 | 默认 | 说明 |
|------|------|------|
| `--host` | `0.0.0.0` | 监听所有网卡（ESP32 必须能访问） |
| `--port` | `8765` | 与 `secrets.h` 的 `LAMP_VOICE_SERVER_PORT` 一致 |
| `--model` | `qwen3.5:9b` | 默认 9B：热机单句约 0.4 s（首句含 18 s 加载）；换 `deepseek-r1:1.5b` 更小但容易胡说 |
| `--ollama` | `http://127.0.0.1:11434` | Ollama 地址 |
| `--max-seconds` | `15` | 超长音频直接拒绝 |
| `--timeout` | `120` | 大模型超时（秒） |
| `--no-asr` | 关 | 只测大脑（Vosk 不加载） |
| `--check` | 关 | 自检 ASR + Ollama 后退出 |

## 6. 常见问题

| 现象 | 原因 / 处理 |
|------|-------------|
| `--check` 里 Ollama FAILED | 没启动 `ollama serve`，或模型名不一致（`ollama list` 核对） |
| `--check` 里 Vosk FAILED | 模型目录被移动/解压不完整；重新解压 `models/*.zip` |
| `test_client.py` 卡住 | 模型首次加载 + CPU 推理需要几秒；`--timeout 300` 再试 |
| ESP32 连不上 `ws=down` | 先 `ping` 电脑 IP；确认防火墙放行 8765；ESP32 与电脑必须在同一网段 |
| ASR 识别为空 | 麦克风数据太小或采样率不对；先在固件里 `mic test` 看 RMS |
| 回答里有 `思考过程` | 已在服务端用 `strip_thinking()` 去 `<think>` 标签并传 `think:false`；若模型仍输出，可换模型 |

> 本目录所有脚本都只监听局域网，没有鉴权。仅用于 MVP 验证，
> 不要把 8765 暴露到公网。