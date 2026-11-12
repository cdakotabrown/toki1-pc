#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""mcr_tool.py — inspect / inject / extract PSX memory-card images.

Operates on the standard 128 KB PSX .mcr layout (16 x 8 KB blocks) used
by the PC-port HAL at `saves/card1.mcr`. Interoperates with MemcardRex
and the common single-save .mcs exports (128 B dir frame + 8 KB data).

Commands:
  list    CARD                           — list directory entries
  extract CARD NAME OUT.mcs              — dump a save to an .mcs file
  inject  CARD IN.mcs                    — inject an .mcs into a free slot
  delete  CARD NAME                      — free the entry for NAME
  format  CARD                           — zero card and write fresh header
  seed    CARD SEED_DIR                  — inject every *.mcs under SEED_DIR
  album-show CARD N                      — the game's album N save: date, club,
                                           stats, girls (tokimeki_album.py)
  album-set  CARD N FIELD=VALUE ... [--ram RAM]
                                         — edit album N's record and fix its
                                           checksum (the scenario builder,
                                           pc/tools/scenario.py, and the
                                           checkpoint builder, checkpoints.py,
                                           use this); date=YY/MM/DD also sets
                                           youbi, pass_days, season and (with
                                           --ram) the exact season and cal
"""

import argparse
import os
import struct
import sys
from asset_limits import read_asset

MC_SIZE         = 128 * 1024
MC_BLOCK_SIZE   = 8192
MC_FRAME_SIZE   = 128
MC_DATA_BLOCKS  = 15
MCS_TOTAL       = MC_FRAME_SIZE + MC_BLOCK_SIZE  # 8320

STATE_FREE      = 0xA0
STATE_USED_FIRST = 0x51
LINK_TERM       = 0xFFFF


def xor_cs(buf: bytes) -> int:
    x = 0
    for b in buf:
        x ^= b
    return x


def load_card(path: str) -> bytearray:
    data = read_asset(path, MC_SIZE)
    if len(data) != MC_SIZE:
        sys.exit(f"{path}: size {len(data)} != {MC_SIZE}")
    return bytearray(data)


def save_card(path: str, card: bytearray) -> None:
    with open(path, "wb") as f:
        f.write(card)


def dir_frame(card: bytearray, idx: int) -> memoryview:
    return memoryview(card)[idx * MC_FRAME_SIZE:(idx + 1) * MC_FRAME_SIZE]


def block_data(card: bytearray, blk: int) -> memoryview:
    return memoryview(card)[blk * MC_BLOCK_SIZE:(blk + 1) * MC_BLOCK_SIZE]


def parse_entry(card: bytearray, idx: int):
    f = dir_frame(card, idx)
    state, size = struct.unpack("<II", bytes(f[0:8]))
    link = struct.unpack("<H", bytes(f[8:10]))[0]
    name = bytes(f[0x0A:0x0A + 20]).split(b"\x00", 1)[0].decode("ascii", "replace")
    return state, size, link, name


def seal_frame(card: bytearray, idx: int) -> None:
    f = dir_frame(card, idx)
    f[MC_FRAME_SIZE - 1] = xor_cs(bytes(f[0:MC_FRAME_SIZE - 1]))


def format_header(card: bytearray, idx: int) -> None:
    f = dir_frame(card, idx)
    for i in range(MC_FRAME_SIZE):
        f[i] = 0
    f[0] = ord("M")
    f[1] = ord("C")
    seal_frame(card, idx)


def format_card(card: bytearray) -> None:
    for i in range(MC_SIZE):
        card[i] = 0
    format_header(card, 0)
    for i in range(1, MC_DATA_BLOCKS + 1):
        f = dir_frame(card, i)
        f[0] = STATE_FREE
        struct.pack_into("<H", f, 8, LINK_TERM)
        seal_frame(card, i)
    for i in range(16, 36):
        f = dir_frame(card, i)
        for j in range(4):
            f[j] = 0xFF
        struct.pack_into("<H", f, 8, LINK_TERM)
        seal_frame(card, i)
    format_header(card, 63)


def cmd_list(args):
    card = load_card(args.card)
    print(f"{'slot':>4}  {'state':>5}  {'size':>6}  {'link':>4}  name")
    for i in range(1, MC_DATA_BLOCKS + 1):
        state, size, link, name = parse_entry(card, i)
        print(f"{i:>4}   0x{state:02X}   {size:>6}  0x{link:04X}  {name}")


def find_by_name(card: bytearray, name: str) -> int:
    for i in range(1, MC_DATA_BLOCKS + 1):
        state, _size, _link, n = parse_entry(card, i)
        if state == STATE_USED_FIRST and (n == name or n.startswith(name)):
            return i
    return -1


def find_free(card: bytearray) -> int:
    for i in range(1, MC_DATA_BLOCKS + 1):
        state, _size, _link, _n = parse_entry(card, i)
        if state == STATE_FREE or (state & 0xF0) == 0xA0:
            return i
    return -1


def cmd_extract(args):
    card = load_card(args.card)
    idx = find_by_name(card, args.name)
    if idx < 0:
        sys.exit(f"no entry matching '{args.name}'")
    out = bytearray(MCS_TOTAL)
    out[0:MC_FRAME_SIZE] = bytes(dir_frame(card, idx))
    out[MC_FRAME_SIZE:] = bytes(block_data(card, idx))
    with open(args.out, "wb") as f:
        f.write(out)
    print(f"extracted slot {idx} ('{parse_entry(card, idx)[3]}') -> {args.out}")


def cmd_inject(args):
    card = load_card(args.card)
    data = read_asset(args.mcs, MCS_TOTAL)
    if len(data) != MCS_TOTAL:
        sys.exit(f"{args.mcs}: size {len(data)} != {MCS_TOTAL}")
    name = data[0x0A:0x0A + 20].split(b"\x00", 1)[0].decode("ascii", "replace")
    if not name:
        sys.exit("mcs has empty filename field")
    if find_by_name(card, name) >= 0 and not args.force:
        sys.exit(f"'{name}' already present; pass --force to overwrite")
    idx = find_by_name(card, name)
    if idx < 0:
        idx = find_free(card)
    if idx < 0:
        sys.exit("card is full")
    dir_frame(card, idx)[:] = data[:MC_FRAME_SIZE]
    seal_frame(card, idx)
    block_data(card, idx)[:] = data[MC_FRAME_SIZE:]
    save_card(args.card, card)
    print(f"injected '{name}' -> slot {idx}")


def cmd_delete(args):
    card = load_card(args.card)
    idx = find_by_name(card, args.name)
    if idx < 0:
        sys.exit(f"no entry matching '{args.name}'")
    f = dir_frame(card, idx)
    for i in range(MC_FRAME_SIZE):
        f[i] = 0
    f[0] = STATE_FREE
    struct.pack_into("<H", f, 8, LINK_TERM)
    seal_frame(card, idx)
    save_card(args.card, card)
    print(f"freed slot {idx}")


def cmd_format(args):
    card = bytearray(MC_SIZE)
    format_card(card)
    save_card(args.card, card)
    print(f"formatted {args.card}")


def cmd_seed(args):
    card = load_card(args.card) if os.path.exists(args.card) else None
    if card is None:
        card = bytearray(MC_SIZE)
        format_card(card)
    injected = 0
    for fn in sorted(os.listdir(args.seed_dir)):
        if not fn.lower().endswith(".mcs"):
            continue
        path = os.path.join(args.seed_dir, fn)
        data = read_asset(path, MCS_TOTAL)
        if len(data) != MCS_TOTAL:
            print(f"  skip (size {len(data)}): {fn}")
            continue
        name = data[0x0A:0x0A + 20].split(b"\x00", 1)[0].decode("ascii", "replace")
        if not name:
            print(f"  skip (no name): {fn}")
            continue
        if find_by_name(card, name) >= 0:
            print(f"  skip (exists): {name}")
            continue
        idx = find_free(card)
        if idx < 0:
            print(f"  skip (full): {name}")
            continue
        dir_frame(card, idx)[:] = data[:MC_FRAME_SIZE]
        seal_frame(card, idx)
        block_data(card, idx)[:] = data[MC_FRAME_SIZE:]
        print(f"  seeded '{name}' -> slot {idx}")
        injected += 1
    save_card(args.card, card)
    print(f"{injected} save(s) injected")


def cmd_album_show(args):
    import tokimeki_album as alb
    card = load_card(args.card)
    try:
        rec = alb.read_record(card, args.n)
    except alb.AlbumError as e:
        sys.exit(str(e))
    g = lambda k: alb.get_field(rec, k)
    club = g("basyo_pack") >> 12
    print(f"album {args.n} ({alb.album_name(args.n)}), listed in the system file: "
          f"{alb.album_listed(card, args.n)}")
    print(f"  date 19{g('year')}/{g('month')}/{g('day')} youbi {g('youbi')} season {g('season')} "
          f"pass_days {g('pass_days')}")
    print(f"  club {club} ({alb.CLUBS[club] if club < len(alb.CLUBS) else '?'})")
    print("  " + " ".join(f"{k}={g(k)}" for k in alb.STATS))
    base = alb.FIELDS["girl_tbl"][0]
    for i, name in enumerate(alb.GIRLS):
        vals = {f: struct.unpack_from(fmt, rec, base + 56 * i + off)[0]
                for f, (off, fmt) in alb.GIRL_FIELDS.items()}
        dates = sum(rec[base + 56 * i + 21:base + 56 * i + 56])
        print(f"  girl {i:2d} {name:7s} " + " ".join(f"{f}={v:#x}" if f in ("flags", "birthday")
                                              else f"{f}={v}" for f, v in vals.items())
              + f" dates={dates}")
    res = [struct.unpack_from("<I", rec, alb.FIELDS["reserved_date"][0] + 4 * i)[0] for i in range(8)]
    print("  reserved_date " + " ".join(f"{v:#010x}" for v in res))
    bk = alb.FIELDS["bukatu_rec"][0]
    clubs = [f"{alb.CLUBS[c]}={struct.unpack_from('<H', rec, bk + 4 * c)[0]}/{rec[bk + 4 * c + 2]}"
             for c in range(12) if any(rec[bk + 4 * c:bk + 4 * c + 4])]
    print(f"  activity {g('activity'):#x}  bukatu (points/b) {' '.join(clubs) or '-'}  "
          f"date_basyo_hist {g('date_basyo_hist').hex()}")
    ev = alb.FIELDS["ev_flag"][0]
    print("  ev_flag set: " + (" ".join(f"{i:02x}={rec[ev + i]}" for i in range(256) if rec[ev + i])
                               or "-"))


def ram_symbols(binary):
    """PS1 data symbols (name -> address) of a port binary: calendar_cal
    needs cal_font_load_tbl0"""
    import subprocess
    out = {}
    r = subprocess.run(["nm", binary], capture_output=True, text=True)
    for line in r.stdout.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in "AaBbDd":
            out.setdefault(p[2], int(p[0], 16))
    return out


def cmd_album_set(args):
    import tokimeki_album as alb
    card = load_card(args.card)
    try:
        rec = alb.read_record(card, args.n)
        date = None
        for kv in args.fields:
            k, _, v = kv.partition("=")
            if k == "date":
                date = tuple(int(x) for x in v.split("/"))
            else:
                alb.set_field(rec, k, v)
        if date:
            ram = syms = None
            if args.ram:
                with open(args.ram, "rb") as fh:
                    ram = fh.read()
                syms = ram_symbols(args.bin)
            alb.apply_date(rec, *date, ram=ram, symbols=syms,
                           keep={kv.partition("=")[0] for kv in args.fields})
        alb.write_record(card, args.n, rec)
    except (alb.AlbumError, ValueError, KeyError) as e:
        sys.exit(f"album-set: {e}")
    save_card(args.card, card)
    print(f"album {args.n}: {len(args.fields)} field(s) set, checksum {alb.checksum(rec):#010x}")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("list"); s.add_argument("card"); s.set_defaults(fn=cmd_list)
    s = sub.add_parser("extract"); s.add_argument("card"); s.add_argument("name"); s.add_argument("out"); s.set_defaults(fn=cmd_extract)
    s = sub.add_parser("inject"); s.add_argument("card"); s.add_argument("mcs"); s.add_argument("--force", action="store_true"); s.set_defaults(fn=cmd_inject)
    s = sub.add_parser("delete"); s.add_argument("card"); s.add_argument("name"); s.set_defaults(fn=cmd_delete)
    s = sub.add_parser("format"); s.add_argument("card"); s.set_defaults(fn=cmd_format)
    s = sub.add_parser("seed"); s.add_argument("card"); s.add_argument("seed_dir"); s.set_defaults(fn=cmd_seed)
    s = sub.add_parser("album-show"); s.add_argument("card"); s.add_argument("n", type=int); s.set_defaults(fn=cmd_album_show)
    s = sub.add_parser("album-set"); s.add_argument("card"); s.add_argument("n", type=int)
    s.add_argument("fields", nargs="+")
    s.add_argument("--ram", help="2 MB PS1 RAM holding the calendar pictures (scenario.py's "
                   "template schedule.ram): date= then also sets the exact season and cal")
    s.add_argument("--bin", default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                  "..", "build", "tokimeki-pc"),
                   help="the port binary whose symbols locate the calendar table in --ram")
    s.set_defaults(fn=cmd_album_set)

    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
