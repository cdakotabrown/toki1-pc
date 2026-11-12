#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reproduce the lit TMD packets of a retail frame from its RAM and GTE state.

usage: lit_retail_check.py RAM.bin STATE.sav|LABEL.gte [--bk0]

RAM.bin and STATE.sav come from the same reference-emulator regtest run
(-dumpram / -savestate).  The
port's side is a PC_INPUT `shot` with PC_SHOT_VRAM=1 PC_PARITY_RAM=1:
LABEL.ram and LABEL.gte (pc/hal/parity.c).  For
every enabled object3d_sort slot (class_w) and TACO GsDOBJ2 it derives Lw
from the object's coordinate record, Ls = GsWSMATRIX x Lw and LLM =
GsLIGHTWSMATRIX x Lw, takes LCM from _LC and BK / FC / H / OFX / OFY from
the GTE registers in the save state, runs gte_reference.build_lit over the
object's lit runs and searches the RAM for each modelled packet body.
--bk0 is the negative control (BK forced to 0).  Objects whose coordinate
has a super chain are skipped (TACO's lit objects have none).
"""
import argparse
import struct

import gte_reference as ref
import gte_state as gs
import refemu

FAM = {0x20: 'F3L', 0x24: 'TF3L', 0x34: 'TG3L'}
SIZE = {0x20: 16, 0x24: 24, 0x30: 20, 0x34: 28, 0x28: 20, 0x2C: 32, 0x38: 24, 0x3C: 36,
        0x21: 16, 0x29: 16, 0x31: 24, 0x25: 28, 0x35: 36, 0x39: 28, 0x2D: 32, 0x3D: 44}


def mulrot(a, b):
    return [max(-0x8000, min(0x7FFF, sum(a[3 * i + k] * b[3 * k + j] for k in range(3)) >> 12))
            for i in range(3) for j in range(3)]


def mulcoord2(a, b):
    (ar, at), (br, bt) = a, b
    loc = [ref.s32(sum(ar[3 * i + k] * bt[k] for k in range(3)) >> 12) for i in range(3)]
    return mulrot(ar, br), [ref.s32(x + y) for x, y in zip(loc, at)]


def check(ram, st, bk0=False):
    def w(a):
        return struct.unpack_from('<I', ram, a & 0x1FFFFF)[0]

    def mat(a):
        return (list(struct.unpack_from('<9h', ram, a & 0x1FFFFF)),
                list(struct.unpack_from('<3i', ram, (a & 0x1FFFFF) + 20)))

    lightws, ws, lc = mat(0x80131740)[0], mat(0x80131CB0), mat(0x80131760)[0]
    ton = w(0x80131D80)
    bk = [0, 0, 0] if bk0 else st['BK']
    ofx, ofy = st['OF'][0] >> 16, st['OF'][1] >> 16
    ot_base = 0x800F1150 + 20 * w(0x80127180)

    def objects():
        for i in range(w(0x800EF7FC), 160, 2):
            p = 0x801271B0 + 68 * i
            if ram[p & 0x1FFFFF] and ram[(p + 1) & 0x1FFFFF] & 0x80:
                yield 'slot%d' % i, p + 4
        for i in range(64):
            a = 0x8012F0F0 + 16 * i
            if not w(a) & 0x80000000 and w(a + 4):
                yield 'dobj%d' % i, a
        # TAIIKU's GsDOBJ2 array `object` (taiiku_model_init: 16 records,
        # attribute 0x80000000 = off)
        for i in range(16):
            a = 0x8012AB20 + 16 * i
            if not w(a) & 0x80000000 and w(a + 4) and w(a + 8):
                yield 'obj%d' % i, a

    # TmdTG3's fog arm: GsLIGNR == 0 && GsLIGHT_MODE == 1, or GsLIGNR == 1
    # && GsLMODE == 1 (only TG3 has a modelled LFG builder)
    lignr = w(0x80131D74)
    fog = (lignr == 0 and w(0x80131CA0) == 1) or (lignr == 1 and w(0x80131D70) == 1)
    fam = dict(FAM)
    if fog:
        fam[0x34] = 'TG3LFG'
        del fam[0x20], fam[0x24]

    res = {'fog': fog, 'modelled': 0, 'found': 0, 'families': {}, 'colour_only_misses': 0,
           'missed_objects': set(), 'lcm_is_lc': sum(st['LCM'], []) == lc}
    for name, dobj in objects():
        coord, tmd = w(dobj + 4), w(dobj + 8)
        if w(coord + 72):
            continue
        lw = mat(coord + 4)
        ls = mulcoord2(ws, lw)
        light = ref.Light(mulrot(lightws, lw[0]), lc, bk, st['FC'])
        vt, nv, nt, nn, pt, npr = struct.unpack_from('<6I', ram, tmd & 0x1FFFFF)
        verts = [list(struct.unpack_from('<3h', ram, (vt + 8 * k) & 0x1FFFFF)) for k in range(nv)]
        norms = [list(struct.unpack_from('<3h', ram, (nt + 8 * k) & 0x1FFFFF)) for k in range(nn)]
        left, prim = npr, pt
        while left > 0:
            code = ram[(prim + 3) & 0x1FFFFF] & 0xFD
            cnt = struct.unpack_from('<H', ram, prim & 0x1FFFFF)[0]
            if code not in SIZE or cnt == 0:
                break
            size = SIZE[code]
            if code in fam:
                g = ref.Gte(ls[0], ls[1], st['H'], ofx, ofy)
                g.dqa, g.dqb = st['DQA'], st['DQB']
                pr = ram[prim & 0x1FFFFF:(prim & 0x1FFFFF) + size * cnt]
                for _k, _otz, _b, body in ref.build_lit(fam[code], pr, verts, norms, cnt, ton,
                                                        g, light, w(ot_base + 8), 6):
                    res['modelled'] += 1
                    res['families'][fam[code]] = res['families'].get(fam[code], 0) + 1
                    if ram.find(b''.join(struct.pack('<I', x & 0xFFFFFFFF) for x in body)) >= 0:
                        res['found'] += 1
                        continue
                    res['missed_objects'].add(name)
                    if ram.find(b''.join(struct.pack('<I', x) for x in body[1:3])) >= 0:
                        res['colour_only_misses'] += 1
            left -= cnt
            prim += size * cnt
    return res


def load_regs(path):
    """the 64 GTE registers: a reference-emulator save state (read by the
    local refemu_gte_state, pc/tools/refemu.py), or the port's
    <label>.gte shot sidecar (256 bytes, u32le in the same order)"""
    if path.endswith('.gte'):
        raw = open(path, 'rb').read()
        if len(raw) != 256:
            raise SystemExit(f'{path}: expected 256 bytes')
        return struct.unpack('<64I', raw)
    try:
        state = refemu.local_module('refemu_gte_state', 'reading a save state')
    except refemu.RefEmuUnavailable as e:
        raise SystemExit(str(e))
    return gs.gte_regs(state.load(path))[0][1]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('ram')
    ap.add_argument('state', help='retail save state, or the port\'s <label>.gte')
    ap.add_argument('--bk0', action='store_true')
    a = ap.parse_args()
    ram = open(a.ram, 'rb').read()
    st = gs.describe(load_regs(a.state))
    print('GTE BK', st['BK'], 'FC', st['FC'], 'H', st['H'])
    r = check(ram, st, a.bk0)
    print('lit packets modelled %(modelled)d, found word for word %(found)d' % r)
    print('families', r['families'], 'LCM == _LC', r['lcm_is_lc'])
    print('misses whose geometry is present with another colour:', r['colour_only_misses'],
          'objects with misses:', sorted(r['missed_objects']))


if __name__ == '__main__':
    main()
