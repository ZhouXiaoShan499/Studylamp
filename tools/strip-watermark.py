# -*- coding: utf-8 -*-
"""Corner watermark removal: 2D inverse-distance fill + blur + matched grain."""
import random
from PIL import Image, ImageFilter


def fill_region(im, x0, y0, blur=9, grain=1.6, feather=12):
    im = im.convert("RGB")
    w, h = im.size
    src = im.load()
    out = im.copy()
    px = out.load()

    x0 = max(0, x0)
    y0 = max(0, y0)
    # clean reference lines just outside the region
    lx = max(0, x0 - 3)
    ty = max(0, y0 - 3)

    # 2D inverse distance weighting from the two clean boundaries
    p = 1.5
    for y in range(y0, h):
        dT = (y - ty) or 0.5
        wT = 1.0 / (dT ** p)
        for x in range(x0, w):
            dL = (x - lx) or 0.5
            wL = 1.0 / (dL ** p)
            lc = src[lx, y]
            tc = src[x, ty]
            tot = wL + wT
            px[x, y] = tuple(
                int(max(0, min(255, round((lc[i] * wL + tc[i] * wT) / tot))))
                for i in range(3)
            )

    # smooth the filled field so no streaks survive
    blurred = out.filter(ImageFilter.GaussianBlur(blur))
    mask = Image.new("L", (w, h), 0)
    mp = mask.load()
    for y in range(y0, h):
        dy = min(y - y0, feather) / float(feather)
        for x in range(x0, w):
            dx = min(x - x0, feather) / float(feather)
            mp[x, y] = int(255 * min(dx, dy))
    out = Image.composite(blurred, out, mask)

    # re-apply film grain inside the filled area
    px = out.load()
    for y in range(y0 + 2, h):
        for x in range(x0 + 2, w):
            c = px[x, y]
            px[x, y] = tuple(
                int(max(0, min(255, round(c[i] + random.gauss(0.0, grain)))))
                for i in range(3)
            )
    return out


PEND = [
    (
        r"D:\study lamp\lamp\lamp\docs\design\Product_design_sheet_of_a_desk_2026-11-09T08-20-51.png",
        r"D:\study lamp\lamp\lamp\docs\design\study-robot-design-sheet-A.png",
    ),
    (
        r"D:\study lamp\lamp\lamp\docs\design\Industrial_design_presentation_2026-11-09T08-21-32.png",
        r"D:\study lamp\lamp\lamp\docs\design\study-robot-design-sheet-B.png",
    ),
]

if __name__ == "__main__":
    random.seed(7)
    for s, d in PEND:
        img = Image.open(s).convert("RGB")
        fill_region(img, 1394, 936).save(d)
        print(d)
