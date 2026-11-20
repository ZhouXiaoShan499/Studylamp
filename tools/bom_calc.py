#!/usr/bin/env python3
"""
bom_calc.py - BOM calculator for the current LAMP-1 desktop AI robot.

Why this exists
---------------
`tools/mvp_calc.py` still models the cancelled "smart desk lamp" (LED board,
IR receiver, OLED, SG90/MG996R). Those numbers are kept for history, but they
no longer describe the product. This script models the current build:

  * a bench view  - what is actually on the desk in 2026-11
  * phase 1       - electronics for listen / speak / move
  * phase 2       - the full unit: camera, status ring, shell, packaging

This is an internal planning tool. It is intentionally not wired into the
public website: the consumer site shows a retail price, not a bill of
materials. The same numbers are printed here so a docs number can be traced
back to one command:

    python tools/bom_calc.py
    python tools/bom_calc.py --json       # full data for a spreadsheet / quote

Price basis: CNY, excluding tax, shipping, tooling and certification. Product
lines use a 500 pcs small-batch reference price; the calculator applies 1.35x
for a single sample and 0.82x for 5,000 pcs. Bench lines use single-unit
retail prices and are not scaled.
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass


UPDATED = "2026-11-19"


@dataclass(frozen=True)
class Line:
    id: str
    views: tuple[str, ...]
    group: str
    name: str
    spec: str
    qty: int
    price: float
    status: str
    status_key: str
    note: str = ""


VIEWS = [
    {
        "id": "bench",
        "name": "台架",
        "en": "BENCH",
        "meta": "2026-11 现状 · 单件现货价",
        "basis": "bench",
        "note": "台架按单件现货价计；CAM12 与调试模块不计入整机 BOM。不含 CAM12 的台架电子物料约 ¥102.50。",
    },
    {
        "id": "phase1",
        "name": "阶段一",
        "en": "PHASE 1",
        "meta": "能听、能说、会动 · 电子物料",
        "basis": "product",
        "note": "阶段一不含外壳、灯环与摄像头；结构仍以 3D 打印手板验证，手板费用计入 NRE，不计入单台 BOM。",
    },
    {
        "id": "phase2",
        "name": "阶段二",
        "en": "PHASE 2",
        "meta": "视觉 + 灯环 + 整机 · 整机 BOM",
        "basis": "product",
        "note": "整机口径含外壳、灯环与摄像头；模具、认证与打样单列 NRE，不计入单台 BOM。",
    },
]

GROUPS = [
    {"id": "compute", "name": "计算与存储", "en": "COMPUTE"},
    {"id": "audio", "name": "音频", "en": "AUDIO"},
    {"id": "display", "name": "显示与交互", "en": "DISPLAY / UI"},
    {"id": "motion", "name": "运动", "en": "MOTION"},
    {"id": "vision", "name": "视觉", "en": "VISION"},
    {"id": "light", "name": "灯环", "en": "STATUS RING"},
    {"id": "power", "name": "电源", "en": "POWER"},
    {"id": "structure", "name": "结构", "en": "STRUCTURE"},
    {"id": "pack", "name": "包装与装配", "en": "PACK / ASSEMBLY"},
]

# Price factors keyed by basis and volume. Bench is already a single-unit price.
FACTORS = {
    "bench": {"1": 1.0},
    "product": {"1": 1.35, "500": 1.0, "5000": 0.82},
}

# --------------------------------------------------------------------------------------
# Bench: the hardware that is actually in use now. Never mixed with the product BOM.
# --------------------------------------------------------------------------------------
LINES: list[Line] = [
    Line(
        "bench-mcu",
        ("bench",),
        "compute",
        "ESP32-S3-DEVKITC-1 N16R8 开发板",
        "16 MB Flash / 8 MB PSRAM；当前台架主控",
        1,
        48.0,
        "在用",
        "now",
        "整机换成 WROOM 模组，开发板不进入产品 BOM",
    ),
    Line(
        "bench-mic",
        ("bench",),
        "audio",
        "INMP441 I2S MEMS 麦克风",
        "32-bit 槽宽；BCLK 16 / WS 18 / SD 17",
        1,
        9.0,
        "在用",
        "now",
        "阶段二由 CAM12 UAC 或板载麦替代",
    ),
    Line(
        "bench-btn",
        ("bench",),
        "display",
        "HW-483 按键模块",
        "GPIO4，低电平有效",
        1,
        2.5,
        "在用",
        "now",
        "实物触发待复测",
    ),
    Line(
        "bench-led",
        ("bench",),
        "display",
        "HW-479 调试负载（仅 G 通道）",
        "GPIO5；只做台架状态输出",
        1,
        2.0,
        "在用",
        "now",
        "调试件，明确不计入产品 BOM",
    ),
    Line(
        "bench-cam",
        ("bench",),
        "vision",
        "CAM12 USB 摄像头（UVC + UAC）",
        "12MP；P0 验证枚举 / 出图 / 收音 / 放音",
        1,
        78.0,
        "待枚举",
        "wait",
        "P0 尚未通过，不能按可用件计",
    ),
    Line(
        "bench-wires",
        ("bench",),
        "structure",
        "面包板 + 杜邦线",
        "台架接线与调试",
        1,
        15.0,
        "在用",
        "now",
        "非产品 BOM",
    ),
    Line(
        "bench-psu",
        ("bench",),
        "power",
        "5V/2A 电源适配器 + USB 线",
        "台架供电；摄像头约 190 mA",
        1,
        15.0,
        "在用",
        "now",
        "整机改用 5V/4A",
    ),
    Line(
        "bench-otg",
        ("bench",),
        "power",
        "USB-C OTG 转接 + USB-A 母座",
        "摄像头接板载 USB 口",
        1,
        9.0,
        "待用",
        "wait",
        "P0 接线件",
    ),
    Line(
        "bench-cap",
        ("bench",),
        "power",
        "470 µF + 100 nF 去耦",
        "摄像头 5V 电源脚",
        1,
        2.0,
        "待用",
        "wait",
        "P0 接线件",
    ),
    # ----------------------------------------------------------------------------------
    # Phase 1: product electronics. No shell, camera or ring yet.
    # ----------------------------------------------------------------------------------
    Line(
        "p1-mcu",
        ("phase1", "phase2"),
        "compute",
        "ESP32-S3-WROOM-1-N16R8 模组",
        "Wi-Fi / BLE；16 MB Flash / 8 MB PSRAM",
        1,
        26.0,
        "目标",
        "goal",
        "整机主控，替代台架开发板",
    ),
    Line(
        "p1-pcb",
        ("phase1", "phase2"),
        "compute",
        "主控载板 PCB（4 层）",
        "60 × 45 mm；500 台摊薄",
        1,
        8.0,
        "目标",
        "goal",
        "不含贴片工时",
    ),
    Line(
        "p1-mic",
        ("phase1", "phase2"),
        "audio",
        "板载 MEMS 麦克风 + 前置放大",
        "I2S / PDM；近场 1 m",
        1,
        5.0,
        "待选型",
        "wait",
        "替代台架 INMP441",
    ),
    Line(
        "p1-amp",
        ("phase1", "phase2"),
        "audio",
        "MAX98357A I2S 功放",
        "3.2 W 单声道；I2S 输入",
        1,
        9.0,
        "目标",
        "goal",
    ),
    Line(
        "p1-spk",
        ("phase1", "phase2"),
        "audio",
        "8 Ω 2 W 扬声器",
        "40 mm；回答播报",
        1,
        6.0,
        "目标",
        "goal",
    ),
    Line(
        "p1-tft",
        ("phase1", "phase2"),
        "display",
        "1.09″ TFT 240 × 240",
        "SPI；表情 / 计时 / 状态",
        1,
        26.0,
        "目标",
        "goal",
    ),
    Line(
        "p1-key",
        ("phase1", "phase2"),
        "display",
        "轻触按键 × 2 + 键帽",
        "对话 / 计时；整机目标 3 键",
        2,
        1.0,
        "目标",
        "goal",
        "静音键在阶段二补",
    ),
    Line(
        "p1-rgb",
        ("phase1", "phase2"),
        "display",
        "板载 RGB 状态灯",
        "灯环之前的过渡状态提示",
        1,
        1.5,
        "目标",
        "goal",
    ),
    Line(
        "p1-servo",
        ("phase1", "phase2"),
        "motion",
        "总线舵机 × 2",
        "转头 / 点头；UART 总线",
        2,
        55.0,
        "待报价",
        "wait",
        "暂按 55 元/个，实际报价待定",
    ),
    Line(
        "p1-bracket",
        ("phase1", "phase2"),
        "motion",
        "舵机支架 + 轴承组",
        "金属支架；俯仰 / 偏航",
        1,
        15.0,
        "待打样",
        "wait",
    ),
    Line(
        "p1-dist",
        ("phase1", "phase2"),
        "motion",
        "舵机电源分配板",
        "5V/3A；与主控共地",
        1,
        8.0,
        "待打样",
        "wait",
    ),
    Line(
        "p1-psu",
        ("phase1", "phase2"),
        "power",
        "5V/4A 电源适配器",
        "CCC；整机与舵机共用",
        1,
        18.0,
        "目标",
        "goal",
    ),
    Line(
        "p1-jack",
        ("phase1", "phase2"),
        "power",
        "DC 座 + 电源开关",
        "5.5 × 2.1 mm",
        1,
        3.0,
        "目标",
        "goal",
    ),
    Line(
        "p1-passive",
        ("phase1", "phase2"),
        "power",
        "无源器件 / 连接器 / 线束",
        "电阻、电容、JST、排线",
        1,
        8.0,
        "目标",
        "goal",
    ),
    Line(
        "p1-assy",
        ("phase1", "phase2"),
        "pack",
        "装配 + 功能测试",
        "约 16 分钟 / 台（60 元/时）",
        1,
        12.0,
        "目标",
        "goal",
    ),
    # ----------------------------------------------------------------------------------
    # Phase 2: camera, status ring, shell and packaging.
    # ----------------------------------------------------------------------------------
    Line(
        "p2-cam",
        ("phase2",),
        "vision",
        "GC0308 摄像头模组",
        "DVP / 640 × 480；基础在场检测",
        1,
        18.0,
        "阶段二",
        "plan",
        "备选：2MP UVC 摄像头",
    ),
    Line(
        "p2-flex",
        ("phase2",),
        "vision",
        "摄像头 FPC + 连接器",
        "屏蔽排线；固定与走线",
        1,
        3.0,
        "阶段二",
        "plan",
    ),
    Line(
        "p2-key",
        ("phase2",),
        "display",
        "静音按键 + 键帽",
        "第三个按键；长按关闭播报与动作",
        1,
        1.0,
        "阶段二",
        "plan",
    ),
    Line(
        "p2-ring",
        ("phase2",),
        "light",
        "WS2812B 48 mm 灯环",
        "状态反馈；不做照明",
        1,
        12.0,
        "阶段二",
        "plan",
        "按 24 颗灯珠估",
    ),
    Line(
        "p2-guide",
        ("phase2",),
        "light",
        "导光环 PMMA 扩散件",
        "48 mm；柔光",
        1,
        6.0,
        "阶段二",
        "plan",
    ),
    Line(
        "p2-window",
        ("phase2",),
        "structure",
        "凸面视窗（亚克力）",
        "Ø28；低于外壳边缘",
        1,
        8.0,
        "阶段二",
        "plan",
    ),
    Line(
        "p2-shell",
        ("phase2",),
        "structure",
        "外壳（ABS 注塑，3 件）",
        "底座 / 立柱 / 头壳；模具单列",
        1,
        22.0,
        "阶段二",
        "plan",
        "500 台摊薄",
    ),
    Line(
        "p2-weight",
        ("phase2",),
        "structure",
        "底座配重钢板",
        "降低重心；碰不倒",
        1,
        5.0,
        "阶段二",
        "plan",
    ),
    Line(
        "p2-joint",
        ("phase2",),
        "structure",
        "摩擦关节 / 轴承 / 五金",
        "中段调角；2 轴运动",
        1,
        12.0,
        "阶段二",
        "plan",
    ),
    Line(
        "p2-fastener",
        ("phase2",),
        "structure",
        "螺丝 / 硅胶脚垫",
        "M2 / M3 套件",
        1,
        3.0,
        "阶段二",
        "plan",
    ),
    Line(
        "p2-pack",
        ("phase2",),
        "pack",
        "包装（盒 + 内衬 + 说明卡）",
        "运输防护；不开说明书也能用",
        1,
        8.0,
        "阶段二",
        "plan",
    ),
    Line(
        "p2-assy",
        ("phase2",),
        "pack",
        "整机装配增量",
        "外壳 / 灯环 / 摄像头装配",
        1,
        4.0,
        "阶段二",
        "plan",
    ),
]

NRE = [
    {
        "id": "nre-mold",
        "name": "注塑模具（外壳 3 件）",
        "amount": 45000.0,
        "note": "简化模；实际报价待定",
        "views": ["phase2"],
    },
    {
        "id": "nre-cert",
        "name": "CCC / CE 认证",
        "amount": 35000.0,
        "note": "含测试与整改预留",
        "views": ["phase2"],
    },
    {
        "id": "nre-pcb",
        "name": "PCB 打样 + 钢网",
        "amount": 3000.0,
        "note": "两轮打样",
        "views": ["phase1", "phase2"],
    },
    {
        "id": "nre-pack",
        "name": "包装打样",
        "amount": 2000.0,
        "note": "盒 / 内衬 / 说明卡",
        "views": ["phase2"],
    },
    {
        "id": "nre-print",
        "name": "3D 打印手板",
        "amount": 1500.0,
        "note": "3 轮结构验证",
        "views": ["phase1", "phase2"],
    },
]

CLOUD = {
    "perUse": 0.01,
    "usesPerDay": 10,
    "daysPerYear": 365,
    "perYear": 36.5,
    "note": "ASR + LLM + TTS 的量级成本；每天 10 次",
}


# --------------------------------------------------------------------------------------
# Calculations
# --------------------------------------------------------------------------------------
def lines_for(view: str) -> list[Line]:
    return [line for line in LINES if view in line.views]


def basis_for(view: str) -> str:
    for item in VIEWS:
        if item["id"] == view:
            return str(item["basis"])
    raise KeyError(view)


def factor_for(view: str, volume: str) -> float:
    return FACTORS[basis_for(view)][volume]


def half_up(value: float) -> float:
    """Round to cents half-up, matching how the numbers are quoted."""
    return int(value * 100 + 0.5 + 1e-9) / 100


def raw_total(view: str) -> float:
    return half_up(sum(line.qty * line.price for line in lines_for(view)))


def view_total(view: str, volume: str) -> float:
    return half_up(raw_total(view) * factor_for(view, volume))


def payload() -> dict:
    return {
        "updated": UPDATED,
        "currency": "CNY",
        "factors": FACTORS,
        "views": VIEWS,
        "groups": GROUPS,
        "lines": [
            {
                "id": line.id,
                "views": list(line.views),
                "group": line.group,
                "name": line.name,
                "spec": line.spec,
                "qty": line.qty,
                "price": line.price,
                "status": line.status,
                "statusKey": line.status_key,
                "note": line.note,
            }
            for line in LINES
        ],
        "nre": {
            "items": NRE,
            "total": round(sum(item["amount"] for item in NRE), 2),
        },
        "cloud": CLOUD,
    }


def print_summary() -> None:
    print("== LAMP-1 BOM (CNY, tax / shipping / NRE excluded) ==")
    for view in VIEWS:
        lines = lines_for(view["id"])
        qty = sum(line.qty for line in lines)
        total = view_total(view["id"], "500" if view["basis"] == "product" else "1")
        basis = "500 pcs" if view["basis"] == "product" else "1 pc"
        print(f"  {view['name']:4s} ({view['en']:7s}) {len(lines):2d} lines / {qty:2d} pcs  CNY {total:8.2f}  [{basis}]")

    print("\n  volume sensitivity (product views):")
    for volume in ("1", "500", "5000"):
        totals = [view_total(item["id"], volume) for item in VIEWS if item["basis"] == "product"]
        print(f"    {volume:>4s} pcs: " + "  ".join(f"{total:7.2f}" for total in totals))

    nre_total = sum(item["amount"] for item in NRE)
    print(f"\n  NRE total (all phases): CNY {nre_total:,.2f}")
    for view in VIEWS:
        if view["basis"] != "product":
            continue
        view_nre = sum(item["amount"] for item in NRE if view["id"] in item["views"])
        print(f"    {view['name']:4s}: CNY {view_nre:,.2f}")
    for volume in (500, 5000):
        print(f"    {volume:4d} pcs -> CNY {nre_total / volume:7.2f}/unit (all phases)")
    print(f"  cloud: approx. CNY {CLOUD['perYear']:.1f}/year/unit at {CLOUD['usesPerDay']} uses/day")


def main() -> None:
    parser = argparse.ArgumentParser(description="LAMP-1 internal BOM calculator")
    parser.add_argument("--json", action="store_true", help="print the full data as JSON")
    args = parser.parse_args()

    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")

    print_summary()

    if args.json:
        print("\n== JSON ==")
        print(json.dumps(payload(), ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
