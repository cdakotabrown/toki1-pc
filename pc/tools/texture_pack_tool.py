#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Texture dumps and replacement packs for the PC port (enhanced profile).

The game writes what it uploads and samples as PNGs when dumping is on
(`[graphics] texture_dump = true` or TOKI_TEXTURE_DUMP=1; pc/hal/gpu_texrep.c)
into <data dir>/textures/dump/SLPS-00065/, with index.csv.  A pack is a folder
of PNGs with the same names, any size: <data dir>/textures/packs/NAME/.

File names (pc/hal/gpu_texrep.h):
    t16-<source hash>-<W>x<H>.png                  15-bit pictures
    t8-<source hash>-<palette hash>-<W>x<H>.png    8-bit, under one palette
    t4-<source hash>-<palette hash>-<W>x<H>.png    4-bit, under one palette
W x H is the original size in texels; a replacement may be any size (2x, 4x
or anything else: it is scaled to the internal resolution).  Alpha is
coverage: the dump writes 0 for the console's see-through colour 0000h and
255 for everything else; when loading, alpha < 16 is see-through, 16..239 is
mixed with what is behind (soft edges), >= 240 is solid.  The console's
semi-transparency bit is not in the PNG: a replacement keeps the original
texel's.

    python3 pc/tools/texture_pack_tool.py list DUMP_DIR [--by kind|size|scene]
    python3 pc/tools/texture_pack_tool.py make-test-pack DUMP_DIR PACK_DIR
            [--scale 2] [--tint 255,160,160] [--only SUBSTRING]
    python3 pc/tools/texture_pack_tool.py validate PACK_DIR [--dump DUMP_DIR]

`make-test-pack` is a synthetic pack for testing the replacement path: every
dumped PNG enlarged by nearest neighbour and tinted, so a replaced texture is
obvious on screen.  `validate` checks names, PNG headers and sizes, and with
--dump reports pack files that no dumped texture matches (and how many dumped
textures the pack covers).

Only the Python standard library is used (zlib for PNG).
"""
import argparse
import collections
import csv
import os
import re
import struct
import sys
import zlib

NAME_RE = re.compile(
    r'^(?:t16-(?P<h16>[0-9a-f]{16})|t(?P<bits>[48])-(?P<h>[0-9a-f]{16})-(?P<clut>[0-9a-f]{16}))'
    r'-(?P<w>\d+)x(?P<hgt>\d+)\.png$', re.I)

PNG_SIG = b'\x89PNG\r\n\x1a\n'


def parse_name(name):
    """(kind, width, height) of a dump / pack file name, or None."""
    m = NAME_RE.match(name)
    if not m:
        return None
    kind = 't16' if m.group('h16') else 't' + m.group('bits')
    w, h = int(m.group('w')), int(m.group('hgt'))
    if not (0 < w <= 4096 and 0 < h <= 512):
        return None
    return kind, w, h


def png_size(path):
    """(width, height) from the IHDR chunk; None when not a PNG."""
    with open(path, 'rb') as fh:
        head = fh.read(24)
    if len(head) < 24 or head[:8] != PNG_SIG or head[12:16] != b'IHDR':
        return None
    return struct.unpack('>II', head[16:24])


def read_png(path):
    """(width, height, rows of RGBA bytes) for 8-bit RGBA/RGB/grey PNGs."""
    with open(path, 'rb') as fh:
        data = fh.read()
    if data[:8] != PNG_SIG:
        raise ValueError(f'{path}: not a PNG')
    pos, idat, ihdr = 8, [], None
    while pos < len(data):
        n, typ = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if typ == b'IHDR':
            ihdr = struct.unpack('>IIBBBBB', body)
        elif typ == b'IDAT':
            idat.append(body)
        elif typ == b'IEND':
            break
        pos += 12 + n
    w, h, depth, ctype, _, _, interlace = ihdr
    channels = {6: 4, 2: 3, 0: 1, 4: 2}.get(ctype)
    if depth != 8 or channels is None or interlace:
        raise ValueError(f'{path}: only 8-bit non-interlaced RGBA/RGB/grey PNGs')
    raw = zlib.decompress(b''.join(idat))
    stride, bpp = w * channels, channels
    rows, prev, off = [], bytearray(stride), 0
    for _ in range(h):
        f, line = raw[off], bytearray(raw[off + 1:off + 1 + stride])
        off += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        if channels == 4:
            rows.append(bytes(line))
        else:
            out = bytearray()
            for x in range(w):
                px = line[x * channels:(x + 1) * channels]
                if channels == 3:
                    out += px + b'\xff'
                elif channels == 1:
                    out += bytes((px[0], px[0], px[0], 255))
                else:
                    out += bytes((px[0], px[0], px[0], px[1]))
            rows.append(bytes(out))
    return w, h, rows


def write_png(path, w, h, rows):
    """An 8-bit RGBA PNG (filter 0, zlib level 6)."""
    def chunk(typ, body):
        return (struct.pack('>I', len(body)) + typ + body +
                struct.pack('>I', zlib.crc32(typ + body) & 0xffffffff))
    raw = b''.join(b'\x00' + r for r in rows)
    with open(path, 'wb') as fh:
        fh.write(PNG_SIG + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                 chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def read_index(dump_dir):
    path = os.path.join(dump_dir, 'index.csv')
    if not os.path.exists(path):
        return []
    with open(path, newline='') as fh:
        return list(csv.DictReader(fh))


def cmd_list(args):
    rows = read_index(args.dump)
    if not rows:
        print(f'{args.dump}: no index.csv (nothing dumped yet?)')
        return 1
    missing = [r['file'] for r in rows if not os.path.exists(os.path.join(args.dump, r['file']))]
    groups = collections.Counter()
    for r in rows:
        kind = r['file'].split('-', 1)[0]
        key = {'kind': f"{kind} {r['kind']}", 'size': f"{kind} {r['width']}x{r['height']}",
               'scene': f"scene {r['scene']}"}[args.by]
        groups[key] += 1
    print(f'{len(rows)} dumped textures in {args.dump}')
    for key, n in groups.most_common():
        print(f'  {n:6d}  {key}')
    if args.files:
        for r in rows:
            print(f"{r['file']}  {r['kind']}  vram {r['vram_x']},{r['vram_y']} "
                  f"{r['vram_w']}x{r['vram_h']}  scene {r['scene']} frame {r['frame']}")
    if missing:
        print(f'{len(missing)} file(s) in the index are missing (deleted?)')
    return 0


def cmd_make(args):
    tint = [int(c) for c in args.tint.split(',')]
    if len(tint) != 3 or not all(0 <= c <= 255 for c in tint):
        print('--tint wants R,G,B (0..255)')
        return 2
    os.makedirs(args.pack, exist_ok=True)
    made = 0
    for name in sorted(os.listdir(args.dump)):
        if not parse_name(name) or (args.only and args.only not in name):
            continue
        w, h, rows = read_png(os.path.join(args.dump, name))
        k = args.scale
        out = []
        for y in range(h * k):
            src = rows[y // k]
            line = bytearray(w * k * 4)
            for x in range(w * k):
                p = src[(x // k) * 4:(x // k) * 4 + 4]
                line[x * 4] = p[0] * tint[0] // 255
                line[x * 4 + 1] = p[1] * tint[1] // 255
                line[x * 4 + 2] = p[2] * tint[2] // 255
                line[x * 4 + 3] = p[3]
            out.append(bytes(line))
        write_png(os.path.join(args.pack, name), w * k, h * k, out)
        made += 1
    print(f'{made} replacement(s) written to {args.pack} ({args.scale}x, tint {args.tint})')
    return 0 if made else 1


def cmd_validate(args):
    problems, ok, scales = [], 0, collections.Counter()
    names = set()
    for name in sorted(os.listdir(args.pack)):
        path = os.path.join(args.pack, name)
        if os.path.isdir(path):
            continue
        parsed = parse_name(name)
        if not parsed:
            if name.lower().endswith('.png'):
                problems.append(f'{name}: not a texture name (t16-/t8-/t4-...-WxH.png); ignored by the game')
            continue
        kind, w, h = parsed
        size = png_size(path)
        if not size:
            problems.append(f'{name}: not a PNG file')
            continue
        pw, ph = size
        if pw % w or ph % h:
            problems.append(f'{name}: {pw}x{ph} is not a whole multiple of {w}x{h} '
                            '(allowed, but it will be resampled)')
        elif pw // w != ph // h:
            problems.append(f'{name}: {pw}x{ph} scales width {pw // w}x but height {ph // h}x')
        else:
            scales[pw // w] += 1
        names.add(name)
        ok += 1
    print(f'{args.pack}: {ok} replacement file(s)')
    for k, n in sorted(scales.items()):
        print(f'  {n:6d} at {k}x')
    if scales:
        print(f'  the game will draw this pack at {min(max(max(scales), 2), 4)}x '
              '(the largest factor, at least 2, at most 4)')
    unmatched = 0
    if args.dump:
        dumped = {r['file'] for r in read_index(args.dump)}
        dumped |= {n for n in os.listdir(args.dump) if parse_name(n)}
        stray = sorted(names - dumped)
        unmatched = len(stray)
        for name in stray:
            problems.append(f'{name}: no dumped texture has this name (wrong hash or palette?)')
        print(f'  covers {len(names & dumped)} of {len(dumped)} dumped texture(s); '
              f'{unmatched} unmatched')
    for p in problems:
        print('  ' + p)
    hard = [p for p in problems if 'not a PNG' in p or 'no dumped texture' in p]
    return 1 if hard else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('list', help='summarise a dump folder (index.csv)')
    p.add_argument('dump')
    p.add_argument('--by', choices=('kind', 'size', 'scene'), default='size')
    p.add_argument('--files', action='store_true', help='one line per texture')
    p.set_defaults(fn=cmd_list)
    p = sub.add_parser('make-test-pack', help='a synthetic pack: enlarged, tinted dumps')
    p.add_argument('dump')
    p.add_argument('pack')
    p.add_argument('--scale', type=int, default=2, choices=(1, 2, 3, 4))
    p.add_argument('--tint', default='255,160,160', help='R,G,B multiplier (255 = keep)')
    p.add_argument('--only', default='', help='only files whose name contains this')
    p.set_defaults(fn=cmd_make)
    p = sub.add_parser('validate', help='check a pack folder')
    p.add_argument('pack')
    p.add_argument('--dump', help='the dump folder to match names against')
    p.set_defaults(fn=cmd_validate)
    args = ap.parse_args(argv)
    return args.fn(args)


if __name__ == '__main__':
    sys.exit(main())
