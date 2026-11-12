#!/usr/bin/env python3
"""云端 Key 体检：扫一遍仓库（含被 .gitignore 的本地文件），告诉你

  - 哪些文件里写了疑似云端 Key，各是哪一家；
  - 它在 git 里是被跟踪（会泄露）还是只在本机；
  - web/ 里有没有 Key（网站是公开产物，写进去就等于公开）。

用法：
    python tools/scan_cloud_keys.py               # 全仓体检
    python tools/scan_cloud_keys.py --web-only    # 只查 web/（CI 里的守卫用，命中即退出 1）
    python tools/scan_cloud_keys.py --strict      # 被跟踪文件里只要命中就退出 1
    python tools/scan_cloud_keys.py --quiet       # 只输出结论行

退出码：0 干净；1 有风险命中；2 只在本地未跟踪文件里命中（可接受，但要知道）。
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

SKIP_DIRS = {
    ".git", "venv", ".venv", ".pio", ".pioenvs", "managed_components",
    "build", "node_modules", "__pycache__", ".mypy_cache", "stash-backup",
}
TEXT_EXT = {
    ".md", ".py", ".c", ".cpp", ".h", ".hpp", ".cxx", ".ini", ".json", ".txt",
    ".yml", ".yaml", ".html", ".js", ".mjs", ".css", ".toml", ".cfg", ".bat",
    ".ps1", ".sh", ".csv", ".example", ".env", ".sdkconfig", ".defaults",
}

# 判定顺序：越具体的放前面
PATTERNS = [
    ("腾讯云 SecretId", re.compile(r"AKID[A-Za-z0-9]{16,}")),
    ("模型平台 API Key", re.compile(r"sk-[A-Za-z0-9]{32,}")),
    ("短 sk- Key", re.compile(r"sk-[A-Za-z0-9]{20,31}")),
    ("疑似 Key 赋值", re.compile(
        r"(?i)(?:secret[_-]?key|secret[_-]?id|api[_-]?key|access[_-]?key|token)\s*[:=]\s*[\"']?([A-Za-z0-9_\-]{20,})")),
]

# 文档/示例里允许出现的占位写法
PLACEHOLDER = re.compile(r"(?i)(your|example|placeholder|xxxx|<|填|占位|dummy|fake|test)")


def tracked_files() -> set[str]:
    try:
        out = subprocess.run(
            ["git", "ls-files", "-z"], cwd=ROOT, capture_output=True, check=False,
        ).stdout.decode("utf-8", "replace")
    except OSError:
        return set()
    return {p for p in out.split("\0") if p}


def iter_files(base: Path):
    for p in base.rglob("*"):
        if not p.is_file():
            continue
        rel_parts = p.relative_to(ROOT).parts
        if any(part in SKIP_DIRS for part in rel_parts):
            continue
        if p.suffix.lower() in TEXT_EXT or p.name.startswith(".env") or p.name.endswith(".env") \
                or p.name.endswith(".env.example"):
            yield p


def mask(value: str) -> str:
    if len(value) <= 10:
        return value[:2] + "***"
    return value[:8] + ".." + value[-2:]


def classify(line: str) -> list[tuple[str, str]]:
    """返回该行里不重叠的命中，长匹配优先，避免同一把 Key 被两个正则各报一次。"""
    spans = []
    for label, pattern in PATTERNS:
        for m in pattern.finditer(line):
            value = m.group(1) if m.groups() else m.group(0)
            if PLACEHOLDER.search(value) or PLACEHOLDER.search(line):
                continue
            # 宏名 / 变量名不是 Key：全大写带下划线的、LAMP_ 开头的都跳过
            if re.fullmatch(r"[A-Z][A-Z0-9_]{6,}", value) or value.startswith("LAMP_"):
                continue
            spans.append((m.start(), m.end(), label, value))
    spans.sort(key=lambda s: (s[0], -(s[1] - s[0])))
    chosen = []
    last_end = -1
    for start, end, label, value in spans:
        if start < last_end:
            continue
        chosen.append((label, mask(value)))
        last_end = end
    return chosen


def main() -> int:
    try:                                   # Windows 控制台默认 GBK，避免打不出符号
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

    ap = argparse.ArgumentParser(description="扫描仓库里的云端 Key")
    ap.add_argument("--web-only", action="store_true", help="只查 web/（网站公开产物）")
    ap.add_argument("--strict", action="store_true", help="被跟踪文件命中即失败")
    ap.add_argument("--quiet", action="store_true", help="只输出结论")
    args = ap.parse_args()

    base = ROOT / "web" if args.web_only else ROOT
    if not base.exists():
        print("找不到目录：%s" % base)
        return 1

    tracked = tracked_files()
    hits = []          # (rel, lineno, label, masked, tracked, snippet)
    for path in iter_files(base):
        rel = path.relative_to(ROOT).as_posix()
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
        for idx, line in enumerate(lines, 1):
            if len(line) > 4000:
                continue
            for label, masked in classify(line):
                hits.append((rel, idx, label, masked, rel in tracked, line.strip()[:100]))

    if not hits:
        print("干净：%s 下没有发现疑似 Key。" % ("web/" if args.web_only else "全仓"))
        return 0

    if not args.quiet:
        print("%-46s %-5s %-16s %-22s %s" % ("文件", "行", "判定", "值（打码）", "状态"))
        print("-" * 120)
        for rel, idx, label, masked, is_tracked, _snippet in hits:
            state = "git 已跟踪 !!" if is_tracked else "本地未跟踪"
            print("%-46s %-5d %-16s %-22s %s" % (rel, idx, label, masked, state))
        print("-" * 120)

    tracked_hits = [h for h in hits if h[4]]
    print("命中 %d 处：git 已跟踪 %d 处，仅本地 %d 处。"
          % (len(hits), len(tracked_hits), len(hits) - len(tracked_hits)))

    if args.web_only or args.strict:
        if tracked_hits or args.web_only:
            print("失败：公开产物 / 仓库里不该出现 Key。改成 .secrets/ 或环境变量。")
            return 1
    if tracked_hits:
        print("风险：被跟踪的文件里出现 Key，一旦 push 就等于公开。")
        return 1
    print("结论：命中的都只在本地未跟踪文件里（.gitignore 之外），不会被 push；注意别贴到 Issue/截图。")
    return 2


if __name__ == "__main__":
    sys.exit(main())
