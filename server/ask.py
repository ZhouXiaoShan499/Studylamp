#!/usr/bin/env python3
"""Ask the local DeepSeek model from the terminal (voice task 2).

    python ask.py "今天适合复习吗"
    python ask.py --stream            # interactive, streams tokens
    python ask.py --model qwen3.5:4b "..."

Requires Ollama running on http://127.0.0.1:11434 with the model pulled:
    ollama pull qwen3.5:4b         # default: 4B, warm reply under 1 s
    ollama pull deepseek-r1:1.5b   # fast fallback: under 1 s, weaker answers
"""

from __future__ import annotations

import argparse
import json
import sys

import requests

SYSTEM_PROMPT = (
    "你是「小灯」，一盏桌面智能台灯的语音助手。"
    "回答要口语化、亲切、简短：最多三句话，总字数不超过 80 字。"
    "不要使用 Markdown、列表或表情符号，因为答案会被直接念出来。"
)

# Assemble the reasoning tags at runtime: writing them literally in source can
# be swallowed by editors/terminals that treat "<...>" as markup.
THINK_TAGS = (
    ("<" + "think" + ">", "</" + "think" + ">"),
    ("<" + "thinking" + ">", "</" + "thinking" + ">"),
)


def split_leading_think(raw: str) -> tuple[str, str]:
    """Classify a stream prefix: ('wait'|'thinking'|'answer', visible text)."""
    stripped = raw.lstrip()
    for open_tag, close_tag in THINK_TAGS:
        if stripped.startswith(open_tag):
            end = raw.find(close_tag)
            if end < 0:
                return "thinking", ""
            return "answer", raw[end + len(close_tag):]
        # Still too short to decide whether this is the special token.
        if len(stripped) < len(open_tag) and open_tag.startswith(stripped):
            return "wait", ""
    return "answer", raw


def strip_think(raw: str) -> str:
    """Drop leading reasoning blocks; R1 writes them even with think=False."""
    text = raw
    for open_tag, close_tag in THINK_TAGS:
        while open_tag in text:
            start = text.find(open_tag)
            end = text.find(close_tag, start)
            if end < 0:
                text = text[:start]
                break
            text = text[:start] + text[end + len(close_tag):]
    return text.strip()


def ask(host: str, model: str, prompt: str, stream: bool, timeout: int) -> str:
    payload = {
        "model": model,
        "prompt": prompt,
        "system": SYSTEM_PROMPT,
        "stream": stream,
        "think": False,
        # 1.5B 的思考链能吃满 220 个 token（实测 done_reason=length），正文会为空。
        "options": {"temperature": 0.7, "num_predict": 768},
    }
    if not stream:
        response = requests.post(f"{host}/api/generate", json=payload, timeout=timeout)
        response.raise_for_status()
        return strip_think(str(response.json().get("response", "")))

    raw = ""
    answer = ""
    emitted = 0
    state = "wait"
    with requests.post(
        f"{host}/api/generate", json=payload, timeout=timeout, stream=True
    ) as response:
        response.raise_for_status()
        for line in response.iter_lines(decode_unicode=True):
            if not line:
                continue
            try:
                chunk = json.loads(line)
            except json.JSONDecodeError:
                continue
            piece = chunk.get("response", "")
            raw += piece

            # DeepSeek-R1 sometimes emits its reasoning even with think=False;
            # show only the final answer. The user asked for a lamp, not a diary.
            if state == "wait":
                state, visible = split_leading_think(raw)
                if state == "wait" or state == "thinking":
                    answer = ""
                else:
                    answer = visible
            elif state == "thinking":
                for _open, close in THINK_TAGS:
                    end = raw.find(close)
                    if end >= 0:
                        answer = raw[end + len(close):]
                        state = "answer"
                        break
            else:
                answer += piece

            if state == "answer" and len(answer) > emitted:
                print(answer[emitted:], end="", flush=True)
                emitted = len(answer)

            if chunk.get("done"):
                break
    print()
    return answer.strip()


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Ask local DeepSeek through Ollama")
    parser.add_argument("prompt", nargs="*", help="question; omit for interactive mode")
    parser.add_argument("--model", default="qwen3.5:4b")
    parser.add_argument("--host", default="http://127.0.0.1:11434")
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--stream", action="store_true", help="stream tokens as they are generated")
    args = parser.parse_args(argv)

    text = " ".join(args.prompt).strip()
    if text:
        answer = ask(args.host, args.model, text, args.stream, args.timeout)
        if not args.stream:
            print(answer)
        return 0

    print(f"model={args.model} host={args.host}  (Ctrl+C to quit)")
    try:
        while True:
            question = input("you> ").strip()
            if not question:
                continue
            print("ai > ", end="", flush=True)
            ask(args.host, args.model, question, True, args.timeout)
    except (EOFError, KeyboardInterrupt):
        print()
        return 0


if __name__ == "__main__":
    sys.exit(main())