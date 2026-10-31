#!/usr/bin/env python3
"""LAMP-1 LAN voice bridge.

    ESP32 (INMP441 I2S mic) --WebSocket--> this server --> Vosk ASR --> Ollama
                                                          |
    ESP32 Serial <--WebSocket text (reply) <--------------+

Protocol (all control frames are JSON text, audio frames are raw binary):

    -> {"type":"audio_start","sample_rate":16000,"format":"s16le","channels":1}
    -> <binary PCM chunks: 16-bit little-endian mono>
    -> {"type":"audio_end","frames":N,"reason":"silence"}
    <- {"type":"asr","text":"..."}
    <- {"type":"reply","text":"...","asr":"...","elapsed_ms":1234}
    <- {"type":"error","message":"..."}

    -> {"type":"text","content":"你好"}      (skip ASR, test the brain)
    <- {"type":"ready","model":"...","asr":true}

Run:  python voice_server.py --model qwen3.5:9b
Check: python voice_server.py --check
"""

from __future__ import annotations

import argparse
import array
import asyncio
import json
import logging
import re
import sys
import time
import wave
from pathlib import Path
from typing import Optional

import requests
import websockets

LOG = logging.getLogger("voice_server")

BASE_DIR = Path(__file__).resolve().parent
DEFAULT_VOSK_MODEL = BASE_DIR / "models" / "vosk-model-small-cn-0.22"
SAMPLE_RATE = 16000

SYSTEM_PROMPT = (
    "你是「小灯」，一盏桌面智能台灯的语音助手。"
    "回答要口语化、亲切、简短：最多三句话，总字数不超过 80 字。"
    "不要使用 Markdown、列表或表情符号，因为答案会被直接念出来。"
    "如果问题没听清或信息不足，就自然地反问一句。\n"
    "当用户想控制台灯（开关、亮度、色温、场景）时，先给一句简短回应，"
    "再另起一行输出控制指令，指令本身不会显示给用户：\n"
    "<<LIGHT:on>> 开灯；<<LIGHT:on:80>> 开灯并调到 80%；<<LIGHT:off>> 关灯；\n"
    "<<BRI:50>> 只调亮度；<<CCT:4000>> 调色温；<<PRESET:night>> 切场景"
    "（可选 study/office/rest/night）。\n"
    "纯聊天时不要输出任何指令。"
    "只有用户明确说出开关灯、调亮度/色温、切场景时才输出指令，"
    "不要主动替用户做这些操作，也不要顺手附赠指令。"
    "如果用户只是在询问意见、征求意见（比如「适合复习吗」「要不要学习」「休息一下吧」），"
    "只回答建议，绝不输出指令。"
    "判断标准：用户是否用了明确的祈使/命令句式，例如「开灯」「关灯」「把灯打开」"
    "「调到一半」「切换成夜间模式」。"
    "带疑问、商量、征求意见的语气（「要不要」「吧」「吗」「可以吗」）都不算命令，"
    "不输出任何指令。"
)

WEEKDAYS = ("星期一", "星期二", "星期三", "星期四", "星期五", "星期六", "星期日")


def build_system_prompt(now: Optional[time.struct_time] = None) -> str:
    """把当前日期/星期/时间拼进 system prompt，让模型能如实回答时间类问题。

    Ollama 的 system 字段用一次就定死，无法靠模型自己「知道」时间；这里在每次
    提问时注入本地时钟，既真实又不额外发请求。
    """
    now = now or time.localtime()
    stamp = "今天是 %d 年 %d 月 %d 日，%s，现在是 %02d:%02d。" % (
        now.tm_year, now.tm_mon, now.tm_mday,
        WEEKDAYS[now.tm_wday], now.tm_hour, now.tm_min,
    )
    return stamp + " " + SYSTEM_PROMPT

CMD_RE = re.compile(r"<<\s*([A-Za-z]+)\s*:\s*([^>]*)>>")

DEBUG_DIR = BASE_DIR / "debug"


def pcm_stats(pcm: bytes) -> tuple[int, int]:
    """峰值 / RMS：区分"没声音"和"有声音但识别不出"。"""
    if len(pcm) < 2:
        return 0, 0
    samples = array.array("h")
    samples.frombytes(pcm[: len(pcm) // 2 * 2])
    if sys.byteorder == "big":
        samples.byteswap()
    peak = max(max(samples), -min(samples))
    rms = int((sum(s * s for s in samples) / len(samples)) ** 0.5)
    return peak, rms


def save_debug_wav(pcm: bytes, label: str = "utt") -> str:
    """把这段录音落盘，方便事后用 Audacity / 离线 ASR 复查。"""
    DEBUG_DIR.mkdir(exist_ok=True)
    path = DEBUG_DIR / f"{label}_{time.strftime('%H%M%S')}.wav"
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(SAMPLE_RATE)
        handle.writeframes(pcm)
    return str(path)


def parse_directives(answer: str) -> tuple[str, list[dict]]:
    """把 <<LIGHT:on:80>> 这类指令从回答里摘出来，变成 ESP32 能执行的 cmd。"""
    cmds: list[dict] = []
    for raw_name, raw_arg in CMD_RE.findall(answer):
        parts = [p.strip() for p in re.split(r"[:：]", raw_arg) if p.strip()]
        if not parts:
            continue
        name = raw_name.upper()
        cmd = None
        if name == "LIGHT":
            action = parts[0].lower()
            if action in ("on", "off", "toggle"):
                cmd = {"light": action}
                if action == "on" and len(parts) > 1 and parts[1].isdigit():
                    cmd["brightness"] = max(1, min(100, int(parts[1])))
        elif name == "BRI" and parts[0].isdigit():
            cmd = {"brightness": max(0, min(100, int(parts[0])))}
        elif name == "CCT" and parts[0].isdigit():
            cmd = {"cct": max(2700, min(6500, int(parts[0])))}
        elif name == "PRESET" and parts[0].lower() in ("study", "office", "rest", "night"):
            cmd = {"preset": parts[0].lower()}
        if cmd:
            cmds.append(cmd)
    return CMD_RE.sub("", answer).strip(), cmds


def fallback_command(text: str) -> Optional[dict]:
    """模型忘了写指令时的兜底：开关灯必须可靠，不能靠 1.5B 的自觉。"""
    compact = re.sub(r"\s+", "", text or "")
    if not compact:
        return None
    if any(word in compact for word in ("关灯", "关闭灯", "把灯关", "灯关掉", "关掉灯")):
        return {"light": "off"}
    if any(word in compact for word in ("开灯", "打开灯", "把灯开", "灯打开", "开一下灯")):
        return {"light": "on"}
    return None


# 明确命令式触发词：只有用户原话里出现这些词，才允许模型吐出的指令真正执行。
# 9B 模型对「疑问 vs 命令」的区分不稳定——文字层答得对，指令层却会偷偷附赠，
# 所以这里用确定性规则兜底，把最终拍板权从模型手里收回来。
_COMMAND_HINTS = (
    "开灯", "打开灯", "把灯开", "关灯", "关掉灯", "把灯关", "灯关掉",
    "亮度", "调亮", "调暗", "色温", "调色温",
    "场景", "切换", "模式", "夜间", "专注", "办公", "学习模式", "休息模式",
)


def wants_control(text: str) -> bool:
    """用户原话是否在明确下指令（而非征求意见/纯聊天）。"""
    compact = re.sub(r"\s+", "", text or "")
    if not compact:
        return False
    # 疑问/商量语气直接判为不下指令
    if any(w in compact for w in ("吗", "吧", "要不要", "可以吗", "好不好", "呢")):
        return False
    return any(hint in compact for hint in _COMMAND_HINTS)


def strip_thinking(text: str) -> str:
    """DeepSeek-R1 may wrap its chain of thought in <think> tags."""
    cleaned = re.sub(r"<think(?:ing)?>.*?</think(?:ing)?>", "", text, flags=re.S | re.I)
    # Truncated reply: drop an unterminated block instead of speaking it aloud.
    cleaned = re.sub(r"<think(?:ing)?>.*$", "", cleaned, flags=re.S | re.I)
    return cleaned.strip()


# 模型偶尔把「(无控制指令)」这类备注直接写进正文，会被台灯屏幕显示出来，洗掉。
_NOISE = re.compile(r"[（(]\s*无控制指令\s*[)）]", re.I)


def strip_noise(text: str) -> str:
    return _NOISE.sub("", text or "").strip()


class Asr:
    """Thin Vosk wrapper. Missing model/dependency degrades to text-only mode."""

    def __init__(self, model_path: Path):
        self.model_path = model_path
        self.error: Optional[str] = None
        self._vosk = None
        self._model = None
        try:
            import vosk  # type: ignore

            vosk.SetLogLevel(-1)
            if not model_path.exists():
                raise FileNotFoundError(f"Vosk model not found: {model_path}")
            LOG.info("loading Vosk model: %s", model_path)
            self._model = vosk.Model(str(model_path))
            self._vosk = vosk
            LOG.info("Vosk model ready")
        except Exception as exc:  # noqa: BLE001 - startup must not crash
            self.error = str(exc)
            LOG.warning("ASR disabled: %s", exc)

    @property
    def available(self) -> bool:
        return self._model is not None

    def new_recognizer(self):
        if not self.available:
            return None
        return self._vosk.KaldiRecognizer(self._model, SAMPLE_RATE)

    @staticmethod
    def final_text(recognizer) -> str:
        if recognizer is None:
            return ""
        try:
            payload = json.loads(recognizer.FinalResult())
        except Exception:  # noqa: BLE001
            return ""
        return str(payload.get("text", "")).strip()


class Brain:
    """Ollama /api/generate client."""

    def __init__(self, host: str, model: str, timeout: int = 120):
        self.host = host.rstrip("/")
        self.model = model
        self.timeout = timeout

    def health(self) -> bool:
        try:
            response = requests.get(f"{self.host}/api/tags", timeout=5)
            response.raise_for_status()
            names = {item.get("name") for item in response.json().get("models", [])}
            if self.model not in names:
                LOG.warning("model %s is not pulled yet (ollama pull %s)", self.model, self.model)
                return False
            return True
        except Exception as exc:  # noqa: BLE001
            LOG.warning("Ollama not reachable at %s: %s", self.host, exc)
            return False

    def ask(self, prompt: str) -> tuple[str, int]:
        started = time.perf_counter()
        response = requests.post(
            f"{self.host}/api/generate",
            json={
                "model": self.model,
                "prompt": prompt,
                "system": build_system_prompt(),
                "stream": False,
                "think": False,
                "options": {
                    "temperature": 0.7,
                    # 1.5B 的思考链能吃满 220 个 token（实测 done_reason=length，
                    # strip_thinking 之后正文为空，只剩兜底话术），这里留足预算。
                    "num_predict": 768,
                },
            },
            timeout=self.timeout,
        )
        response.raise_for_status()
        payload = response.json()
        answer = strip_noise(strip_thinking(str(payload.get("response", ""))))
        if not answer:
            LOG.warning(
                "empty answer after stripping (done_reason=%s, eval_count=%s)",
                payload.get("done_reason"),
                payload.get("eval_count"),
            )
            answer = "我刚刚走神了，能再问一次吗？"
        elapsed_ms = int((time.perf_counter() - started) * 1000)
        return answer, elapsed_ms


class DisabledAsr:
    """Stand-in used by --no-asr (text-only debugging)."""

    available = False
    error = "disabled by --no-asr"

    def new_recognizer(self):
        return None

    @staticmethod
    def final_text(recognizer) -> str:  # noqa: ARG004
        return ""


async def send_json(ws, payload: dict) -> None:
    await ws.send(json.dumps(payload, ensure_ascii=False))


async def answer_and_send(ws, brain: Brain, prompt: str, asr_text: str) -> None:
    if not prompt:
        await send_json(
            ws,
            {"type": "reply", "text": "我没听清，再说一遍好吗？", "asr": "", "elapsed_ms": 0},
        )
        return
    try:
        answer, elapsed_ms = await asyncio.to_thread(brain.ask, prompt)
    except Exception as exc:  # noqa: BLE001
        LOG.exception("LLM call failed")
        await send_json(ws, {"type": "error", "message": f"LLM failed: {exc}"})
        return
    LOG.info("reply in %d ms: %s", elapsed_ms, answer)

    text, cmds = parse_directives(answer)
    # 硬门：只有用户原话明确在下命令时才放行指令，拦截模型的「顺手附赠」。
    if cmds and not wants_control(prompt):
        LOG.info("suppressed %d cmd(s), user was not giving an order: %r", len(cmds), prompt)
        cmds = []
    if not cmds:
        fallback = fallback_command(prompt)
        if fallback:
            cmds = [fallback]
            LOG.info("model forgot the directive, keyword fallback -> %s", fallback)
    for cmd in cmds:
        LOG.info("cmd -> %s", cmd)
        await send_json(ws, {"type": "cmd", **cmd})
    await send_json(
        ws,
        {
            "type": "reply",
            "text": text or "好的。",
            "asr": asr_text,
            "elapsed_ms": elapsed_ms,
            "commands": cmds,
        },
    )


async def handle_connection(ws, asr, brain: Brain, max_audio_bytes: int) -> None:
    peer = f"{ws.remote_address[0]}:{ws.remote_address[1]}" if ws.remote_address else "?"
    LOG.info("[%s] connected (asr=%s)", peer, asr.available)
    recognizer = asr.new_recognizer()
    audio_bytes = 0
    recording = False
    pcm = bytearray()

    try:
        await send_json(
            ws,
            {"type": "ready", "model": brain.model, "asr": asr.available,
             "asr_error": asr.error},
        )

        async for message in ws:
            if isinstance(message, (bytes, bytearray)):
                if not recording:
                    continue
                audio_bytes += len(message)
                if len(pcm) < 16000 * 2 * 15:  # 只保留前 15 秒用于排查
                    pcm.extend(message)
                if audio_bytes > max_audio_bytes:
                    LOG.warning("[%s] audio exceeds %d bytes, dropped", peer, max_audio_bytes)
                    recording = False
                    recognizer = asr.new_recognizer()
                    await send_json(ws, {"type": "error", "message": "audio too long"})
                    continue
                if recognizer is not None:
                    # Feed Vosk live: it decodes incrementally, audio_end is cheap.
                    recognizer.AcceptWaveform(bytes(message))
                continue

            try:
                data = json.loads(message)
            except json.JSONDecodeError:
                await send_json(ws, {"type": "error", "message": "invalid json"})
                continue

            kind = data.get("type", "")
            if kind == "audio_start":
                recognizer = asr.new_recognizer()
                audio_bytes = 0
                pcm = bytearray()
                recording = True
                LOG.info("[%s] audio_start (%s Hz)", peer, data.get("sample_rate", "?"))
            elif kind == "audio_end":
                recording = False
                text = asr.final_text(recognizer)
                recognizer = asr.new_recognizer()
                peak, rms = pcm_stats(pcm)
                saved = save_debug_wav(pcm) if pcm else "-"
                LOG.info("[%s] audio_end: %d bytes peak=%d rms=%d -> %r  %s", peer, audio_bytes, peak, rms,
                         text, saved)
                await send_json(ws, {"type": "asr", "text": text})
                await answer_and_send(ws, brain, text, text)
            elif kind == "text":
                text = str(data.get("content", "")).strip()
                LOG.info("[%s] text: %r", peer, text)
                await send_json(ws, {"type": "asr", "text": text})
                await answer_and_send(ws, brain, text, text)
            elif kind == "hello":
                await send_json(
                    ws,
                    {"type": "ready", "model": brain.model, "asr": asr.available,
                     "asr_error": asr.error},
                )
            else:
                await send_json(ws, {"type": "error", "message": f"unknown type {kind!r}"})

    except websockets.exceptions.ConnectionClosed:
        LOG.info("[%s] disconnected", peer)
    except Exception:  # noqa: BLE001
        LOG.exception("[%s] handler crashed", peer)


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description="LAMP-1 LAN voice bridge (Vosk + Ollama)")
    parser.add_argument("--host", default="0.0.0.0", help="bind address (default: all interfaces)")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--model", default="qwen3.5:9b", help="Ollama model name")
    parser.add_argument("--ollama", default="http://127.0.0.1:11434", help="Ollama base URL")
    parser.add_argument("--vosk-model", default=str(DEFAULT_VOSK_MODEL), help="Vosk model folder")
    parser.add_argument("--max-seconds", type=int, default=15, help="reject longer utterances")
    parser.add_argument("--timeout", type=int, default=120, help="LLM timeout in seconds")
    parser.add_argument("--no-asr", action="store_true", help="text-only mode (skip Vosk)")
    parser.add_argument("--check", action="store_true", help="verify ASR + Ollama, then exit")
    return parser.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")

    brain = Brain(args.ollama, args.model, args.timeout)
    asr = DisabledAsr() if args.no_asr else Asr(Path(args.vosk_model))
    max_audio_bytes = args.max_seconds * SAMPLE_RATE * 2

    if args.check:
        brain_ok = brain.health()
        print(f"Ollama : {'ok' if brain_ok else 'FAILED'}  {args.ollama} model={args.model}")
        print(f"Vosk   : {'ok' if asr.available else 'FAILED'}  {getattr(asr, 'error', '') or 'loaded'}")
        return 0 if (brain_ok and asr.available) else 1

    brain.health()

    async def runner() -> None:
        async with websockets.serve(
            lambda ws: handle_connection(ws, asr, brain, max_audio_bytes),
            args.host,
            args.port,
            max_size=2 * 1024 * 1024,
            ping_interval=20,
            ping_timeout=20,
        ):
            LOG.info("listening on ws://%s:%d", args.host, args.port)
            LOG.info("model=%s asr=%s", args.model, asr.available)
            LOG.info("point LAMP_VOICE_SERVER_HOST at this PC's LAN IP (see ipconfig)")
            await asyncio.Future()

    try:
        asyncio.run(runner())
    except KeyboardInterrupt:
        LOG.info("stopped")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())