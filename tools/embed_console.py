#!/usr/bin/env python3
"""把 web/device.html 打包成固件里的常量数组。

设备端页面不走 GitHub Pages：https 页面连不上 ws://（混合内容会被浏览器拦掉），
所以页面必须由设备自己在 http 上提供。这里把 HTML 压成 C 数组编进固件，
省掉 LittleFS 上传这一步，烧一次就带页面。

用法：
    python tools/embed_console.py          # 重新生成 firmware/include/console_html.h
    python tools/embed_console.py --check  # 只检查是否过期（CI 用，不写文件）

生成两份数据：原始 HTML 和 gzip 版本。浏览器带 Accept-Encoding: gzip 时发压缩的，
否则发原文，curl 这类不带头的客户端也能读到正常 HTML。
"""

from __future__ import annotations

import argparse
import gzip
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "web" / "device.html"
TARGET = ROOT / "firmware" / "include" / "console_html.h"


def read_source() -> bytes:
    """按字节读页面，并把 CRLF 归一成 LF。

    Windows 检出时 git 会按 `* text=auto` 把 device.html 转成 CRLF，
    而 Linux CI 上是 LF。若照原样打包，两边产出的数组差一个 0x0D，
    --check 在 CI 上必然失败（本地却通过，最难查的那种）。
    HTML 对 CRLF/LF 没有语义差别，统一成 LF 让产物与平台无关。
    """
    return SOURCE.read_bytes().replace(b"\r\n", b"\n")


def as_c_array(name: str, data: bytes) -> str:
    rows = [f"const uint8_t {name}[] PROGMEM = {{"]
    for offset in range(0, len(data), 16):
        chunk = data[offset : offset + 16]
        rows.append("    " + ",".join(f"0x{byte:02X}" for byte in chunk) + ",")
    rows.append("};")
    rows.append(f"const size_t {name}Len = {len(data)};")
    return "\n".join(rows)


def render(raw: bytes) -> str:
    # mtime=0 让同样的输入产出同样的字节，改动才能在 git diff 里看清。
    packed = gzip.compress(raw, 9, mtime=0)
    return (
        "#pragma once\n"
        "// 由 tools/embed_console.py 从 web/device.html 生成，不要手改。\n"
        "// 改完页面跑一次：python tools/embed_console.py\n"
        "#include <Arduino.h>\n"
        "\n"
        f"{as_c_array('kConsoleHtml', raw)}\n"
        "\n"
        f"{as_c_array('kConsoleHtmlGz', packed)}\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="只检查是否与页面一致")
    args = parser.parse_args()

    if not SOURCE.exists():
        print(f"missing page: {SOURCE}", file=sys.stderr)
        return 1

    raw = read_source()
    content = render(raw)

    if args.check:
        current = TARGET.read_text(encoding="utf-8") if TARGET.exists() else ""
        if current != content:
            print("console_html.h 已过期，请运行 python tools/embed_console.py", file=sys.stderr)
            return 1
        print(f"console_html.h up to date ({len(raw)} B html)")
        return 0

    # newline="\n"：Windows 上 write_text 默认会把 \n 转成 CRLF，
    # 写出来就和仓库里的 LF 版本不一致了。固定成 LF，本地与 CI 同一份字节。
    TARGET.write_text(content, encoding="utf-8", newline="\n")
    packed_len = len(gzip.compress(raw, 9, mtime=0))
    print(f"wrote {TARGET.relative_to(ROOT)}: html {len(raw)} B, gzip {packed_len} B")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
