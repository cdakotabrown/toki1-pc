#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""XA ADPCM -> WAV decoder for Tokimeki Memorial XA files.

The game's XA files (character voice, ambient streams) are raw CD-XA
Mode 2 Form 2 sectors, 2336 bytes each:
    8 bytes subheader (file/channel/submode/ci, duplicated for robustness)
    2324 bytes XA ADPCM user data (18 sound groups of 128 bytes each + trailer)
    4 bytes EDC

Each sound group carries 28 samples x 4 sound units (112 bytes of samples
+ 16 bytes of duplicated sound parameters). Samples are 4-bit ADPCM with
a 2-sample prediction filter; coefficients match VAG/SPU ADPCM.

We produce a 44.1kHz stereo 16-bit PCM WAV by default (resample-free —
37.8kHz is the source rate). Output is what we'd play through SDL_audio
directly, so edits in e.g. Audacity can be packed back trivially if we
ever implement WAV->XA (not needed for read-only voice playback).

Usage:
    python3 pc/tools/xa_extract.py input.xa [output.wav]
    python3 pc/tools/xa_extract.py --batch pc/assets/raw/CDROM/XA_G [pc/assets/xa/]
"""
from __future__ import annotations

import argparse
import struct
import sys
import wave
import os
import tempfile
from pathlib import Path

SECTOR_SIZE = 2336
SUBHEADER_SIZE = 8
USER_DATA_SIZE = 2324
SOUND_GROUP_SIZE = 128
GROUPS_PER_SECTOR = 18

# Standard Sony PS1 ADPCM filter coefficients (f0, f1), fixed point
# (divide by 64 to apply). Same table used by VAG decoder.
XA_FILTERS = [
    (0, 0),
    (60, 0),
    (115, -52),
    (98, -55),
    (122, -60),
]


def _sext4(v: int) -> int:
    return v - 16 if v & 0x8 else v


def _clip16(v: int) -> int:
    if v < -32768: return -32768
    if v >  32767: return  32767
    return v


def decode_sound_group(group: bytes, prev_l, prev_r, stereo=True, bits4=True):
    """Decode one 128-byte XA sound group.

    Returns (left_samples, right_samples, new_prev_l, new_prev_r). For mono
    streams the right channel is empty and prev_r is passed through.
    """
    # Sound parameters occupy the first 16 bytes. There are 8 parameter
    # bytes per half-group for 4-bit data (4 sound units), each byte giving
    # [range<<0 | filter<<4]. Bytes 0-3 are the "upper" set, 4-7 the
    # "lower" (same values, duplicated at +0 and +4 for error resilience).
    sp = group[:16]
    L = []; R = []

    # 28 samples per sound unit, 4 or 8 sound units depending on bits.
    # For 4-bit: 4 units. Each sample nibble stored at byte 16 + i*4 + unit/2
    # with low nibble -> unit 0/1, high nibble -> unit 2/3.
    n_units = 4  # 4-bit mode
    for u in range(n_units):
        param = sp[4 + u]  # duplicated at +0; we use the lower copy
        shift = 12 - (param & 0xF)
        filt  = (param >> 4) & 0xF
        if filt >= len(XA_FILTERS):
            filt = 0
        f0, f1 = XA_FILTERS[filt]

        samples = []
        for s in range(28):
            byte = group[16 + s * 4 + (u // 2)]
            nibble = (byte >> (4 * (u & 1))) & 0xF
            raw = _sext4(nibble) << shift
            if stereo:
                if u % 2 == 0:
                    pred = (prev_l[0] * f0 + prev_l[1] * f1) >> 6
                    s16 = _clip16(raw + pred)
                    prev_l = (s16, prev_l[0])
                else:
                    pred = (prev_r[0] * f0 + prev_r[1] * f1) >> 6
                    s16 = _clip16(raw + pred)
                    prev_r = (s16, prev_r[0])
            else:
                pred = (prev_l[0] * f0 + prev_l[1] * f1) >> 6
                s16 = _clip16(raw + pred)
                prev_l = (s16, prev_l[0])
            samples.append(s16)

        if stereo:
            if u % 2 == 0:
                L.extend(samples)
            else:
                R.extend(samples)
        else:
            L.extend(samples)

    return L, R, prev_l, prev_r


def decode_xa(xa_path: Path, out_wav: Path, file_filter=None, channel_filter=None):
    if xa_path.stat().st_size % SECTOR_SIZE:
        raise ValueError(f'{xa_path}: incomplete XA sector')
    sectors_used = 0
    temp = None
    try:
        with xa_path.open('rb') as source:
            # Coding information comes from the first selected AUDIO sector.
            first = None
            while sector := source.read(SECTOR_SIZE):
                sub = sector[:SUBHEADER_SIZE]
                if (sub[2] & 4 and
                    (file_filter is None or sub[0] == file_filter) and
                    (channel_filter is None or sub[1] == channel_filter)):
                    first = sub
                    break
            if first is None:
                print(f'  {xa_path.name}: no audio sectors found')
                return 0
            ci = first[3]
            if ci & 0x10:
                raise ValueError('8-bit XA is not supported')
            stereo = bool(ci & 1)
            freq = 18900 if ci & 4 else 37800
            prev_l = prev_r = (0, 0)
            source.seek(0)
            with tempfile.NamedTemporaryFile(dir=out_wav.parent,
                                             suffix='.wav.tmp', delete=False) as f:
                temp = Path(f.name)
            with wave.open(str(temp), 'wb') as wav:
                wav.setnchannels(2 if stereo else 1)
                wav.setsampwidth(2)
                wav.setframerate(freq)
                while sector := source.read(SECTOR_SIZE):
                    if len(sector) != SECTOR_SIZE:
                        raise ValueError('XA changed or was truncated during decoding')
                    sub = sector[:SUBHEADER_SIZE]
                    if not sub[2] & 4:
                        continue
                    if file_filter is not None and sub[0] != file_filter:
                        continue
                    if channel_filter is not None and sub[1] != channel_filter:
                        continue
                    if (sub[3] & 0x15) != (ci & 0x15):
                        raise ValueError('mixed XA coding modes; select a file/channel')
                    body = sector[SUBHEADER_SIZE:SUBHEADER_SIZE + USER_DATA_SIZE]
                    for g in range(GROUPS_PER_SECTOR):
                        grp = body[g * SOUND_GROUP_SIZE:(g + 1) * SOUND_GROUP_SIZE]
                        left, right, prev_l, prev_r = decode_sound_group(
                            grp, prev_l, prev_r, stereo=stereo, bits4=True)
                        pcm = bytearray()
                        if stereo:
                            for lval, rval in zip(left, right):
                                pcm += struct.pack('<hh', lval, rval)
                        else:
                            for val in left:
                                pcm += struct.pack('<h', val)
                        wav.writeframesraw(pcm)
                    sectors_used += 1
        os.replace(temp, out_wav)
        temp = None
    finally:
        if temp is not None:
            temp.unlink(missing_ok=True)
    print(f'  {xa_path.name} -> {out_wav.name} ({sectors_used} sectors, '
          f'{freq}Hz {"stereo" if stereo else "mono"})')
    return sectors_used


def batch_extract(root: Path, out_dir: Path):
    out_dir.mkdir(parents=True, exist_ok=True)
    count = 0
    for xa in root.rglob("*.XA"):
        out = out_dir / (xa.stem + ".wav")
        if decode_xa(xa, out) > 0:
            count += 1
    print(f"Extracted {count} XA files")


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("input")
    p.add_argument("output", nargs="?")
    p.add_argument("--batch", action="store_true",
                   help="treat input as a directory, recursively extract")
    p.add_argument("--file", type=int, default=None,
                   help="only decode sectors with this file id (default: all)")
    p.add_argument("--channel", type=int, default=None,
                   help="only decode sectors with this channel id (default: all)")
    args = p.parse_args()

    if args.batch:
        batch_extract(Path(args.input),
                      Path(args.output) if args.output else Path("pc/assets/xa"))
    else:
        out = Path(args.output) if args.output else Path(args.input).with_suffix(".wav")
        decode_xa(Path(args.input), out, args.file, args.channel)


if __name__ == "__main__":
    main()
