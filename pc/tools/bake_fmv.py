#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""bake_fmv.py — convert a jpsxdec-extracted PNG sequence + WAV into a
single .fmv container for the PC port's HAL FMV player.

Format:
  offset size  field
  0      4     magic "TFMV"
  4      2     width  (u16 LE)
  6      2     height (u16 LE)
  8      4     n_frames (u32 LE)
  12     2     fps      (u16 LE)
  14     2     reserved (u16)
  16     ...   n_frames of width*height*3 RGB24 bytes, concatenated

The companion .wav file is kept alongside as a standard PCM WAV.

Usage:
  bake_fmv.py <png_glob> <out.fmv> [--fps 15]
"""

import argparse
import glob
import os
import struct
import sys

from PIL import Image
from asset_limits import check_pixels


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("png_glob")
    ap.add_argument("out_fmv")
    ap.add_argument("--fps", type=int, default=15)
    args = ap.parse_args()

    paths = sorted(glob.glob(args.png_glob))
    if not paths:
        sys.exit(f"no PNGs match {args.png_glob}")
    first = Image.open(paths[0])
    check_pixels(*first.size)
    first = first.convert("RGB")
    w, h = first.size
    n = len(paths)
    print(f"{n} frames, {w}x{h}, fps={args.fps} -> {args.out_fmv}")

    with open(args.out_fmv, "wb") as f:
        f.write(b"TFMV")
        f.write(struct.pack("<HHIHH", w, h, n, args.fps, 0))
        for i, p in enumerate(paths):
            im = Image.open(p)
            check_pixels(*im.size)
            im = im.convert("RGB")
            if im.size != (w, h):
                sys.exit(f"frame {i} size mismatch: {im.size} vs {(w,h)}")
            f.write(im.tobytes())
            if i % 50 == 0:
                print(f"  frame {i}/{n}")
    print(f"wrote {os.path.getsize(args.out_fmv)} bytes")


if __name__ == "__main__":
    main()
