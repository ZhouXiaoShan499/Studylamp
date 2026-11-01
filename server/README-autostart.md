# 开机自启：让语音服务不用手动运行

这份文档解决一个问题：**不想每次开电脑都手动敲 `python voice_server.py`，想要「上电即用」。**

## 原理

`voice_server.py` 是常驻服务（WebSocket 8765），ESP32 上电后会主动连它。
只要让这个 Python 进程**开机自动在后台运行**，你就不需要碰任何命令行。

做法用的是 Windows 自带的**任务计划程序（Task Scheduler）**，不依赖任何第三方工具
（不需要 nssm），注册为 SYSTEM 账号、开机即启动、无需登录桌面也能跑。

```
开机 ──▶ 任务计划自动拉起 run_voice_service.bat ──▶ venv 里的 python ──▶ voice_server.py
                                                          ▲
                                              异常退出 3 秒后自动重启
```

## 三步完成（一次性操作）

1. **确认 Ollama 已装好模型并设为开机自启**（Ollama 安装器默认就会开机自启）：

   ```powershell
   ollama list              # 应能看到 qwen3.5:4b
   ```

2. **右键「以管理员身份运行」`install_service.bat`**。它会自动：
   - 用 Python 3.10 创建独立虚拟环境 `server\venv`
   - 安装 `vosk` / `websockets` / `requests` 依赖
   - 注册开机自启任务「LAMP Voice Service」
   - 放行防火墙 8765 端口

3. **立即验证一次**（不用等重启）：

   ```powershell
   schtasks /run /tn "LAMP Voice Service"
   ```

   然后自检：

   ```powershell
   server\venv\Scripts\python.exe server\voice_server.py --check
   ```

   看到 `Ollama : ok` 和 `Vosk   : ok` 就说明整条链路通了。

## 文件清单

| 文件 | 作用 |
|------|------|
| `install_service.bat` | 一键安装（建 venv + 装依赖 + 注册自启 + 放行防火墙） |
| `run_voice_service.bat` | 实际运行入口，崩溃自动重启 |
| `uninstall_service.bat` | 一键卸载自启任务和防火墙规则 |
| `logs\voice_service.log` | 运行日志（排查问题时看这里） |

## 常用操作

```powershell
# 立即启动 / 停止
schtasks /run /tn "LAMP Voice Service"
schtasks /end /tn "LAMP Voice Service"

# 查看任务是否存在
schtasks /query /tn "LAMP Voice Service"

# 看实时日志
Get-Content server\logs\voice_service.log -Wait -Tail 50
```

## 常见问题

| 现象 | 处理 |
|------|------|
| `--check` 里 Ollama FAILED | 确认 Ollama 已开机自启且模型名是 `qwen3.5:4b`（`ollama list` 核对） |
| `--check` 里 Vosk FAILED | `server\models\vosk-model-small-cn-0.22` 目录是否完整（约 40MB） |
| ESP32 连不上 | 确认电脑 IP 写进了 `firmware/include/secrets.h`；防火墙已放行 8765；ESP32 与电脑同网段 |
| 想换回 9b 模型 | 设置系统环境变量 `LAMP_MODEL=qwen3.5:9b` 后重启任务即可 |
| 想彻底清理 | 先跑 `uninstall_service.bat`，再删 `server\venv` |

> 注意：任务计划用 SYSTEM 账号运行，`run_voice_service.bat` 的路径必须保持不动，
> 移动项目目录后需重新运行 `install_service.bat`。
