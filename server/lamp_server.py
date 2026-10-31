"""LAMP-1 语音闭环服务端（PC 侧）。

一条进程里同时跑三件事：

  1. 浏览器页面  http://<PC-IP>:8080/          文字聊天（流式输出）
  2. WebSocket   ws://<PC-IP>:8080/ws          浏览器/脚本用，JSON 协议
  3. TCP         <PC-IP>:8766                  ESP32 用，音频进、文字出

数据流：ESP32/浏览器 PCM(16k mono s16le) → Vosk 离线 ASR → Ollama 本地模型
        → 文字一边生成一边回传（ESP32 逐行 println，浏览器逐字显示）。

用法：
    python server/lamp_server.py
    python server/lamp_server.py --model qwen3.5:9b --http-port 8080 --tcp-port 8766

ESP32 侧的协议（文本行 + 二进制块，便于抓包排查）：
    PCM <字节数>\\n  + <原始 PCM 字节>      # 可以连发很多块
    END\\n                                   # 一句话说完，等待识别和回答
    PING\\n                                  # 心跳，回 PONG\\n
  服务端回：
    HEARD <识别出的文字>\\n
    REPLY <回答片段>\\n                      # 可能有多行
    DONE\\n
    ERR <原因>\\n
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import json
import os
import sys
import time

import aiohttp
from aiohttp import WSMsgType, web

try:
    import vosk
except ImportError:  # 没装也能跑文字聊天，只是没有 ASR
    vosk = None

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_ASR_DIR = os.path.join(HERE, "models", "vosk-model-small-cn-0.22")

SYSTEM_PROMPT = (
    "你是 LAMP-1 桌面台灯的语音助手。回答用简体中文，口语化，"
    "控制在三句话以内，不要使用 Markdown、列表或代码块，"
    "因为答案会被逐字打印在台灯的单色小屏和串口上。"
)


# --------------------------------------------------------------------------
# Ollama
# --------------------------------------------------------------------------
async def ollama_stream(session: aiohttp.ClientSession, url: str, model: str, messages, on_token, think=False):
    payload = {
        "model": model,
        "messages": messages,
        "stream": True,
        "options": {"temperature": 0.7, "num_predict": 220},
    }
    if not think:
        # 思考型模型（qwen3 / deepseek-r1）默认会先吐一大段思考，1.5B 上会把
        # num_predict 吃光；关掉思考，直接出答案。
        payload["think"] = False

    async def post(body):
        return await session.post(url, json=body)

    resp = await post(payload)
    if resp.status == 400:
        text = await resp.text()
        # 老模型不认识 think 参数，去掉再试一次
        if "think" in text.lower() and "think" in payload:
            payload.pop("think")
            resp = await post(payload)
        else:
            raise RuntimeError("ollama %s: %s" % (400, text[:300]))
    if resp.status != 200:
        text = await resp.text()
        raise RuntimeError("ollama %s: %s" % (resp.status, text[:300]))

    async with resp:
        async for raw in resp.content:
            line = raw.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue
            piece = (obj.get("message") or {}).get("content", "")
            if piece:
                await on_token(piece)
            if obj.get("done"):
                break


async def ask_brain(session, args, question: str, on_token) -> str:
    messages = [
        {"role": "system", "content": SYSTEM_PROMPT},
        {"role": "user", "content": question},
    ]
    answer = []

    async def collect(tok):
        answer.append(tok)
        await on_token(tok)

    await ollama_stream(session, args.ollama, args.model, messages, collect)
    return "".join(answer)


# --------------------------------------------------------------------------
# ASR
# --------------------------------------------------------------------------
class Asr:
    def __init__(self, model_dir: str):
        self.model = None
        if vosk is None:
            print("!! 没装 vosk，ASR 不可用（pip install vosk）")
            return
        if not os.path.isdir(model_dir):
            print("!! 找不到 Vosk 模型：%s" % model_dir)
            return
        t0 = time.time()
        self.model = vosk.Model(model_dir)
        print("ASR 模型已加载（%.1fs）：%s" % (time.time() - t0, model_dir))

    @property
    def ready(self) -> bool:
        return self.model is not None

    def new_recognizer(self):
        return vosk.KaldiRecognizer(self.model, 16000)

    @staticmethod
    def clean(result_json: str) -> str:
        text = json.loads(result_json).get("text", "")
        # 中文模型会在字之间插空格；去掉空格和常见标点，便于阅读
        for ch in " \t":
            text = text.replace(ch, "")
        return text.strip()


# --------------------------------------------------------------------------
# ESP32 TCP 协议
# --------------------------------------------------------------------------
async def esp_client(reader: asyncio.StreamReader, writer: asyncio.StreamWriter, state):
    peer = writer.get_extra_info("peername")
    print("[esp] 连接来自 %s" % (peer,))
    rec = state["asr"].new_recognizer() if state["asr"].ready else None
    session = state["session"]
    audio_bytes = 0

    async def send(line: str):
        writer.write((line + "\n").encode("utf-8"))
        await writer.drain()

    try:
        while True:
            line = await reader.readline()
            if not line:
                break
            cmd = line.strip()
            if cmd.startswith(b"PCM "):
                n = int(cmd[4:] or 0)
                data = await reader.readexactly(n)
                audio_bytes += len(data)
                if rec is not None:
                    rec.AcceptWaveform(data)
            elif cmd == b"END":
                seconds = audio_bytes / 32000.0
                audio_bytes = 0
                if rec is None:
                    await send("ERR asr-not-ready")
                    continue
                text = state["asr"].clean(rec.FinalResult())
                rec = state["asr"].new_recognizer()
                print("[esp] 收到 %.2fs 音频 -> %r" % (seconds, text))
                await send("HEARD " + (text or "（没听清）"))
                if not text:
                    await send("REPLY 没听清，再说一遍？")
                    await send("DONE")
                    continue

                buf = []

                async def on_tok(tok):
                    buf.append(tok)
                    text = "".join(buf).replace("\n", " ")
                    # 每 24 个字符切一行发出去：ESP32 那边逐行 println，屏幕/串口都好看
                    while len(text) >= 24:
                        await send("REPLY " + text[:24])
                        text = text[24:]
                    buf.clear()
                    if text:
                        buf.append(text)

                try:
                    await ask_brain(session, state["args"], text, on_tok)
                    rest = "".join(buf).replace("\n", " ")
                    if rest:
                        await send("REPLY " + rest)
                    await send("DONE")
                except Exception as exc:  # noqa: BLE001
                    print("[esp] 模型出错：%r" % (exc,))
                    await send("ERR brain")
            elif cmd == b"PING":
                await send("PONG")
            else:
                print("[esp] 未知命令：%r" % (cmd[:60],))
    except (asyncio.IncompleteReadError, ConnectionResetError):
        pass
    finally:
        print("[esp] 断开 %s" % (peer,))
        writer.close()
        try:
            await writer.wait_closed()
        except Exception:
            pass


# --------------------------------------------------------------------------
# HTTP + WebSocket
# --------------------------------------------------------------------------
async def index(_request):
    return web.FileResponse(os.path.join(HERE, "web", "index.html"))


async def ws_handler(request: web.Request) -> web.WebSocketResponse:
    ws = web.WebSocketResponse(heartbeat=None)
    await ws.prepare(request)
    state = request.app["state"]
    rec = None
    audio = bytearray()

    async def send(obj):
        await ws.send_json(obj)

    async for msg in ws:
        if msg.type == WSMsgType.TEXT:
            try:
                data = json.loads(msg.data)
            except json.JSONDecodeError:
                continue
            kind = data.get("type")

            if kind == "ask":
                question = (data.get("text") or "").strip()
                if not question:
                    continue
                await send({"type": "heard", "text": question})
                try:
                    async def on_tok(tok):
                        await send({"type": "token", "text": tok})

                    await ask_brain(state["session"], state["args"], question, on_tok)
                except Exception as exc:  # noqa: BLE001
                    await send({"type": "error", "text": str(exc)})
                await send({"type": "done"})

            elif kind == "audio_start":
                if not state["asr"].ready:
                    await send({"type": "error", "text": "ASR 未就绪"})
                    continue
                rec = state["asr"].new_recognizer()
                audio.clear()

            elif kind == "audio":
                pcm = base64.b64decode(data.get("b64") or "")
                audio.extend(pcm)
                if rec is not None:
                    rec.AcceptWaveform(pcm)

            elif kind == "audio_end":
                if rec is None:
                    continue
                text = state["asr"].clean(rec.FinalResult())
                rec = None
                await send({"type": "heard", "text": text or "（没听清）"})
                if not text:
                    await send({"type": "done"})
                    continue
                try:
                    async def on_tok(tok):
                        await send({"type": "token", "text": tok})

                    await ask_brain(state["session"], state["args"], text, on_tok)
                except Exception as exc:  # noqa: BLE001
                    await send({"type": "error", "text": str(exc)})
                await send({"type": "done"})

        elif msg.type == WSMsgType.BINARY:
            # 预留：直接把裸 PCM 发上来
            if rec is not None:
                rec.AcceptWaveform(msg.data)

    return ws


# --------------------------------------------------------------------------
async def main_async(args):
    if not os.path.isdir(args.asr_dir) and args.asr_dir == DEFAULT_ASR_DIR:
        print("提示：ASR 模型还没下载，先只跑文字聊天。")
    asr = Asr(args.asr_dir)

    session = aiohttp.ClientSession()
    app = web.Application()
    app["state"] = {"asr": asr, "session": session, "args": args}
    app.router.add_get("/", index)
    app.router.add_get("/ws", ws_handler)
    app.router.add_static("/static", os.path.join(HERE, "web"))

    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, "0.0.0.0", args.http_port)
    await site.start()

    tcp = await asyncio.start_server(lambda r, w: esp_client(r, w, app["state"]), "0.0.0.0", args.tcp_port)

    print("HTTP  : http://127.0.0.1:%d/  （局域网用本机 IP）" % args.http_port)
    print("WS    : ws://127.0.0.1:%d/ws" % args.http_port)
    print("TCP   : 0.0.0.0:%d  （ESP32 连这里）" % args.tcp_port)
    print("模型  : %s @ %s" % (args.model, args.ollama))
    print("ASR   : %s" % ("就绪" if asr.ready else "未就绪"))

    try:
        await asyncio.Future()
    finally:
        tcp.close()
        await tcp.wait_closed()
        await session.close()
        await runner.cleanup()


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--model", default=os.environ.get("LAMP_MODEL", "qwen3.5:9b"))
    p.add_argument("--ollama", default=os.environ.get("OLLAMA_URL", "http://127.0.0.1:11434/api/chat"))
    p.add_argument("--asr-dir", default=os.environ.get("LAMP_ASR_DIR", DEFAULT_ASR_DIR))
    p.add_argument("--http-port", type=int, default=8080)
    p.add_argument("--tcp-port", type=int, default=8766)
    return p.parse_args()


if __name__ == "__main__":
    try:
        asyncio.run(main_async(parse_args()))
    except KeyboardInterrupt:
        sys.exit(0)
