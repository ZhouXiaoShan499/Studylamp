#!/usr/bin/env python3
"""
make_lamp1_assets.py - turn docs/design/lamp1.png into the site's image set.

The new render is a near-square 1312x1199 image. The site needs five different
frames (4:5, 16:10, 4:3, 2:1 and 16:9-ish for social). Instead of letting the
browser crop the robot's halo or base, this script composes each frame:

  * 4:5 portrait  -> scale by height, crop the sides
  * 16:10 / OG    -> sharp robot in the centre, blurred background extension
  * 4:3 / 2:1     -> local crops for head / base detail cards
  * OG            -> wide composition with the wordmark and target price

Run from the repository root:

    python tools/make_lamp1_assets.py

The source PNG stays in docs/design/; only the generated WebP/JPG files are
served from web/assets/.
"""

from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw, ImageEnhance, ImageFilter, ImageFont, ImageOps


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "docs" / "design" / "lamp1.png"
OUT = ROOT / "web" / "assets"


def load_source() -> Image.Image:
    image = Image.open(SRC).convert("RGB")
    # A light warm grade keeps the new render in the same amber family as the site.
    warm = Image.new("RGB", image.size, (255, 205, 160))
    image = Image.blend(image, warm, 0.035)
    image = ImageEnhance.Color(image).enhance(1.05)
    image = ImageEnhance.Contrast(image).enhance(1.02)
    return image


def save_webp(image: Image.Image, name: str, quality: int = 86) -> None:
    path = OUT / name
    image.save(path, format="WEBP", quality=quality, method=6)
    print(f"  {name:36s} {image.width:4d}x{image.height:<4d} {path.stat().st_size / 1024:6.1f} KB")


def save_jpg(image: Image.Image, name: str, quality: int = 88) -> None:
    path = OUT / name
    image.save(path, format="JPEG", quality=quality, optimize=True, progressive=True)
    print(f"  {name:36s} {image.width:4d}x{image.height:<4d} {path.stat().st_size / 1024:6.1f} KB")


def portrait(image: Image.Image, width: int, height: int) -> Image.Image:
    """Scale to the target height, then crop the sides. Keeps the full robot."""
    scaled = image.resize((round(image.width * height / image.height), height), Image.Resampling.LANCZOS)
    left = max(0, (scaled.width - width) // 2)
    return scaled.crop((left, 0, left + width, height))


def crop_box(image: Image.Image, box: tuple[int, int, int, int], width: int, height: int) -> Image.Image:
    cropped = image.crop(box).resize((width, height), Image.Resampling.LANCZOS)
    return cropped.filter(ImageFilter.UnsharpMask(radius=1.2, percent=70, threshold=3))


def wide_compose(image: Image.Image, width: int, height: int, feather: int = 72) -> Image.Image:
    """Sharp robot in the middle, blurred room extension on the sides."""
    background = ImageOps.fit(image, (width, height), method=Image.Resampling.LANCZOS, centering=(0.5, 0.45))
    background = background.filter(ImageFilter.GaussianBlur(34))
    background = ImageEnhance.Brightness(background).enhance(0.72)
    background = ImageEnhance.Color(background).enhance(0.86)

    foreground = image.resize((round(image.width * height / image.height), height), Image.Resampling.LANCZOS)
    x = (width - foreground.width) // 2

    mask = Image.new("L", foreground.size, 0)
    draw = ImageDraw.Draw(mask)
    draw.rectangle([feather, 0, foreground.width - feather, foreground.height], fill=255)
    mask = mask.filter(ImageFilter.GaussianBlur(feather / 2))

    canvas = background.copy()
    canvas.paste(foreground, (x, 0), mask)
    return canvas


def with_vignette(image: Image.Image, strength: float = 0.18) -> Image.Image:
    width, height = image.size
    mask = Image.new("L", (width, height), 0)
    draw = ImageDraw.Draw(mask)
    draw.ellipse([-width * 0.15, -height * 0.2, width * 1.15, height * 1.2], fill=255)
    mask = mask.filter(ImageFilter.GaussianBlur(min(width, height) // 5))
    dark = ImageEnhance.Brightness(image).enhance(1 - strength)
    return Image.composite(image, dark, mask)


def font(path: str, size: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(path, size)


def og_compose(image: Image.Image) -> Image.Image:
    width, height = 1200, 630
    canvas = wide_compose(image, width, height, feather=56).convert("RGB")

    # Left-side dark gradient for the wordmark and price.
    gradient = Image.new("L", (width, height), 0)
    gd = ImageDraw.Draw(gradient)
    for x in range(780):
        alpha = int(246 * (1 - x / 780) ** 1.05)
        gd.line([(x, 0), (x, height)], fill=alpha)
    black = Image.new("RGB", (width, height), (8, 9, 7))
    canvas = Image.composite(black, canvas, gradient)

    draw = ImageDraw.Draw(canvas)
    mono = "C:/Windows/Fonts/consola.ttf"
    sans = "C:/Windows/Fonts/msyh.ttc"
    sans_bold = "C:/Windows/Fonts/msyhbd.ttc"

    draw.text((64, 122), "LAMP—1", font=font(mono, 46), fill=(242, 237, 226))
    draw.text((64, 188), "桌面 AI 机器人", font=font(sans, 32), fill=(207, 200, 185))

    draw.line([(66, 252), (210, 252)], fill=(229, 183, 118), width=2)
    draw.text((64, 280), "目标零售价", font=font(sans, 20), fill=(179, 177, 165))
    draw.text((64, 314), "¥599", font=font(sans_bold, 54), fill=(229, 183, 118))
    draw.text((64, 392), "会听 · 会答 · 会动", font=font(sans, 19), fill=(207, 200, 185))
    draw.text((64, 438), "含主机、电源适配器", font=font(sans, 13), fill=(207, 200, 185))
    draw.text((64, 461), "快速指南、首年云服务", font=font(sans, 13), fill=(207, 200, 185))

    return with_vignette(canvas, 0.16)


def main() -> None:
    if not SRC.exists():
        raise SystemExit(f"missing source image: {SRC}")

    OUT.mkdir(parents=True, exist_ok=True)
    image = load_source()
    print(f"source: {image.width}x{image.height}")

    # 4:5 portrait: index hero and product structure Fig.01.
    save_webp(portrait(image, 1200, 1500), "product-hero-lamp1.webp", 87)

    # 16:10: product page hero and index simulator CTA.
    save_webp(wide_compose(image, 1600, 1000), "product-wide-lamp1.webp", 87)

    # 4:5 head / upper-arm crop: motion, technology hero, product end.
    head = crop_box(image, (380, 0, 1060, 850), 1024, 1280)
    save_webp(head, "head-lamp1.webp", 87)

    # 4:3 detail cards: head and base.
    save_webp(crop_box(image, (380, 0, 1100, 540), 1200, 900), "head-detail-lamp1.webp", 88)
    save_webp(crop_box(image, (380, 540, 1100, 1080), 1200, 900), "base-detail-lamp1.webp", 88)

    # 2:1 control card: base + arms.
    save_webp(crop_box(image, (300, 600, 1200, 1050), 1000, 500), "base-lamp1.webp", 88)

    # Social card.
    save_jpg(og_compose(image), "og-lamp1.jpg", 88)

    # README hero image (same filename as before, so the README keeps working).
    save_jpg(wide_compose(image, 1536, 966, feather=80), "scene-desk.jpg", 88)


if __name__ == "__main__":
    main()
