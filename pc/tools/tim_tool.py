#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""TIM <-> PNG bidirectional converter for PS1 native textures.

TIM is the PlayStation texture format used throughout Tokimeki Memorial
(fonts, sprites, backgrounds). A TIM stores both pixel data (4/8/16/24bpp)
and its CLUT, along with the destination coordinates inside the 1024x512
VRAM framebuffer.

Modes:
    unpack  TIM -> PNG + sidecar .tim.json  (editable RGBA image)
    pack    PNG + sidecar .tim.json -> TIM  (round-trips back into the game)
    batch   recursively unpack every TIM under a directory

Usage:
    python3 pc/tools/tim_tool.py unpack input.tim [output.png]
    python3 pc/tools/tim_tool.py pack   input.png [output.tim]
    python3 pc/tools/tim_tool.py batch  pc/assets/raw/CDROM [pc/assets/tim/]

The sidecar JSON preserves everything the PNG can't carry: pixel mode,
FB destination rectangle, CLUT location, and CLUT index palette. Editors
may modify the PNG pixels (or even the CLUT palette by editing the JSON)
and pack will regenerate a byte-valid TIM.

Legal: you must own the game. Do not redistribute extracted assets.
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys
from pathlib import Path
from asset_limits import read_asset, check_pixels

try:
    from PIL import Image
except ImportError:
    sys.stderr.write("tim_tool: pip install Pillow\n")
    sys.exit(1)

TIM_MAGIC = 0x10

PMODE_4BPP = 0
PMODE_8BPP = 1
PMODE_16BPP = 2
PMODE_24BPP = 3

PMODE_NAMES = {0: "4bpp", 1: "8bpp", 2: "16bpp", 3: "24bpp"}


def bgr555_to_rgba(px: int) -> tuple[int, int, int, int]:
    """Decode a PS1 15-bit color word to RGBA8888.

    PS1 semantics: bit 15 (STP) is the semi-transparency flag. When STP=0
    and RGB=0, the pixel is fully transparent. When STP=0 and RGB!=0, it's
    opaque. When STP=1, it's semi-transparent (mixed by GPU). For PNG we
    collapse to: transparent when (RGB==0 and STP==0), else opaque.
    """
    r = (px >> 0) & 0x1F
    g = (px >> 5) & 0x1F
    b = (px >> 10) & 0x1F
    stp = (px >> 15) & 1
    R = (r << 3) | (r >> 2)
    G = (g << 3) | (g >> 2)
    B = (b << 3) | (b >> 2)
    if r == 0 and g == 0 and b == 0 and stp == 0:
        return (0, 0, 0, 0)
    return (R, G, B, 255)


def rgba_to_bgr555(r: int, g: int, b: int, a: int) -> int:
    """Encode RGBA8888 back to PS1 15-bit color. Preserves STP semantics:
    fully transparent -> 0x0000, everything else -> STP=0, RGB quantised."""
    if a < 128:
        return 0x0000
    r5 = (r >> 3) & 0x1F
    g5 = (g >> 3) & 0x1F
    b5 = (b >> 3) & 0x1F
    w = (b5 << 10) | (g5 << 5) | r5
    if w == 0:
        # Force STP=1 so the PS1 GPU doesn't treat a real black as transparent
        w = 0x8000
    return w


def parse_header(buf: bytes, off: int = 0):
    if len(buf) < 8:
        raise ValueError("TIM too small for header")
    magic = buf[off]
    if magic != TIM_MAGIC:
        raise ValueError(f"not a TIM (magic 0x{magic:02x}, expected 0x10)")
    if buf[off + 1] != 0 or buf[off + 2] != 0 or buf[off + 3] != 0:
        raise ValueError("TIM header reserved bytes nonzero")
    flag = struct.unpack_from("<I", buf, off + 4)[0]
    pmode = flag & 0x7
    has_clut = (flag >> 3) & 1
    return pmode, bool(has_clut), 8  # body starts at +8


def parse_clut(buf: bytes, off: int):
    # CLUT block: u32 bnum (total bytes inc. this header),
    # u16 dx, u16 dy, u16 w, u16 h, then w*h BGR555 entries.
    (bnum,) = struct.unpack_from("<I", buf, off)
    dx, dy, w, h = struct.unpack_from("<HHHH", buf, off + 4)
    pixels_off = off + 12
    check_pixels(w, h)
    n = w * h
    if bnum != 12 + n * 2 or off + bnum > len(buf):
        raise ValueError("CLUT truncated")
    entries = struct.unpack_from(f"<{n}H", buf, pixels_off)
    return {
        "dx": dx, "dy": dy, "w": w, "h": h,
        "entries": list(entries),
    }, off + bnum


def parse_pixels(buf: bytes, off: int, pmode: int):
    (bnum,) = struct.unpack_from("<I", buf, off)
    dx, dy, w16, h = struct.unpack_from("<HHHH", buf, off + 4)
    if bnum != 12 + w16 * h * 2 or off + bnum > len(buf):
        raise ValueError('TIM pixel block length does not match dimensions')
    body = buf[off + 12 : off + bnum]
    # w16 is width in 16-bit units. Convert to pixel width per pmode.
    if pmode == PMODE_4BPP:
        pix_w = w16 * 4
    elif pmode == PMODE_8BPP:
        pix_w = w16 * 2
    elif pmode == PMODE_16BPP:
        pix_w = w16
    elif pmode == PMODE_24BPP:
        # 3 bytes per pixel; 16-bit units mean pix_w = w16*2/3 — rare,
        # so we record both and defer decode.
        pix_w = (w16 * 2) // 3
    else:
        raise ValueError(f"bad pmode {pmode}")
    return {
        "dx": dx, "dy": dy, "w16": w16, "h": h, "pix_w": pix_w,
        "body": body,
    }, off + bnum


def decode_4bpp(body: bytes, w: int, h: int, clut_entries):
    if len(clut_entries) < 16:
        raise ValueError(f"4bpp needs >=16 CLUT entries, got {len(clut_entries)}")
    out = bytearray(w * h * 4)
    i = 0
    for y in range(h):
        for x in range(0, w, 2):
            if i >= len(body):
                break
            byte = body[i]; i += 1
            # Low nibble is the left pixel, high nibble the right.
            idx0 = byte & 0x0F
            idx1 = (byte >> 4) & 0x0F
            for off_x, idx in ((0, idx0), (1, idx1)):
                r, g, b, a = bgr555_to_rgba(clut_entries[idx])
                p = ((y * w) + (x + off_x)) * 4
                out[p] = r; out[p+1] = g; out[p+2] = b; out[p+3] = a
    return bytes(out)


def decode_8bpp(body: bytes, w: int, h: int, clut_entries):
    out = bytearray(w * h * 4)
    for p in range(w * h):
        idx = body[p] if p < len(body) else 0
        if idx < len(clut_entries):
            r, g, b, a = bgr555_to_rgba(clut_entries[idx])
        else:
            r, g, b, a = 0, 0, 0, 0
        q = p * 4
        out[q] = r; out[q+1] = g; out[q+2] = b; out[q+3] = a
    return bytes(out)


def decode_16bpp(body: bytes, w: int, h: int):
    out = bytearray(w * h * 4)
    for p in range(w * h):
        if p * 2 + 1 >= len(body):
            break
        px = body[p*2] | (body[p*2+1] << 8)
        r, g, b, a = bgr555_to_rgba(px)
        q = p * 4
        out[q] = r; out[q+1] = g; out[q+2] = b; out[q+3] = a
    return bytes(out)


def decode_24bpp(body: bytes, w: int, h: int):
    out = bytearray(w * h * 4)
    for p in range(w * h):
        if p * 3 + 2 >= len(body):
            break
        out[p*4]   = body[p*3]
        out[p*4+1] = body[p*3+1]
        out[p*4+2] = body[p*3+2]
        out[p*4+3] = 255
    return bytes(out)


def unpack_tim(tim_path: Path, png_path: Path | None = None):
    data = read_asset(tim_path)
    pmode, has_clut, off = parse_header(data)
    meta = {
        "source": tim_path.name,
        "pmode": pmode,
        "pmode_name": PMODE_NAMES[pmode],
        "has_clut": has_clut,
    }
    clut = None
    if has_clut:
        clut, off = parse_clut(data, off)
        meta["clut"] = {
            "dx": clut["dx"], "dy": clut["dy"],
            "w": clut["w"], "h": clut["h"],
            "entries": [f"0x{e:04x}" for e in clut["entries"]],
        }
    pix, off = parse_pixels(data, off, pmode)
    meta["pixels"] = {
        "dx": pix["dx"], "dy": pix["dy"],
        "w16": pix["w16"], "pix_w": pix["pix_w"], "h": pix["h"],
    }

    w, h = pix["pix_w"], pix["h"]
    check_pixels(w, h)
    if pmode == PMODE_4BPP:
        rgba = decode_4bpp(pix["body"], w, h, clut["entries"])
    elif pmode == PMODE_8BPP:
        rgba = decode_8bpp(pix["body"], w, h, clut["entries"])
    elif pmode == PMODE_16BPP:
        rgba = decode_16bpp(pix["body"], w, h)
    elif pmode == PMODE_24BPP:
        rgba = decode_24bpp(pix["body"], w, h)
    else:
        raise ValueError(f"bad pmode {pmode}")

    if png_path is None:
        png_path = tim_path.with_suffix(".png")
    img = Image.frombytes("RGBA", (w, h), rgba)
    img.save(png_path)
    json_path = png_path.with_suffix(".tim.json")
    json_path.write_text(json.dumps(meta, indent=2))
    return png_path, json_path


def encode_clut(clut_meta) -> bytes:
    entries = [int(e, 16) if isinstance(e, str) else e for e in clut_meta["entries"]]
    w, h = clut_meta["w"], clut_meta["h"]
    body = struct.pack(f"<{len(entries)}H", *entries)
    bnum = 12 + len(body)
    return struct.pack("<IHHHH", bnum, clut_meta["dx"], clut_meta["dy"], w, h) + body


def encode_4bpp_pixels(img: Image.Image, clut_entries):
    # Map each RGBA pixel to the nearest CLUT index.
    clut_rgba = [bgr555_to_rgba(e) for e in clut_entries[:16]]
    px = img.load()
    w, h = img.size
    out = bytearray((w * h) // 2)
    for y in range(h):
        for x in range(0, w, 2):
            idx0 = _nearest_clut_idx(px[x, y], clut_rgba)
            idx1 = _nearest_clut_idx(px[x + 1, y] if x + 1 < w else (0,0,0,0), clut_rgba)
            out[(y * w + x) // 2] = (idx0 & 0xF) | ((idx1 & 0xF) << 4)
    return bytes(out)


def encode_8bpp_pixels(img: Image.Image, clut_entries):
    clut_rgba = [bgr555_to_rgba(e) for e in clut_entries]
    px = img.load()
    w, h = img.size
    out = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            out[y * w + x] = _nearest_clut_idx(px[x, y], clut_rgba)
    return bytes(out)


def encode_16bpp_pixels(img: Image.Image):
    px = img.load()
    w, h = img.size
    out = bytearray(w * h * 2)
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            w15 = rgba_to_bgr555(r, g, b, a)
            o = (y * w + x) * 2
            out[o] = w15 & 0xFF
            out[o + 1] = (w15 >> 8) & 0xFF
    return bytes(out)


def _nearest_clut_idx(rgba, clut_rgba):
    best, best_d = 0, 1 << 30
    if len(rgba) == 3:
        r, g, b = rgba; a = 255
    else:
        r, g, b, a = rgba
    for i, (cr, cg, cb, ca) in enumerate(clut_rgba):
        # Alpha mismatch is expensive so transparent/opaque don't swap.
        d = (r - cr) ** 2 + (g - cg) ** 2 + (b - cb) ** 2
        if a < 128 and ca >= 128:
            d += 1 << 24
        if d < best_d:
            best_d = d; best = i
    return best


def pack_tim(png_path: Path, tim_path: Path | None = None):
    json_path = png_path.with_suffix(".tim.json")
    if not json_path.exists():
        raise FileNotFoundError(f"sidecar missing: {json_path}")
    meta = json.loads(json_path.read_text())
    img = Image.open(png_path)
    check_pixels(*img.size)
    img = img.convert("RGBA")

    pmode = meta["pmode"]
    has_clut = meta["has_clut"]
    header = struct.pack("<BBBBI", TIM_MAGIC, 0, 0, 0,
                        pmode | (0x08 if has_clut else 0))

    clut_bytes = b""
    if has_clut:
        clut_bytes = encode_clut(meta["clut"])

    clut_entries = [int(e, 16) for e in meta["clut"]["entries"]] if has_clut else []
    pix_meta = meta["pixels"]
    if pmode == PMODE_4BPP:
        body = encode_4bpp_pixels(img, clut_entries)
    elif pmode == PMODE_8BPP:
        body = encode_8bpp_pixels(img, clut_entries)
    elif pmode == PMODE_16BPP:
        body = encode_16bpp_pixels(img)
    else:
        raise NotImplementedError(f"packing pmode {pmode} not yet supported")

    bnum = 12 + len(body)
    pix_block = struct.pack("<IHHHH",
                            bnum,
                            pix_meta["dx"], pix_meta["dy"],
                            pix_meta["w16"], pix_meta["h"]) + body

    if tim_path is None:
        tim_path = png_path.with_suffix(".tim")
    tim_path.write_bytes(header + clut_bytes + pix_block)
    return tim_path


def batch_unpack(root: Path, out_dir: Path):
    count = 0; failed = 0
    for tim in root.rglob("*.TIM"):
        rel = tim.relative_to(root)
        dst = out_dir / rel.with_suffix(".png")
        dst.parent.mkdir(parents=True, exist_ok=True)
        try:
            unpack_tim(tim, dst)
            count += 1
        except Exception as e:
            print(f"  FAIL {tim}: {e}")
            failed += 1
    print(f"Unpacked {count} TIM files, {failed} failures")


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = p.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("unpack")
    sp.add_argument("tim"); sp.add_argument("png", nargs="?")

    sp = sub.add_parser("pack")
    sp.add_argument("png"); sp.add_argument("tim", nargs="?")

    sp = sub.add_parser("batch")
    sp.add_argument("root"); sp.add_argument("out", nargs="?", default="pc/assets/tim")

    args = p.parse_args()
    if args.cmd == "unpack":
        png, js = unpack_tim(Path(args.tim), Path(args.png) if args.png else None)
        print(f"Wrote {png} + {js}")
    elif args.cmd == "pack":
        tim = pack_tim(Path(args.png), Path(args.tim) if args.tim else None)
        print(f"Wrote {tim}")
    elif args.cmd == "batch":
        batch_unpack(Path(args.root), Path(args.out))


if __name__ == "__main__":
    main()
