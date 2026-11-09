#!/usr/bin/env python3
"""阿里云百炼语音链路自检：模拟固件 provider=3 的两次请求。

固件里 cloud_asr.cpp 干的事就是这两步，脚本按同样的字节顺序发一遍：
  1) WAV(44 字节头 + 16k 单声道 PCM) -> base64 -> qwen3-asr-flash -> 文本
  2) 文本 -> deepseek-v4.1-flash（同一个 key、同一个域名）-> 一个动作标签

脚本跑通 = Key、地域域名、模型名、请求体格式四件事同时对，固件只剩网络问题。

用法：
    export DASHSCOPE_API_KEY=sk-xxx
    python tools/bailian_voice_check.py                       # 默认用仓库里的开灯录音
    python tools/bailian_voice_check.py --wav my.wav
    python tools/bailian_voice_check.py --asr-only            # 只验第一跳
    python tools/bailian_voice_check.py --host xxx.cn-beijing.maas.aliyuncs.com
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import sys
import time
import urllib.error
import urllib.request
import wave

DEFAULT_HOST = "dashscope.aliyuncs.com"
ASR_PATH = "/compatible-mode/v1/chat/completions"

SYSTEM_PROMPT = (
    "你是台灯的控制中枢。根据用户这句话，只回一个英文单词，不要解释、不要标点："
    "开灯=on，关灯=off，调亮=up，调暗=down，调到最亮=max，夜灯=night，"
    "学习/看书模式=study，休息/睡眠模式=rest，其他无关的话=none。"
)


def post_json(url: str, key: str, payload: dict, timeout: float = 30.0) -> dict:
    body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=body,
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {key}",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", "replace")
        hint = ""
        if e.code in (401, 403):
            hint = (
                "\n提示：Key 与地域不匹配（北京/新加坡的 Key 不通用），"
                "或这个 Key 没有开百炼权限。"
            )
        elif e.code == 404:
            hint = (
                "\n提示：域名或模型名不对。新控制台要换成 "
                "https://<WorkspaceId>.cn-beijing.maas.aliyuncs.com。"
            )
        raise SystemExit(f"[失败] HTTP {e.code}{hint}\n{detail[:600]}") from None
    except urllib.error.URLError as e:
        raise SystemExit(f"[失败] 连不上 {url}：{e.reason}") from None


def load_wav_info(path: str) -> dict:
    with wave.open(path, "rb") as w:
        info = {
            "channels": w.getnchannels(),
            "rate": w.getframerate(),
            "width": w.getsampwidth(),
            "seconds": round(w.getnframes() / w.getframerate(), 2),
        }
    if info["rate"] != 16000 or info["channels"] != 1 or info["width"] != 2:
        raise SystemExit(
            f"[失败] {path} 是 {info['rate']}Hz/{info['channels']}声道/"
            f"{info['width'] * 8}bit，固件只送 16kHz/单声道/16bit"
        )
    return info


def main() -> int:
    ap = argparse.ArgumentParser(description="百炼语音链路自检（对应固件 provider=3）")
    ap.add_argument("--key", default=os.environ.get("DASHSCOPE_API_KEY", ""))
    ap.add_argument("--wav", default=os.path.join("server", "kaitest.wav"))
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--asr-model", default="qwen3-asr-flash")
    ap.add_argument("--llm-model", default="deepseek-v4.1-flash")
    ap.add_argument("--asr-only", action="store_true", help="只跑语音转文字，不打标签")
    ap.add_argument("--dry-run", action="store_true", help="只打印请求体，不发请求")
    args = ap.parse_args()

    if args.dry_run and not args.key:
        args.key = "sk-dryrun"

    if not args.key:
        print("[失败] 没有 Key：export DASHSCOPE_API_KEY=sk-xxx 或传 --key")
        return 2
    if not os.path.exists(args.wav):
        print(f"[失败] 找不到音频 {args.wav}")
        return 2

    info = load_wav_info(args.wav)
    with open(args.wav, "rb") as f:
        wav_bytes = f.read()
    wav_b64 = base64.b64encode(wav_bytes).decode("ascii")
    print(f"[1/2] 音频 {info['seconds']} s，WAV {len(wav_bytes)} B，"
          f"base64 后 {len(wav_b64)} B")

    url = f"https://{args.host}{ASR_PATH}"
    asr_body = {
        "model": args.asr_model,
        "stream": False,
        "asr_options": {"language": "zh", "enable_itn": False},
        "messages": [
            {
                "role": "user",
                "content": [
                    {"type": "input_audio",
                     "input_audio": {"data": f"data:audio/wav;base64,{wav_b64}"}}
                ],
            }
        ],
    }

    t0 = time.time()
    if args.dry_run:
        preview = dict(asr_body)
        preview["messages"] = [
            {"role": "user",
             "content": [{"type": "input_audio",
                          "input_audio": {"data": f"data:audio/wav;base64,<{len(wav_b64)} B>"}}]}
        ]
        print(json.dumps({"url": url, "body": preview}, ensure_ascii=False, indent=2))
        return 0
    resp = post_json(url, args.key, asr_body)
    if resp.get("error"):
        err = resp["error"]
        print(f"[失败] ASR：{err.get('message') or err}")
        print("提示：模型名写错会报 model not found；先在控制台确认能看到 qwen3-asr-flash。")
        return 1
    text = (resp.get("choices") or [{}])[0].get("message", {}).get("content", "")
    if isinstance(text, list):
        text = "".join(part.get("text", "") for part in text)
    text = (text or "").strip()
    print(f"[1/2] {time.time() - t0:.2f} s 识别结果：\"{text}\"")
    if not text:
        print("[失败] 识别为空：麦克风太轻或这段录音确实没说话")
        return 1
    if args.asr_only:
        return 0

    t1 = time.time()
    llm_body = {
        "model": args.llm_model,
        "messages": [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": text},
        ],
        "temperature": 0,
        "max_tokens": 8,
    }
    resp2 = post_json(url, args.key, llm_body)
    if resp2.get("error"):
        err = resp2["error"]
        print(f"[失败] LLM：{err.get('message') or err}")
        print("提示：百炼的第三方模型名以控制台为准（如 deepseek-v4.1-flash，注意 .1），"
              "可先换 --llm-model qwen3.6-flash 试通链路。")
        return 1
    label = (resp2.get("choices") or [{}])[0].get("message", {}).get("content", "")
    label = (label or "").strip().strip("。.,，")
    print(f"[2/2] {time.time() - t1:.2f} s 动作标签：{label or '(空)'}")

    valid = {"on", "off", "up", "down", "max", "night", "study", "rest", "none"}
    if label not in valid:
        print(f"[注意] 返回的不是约定的 9 个标签之一，固件会当 none 处理")
        return 1
    print(f"[通过] 两次请求合计 {time.time() - t0:.2f} s，固件按这个结果就能点灯")
    return 0


if __name__ == "__main__":
    sys.exit(main())
