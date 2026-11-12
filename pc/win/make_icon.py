#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""make_icon.py -- draw pc/win/tokimeki-pc.ico, the Windows exe's icon.

Original artwork drawn from code (no game asset): a rounded pink tile with a
white heart and a small sparkle.  Every size is rendered from a 4x
supersampled master so the small sizes stay crisp.  The .ico is committed;
re-run this only to change the design:

    python3 pc/win/make_icon.py            # -> pc/win/tokimeki-pc.ico
"""
import math
import sys
from pathlib import Path

from PIL import Image, ImageDraw

SIZES = (16, 24, 32, 48, 64, 128, 256)
OUT = Path(__file__).with_name('tokimeki-pc.ico')

TOP = (255, 128, 170)      # tile gradient, top
BOTTOM = (214, 62, 122)    # tile gradient, bottom
HEART = (255, 255, 255)
SHADE = (150, 30, 85, 90)  # the heart's soft drop shadow
SPARK = (255, 244, 178)


def heart_points(cx, cy, r, n=360):
    """The classic parametric heart, scaled to half-width r."""
    pts = []
    for i in range(n):
        t = 2 * math.pi * i / n
        x = 16 * math.sin(t) ** 3
        y = 13 * math.cos(t) - 5 * math.cos(2 * t) - 2 * math.cos(3 * t) - math.cos(4 * t)
        pts.append((cx + x * r / 16, cy - y * r / 16))
    return pts


def sparkle(draw, cx, cy, r, fill):
    """A four-pointed star."""
    k = r * 0.28
    draw.polygon([(cx, cy - r), (cx + k, cy - k), (cx + r, cy), (cx + k, cy + k),
                  (cx, cy + r), (cx - k, cy + k), (cx - r, cy), (cx - k, cy - k)], fill=fill)


def master(px):
    s = px * 4
    img = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    grad = Image.new('RGBA', (s, s))
    gd = ImageDraw.Draw(grad)
    for y in range(s):
        f = y / (s - 1)
        gd.line([(0, y), (s, y)], fill=tuple(int(a + (b - a) * f) for a, b in zip(TOP, BOTTOM)) + (255,))
    mask = Image.new('L', (s, s), 0)
    pad = s * 0.04
    ImageDraw.Draw(mask).rounded_rectangle([pad, pad, s - pad, s - pad], radius=s * 0.22, fill=255)
    img.paste(grad, (0, 0), mask)

    d = ImageDraw.Draw(img, 'RGBA')
    cx, cy, r = s * 0.5, s * 0.53, s * 0.30
    d.polygon(heart_points(cx + s * 0.015, cy + s * 0.03, r), fill=SHADE)
    d.polygon(heart_points(cx, cy, r), fill=HEART)
    if px >= 24:   # too small to read below that
        sparkle(d, s * 0.77, s * 0.24, s * 0.11, SPARK)
        sparkle(d, s * 0.25, s * 0.78, s * 0.06, SPARK)
    return img.resize((px, px), Image.LANCZOS)


def main(out=OUT):
    imgs = [master(px) for px in SIZES]
    imgs[-1].save(out, format='ICO', sizes=[(p, p) for p in SIZES], append_images=imgs[:-1])
    print(f'{out}: {", ".join(str(p) for p in SIZES)} px')


if __name__ == '__main__':
    main(Path(sys.argv[1]) if len(sys.argv) > 1 else OUT)
