# SPDX-License-Identifier: GPL-3.0-or-later
"""The PS1 GTE register file (32 data + 32 control words) as 64 u32: find
the block in a raw memory image and name its fields.

gte_regs(raw) locates the block in a reference save state's decompressed
data (GTE::DoState writes the 64 registers contiguously) by InitGeom's
ZSF3 = 341, ZSF4 = 256 and DQA = -4194 (cr29 / cr30 / cr27); reading the
save state's container is local reference-emulator tooling
(refemu_gte_state.load, pc/tools/refemu.py).  describe() also reads the
port's <label>.gte shot sidecar (lit_retail_check.load_regs)."""
import struct


def gte_regs(raw, dqa=0xEF9E):
    """[(offset, 64 words)] of the GTE register block: ZSF3 = 341 and
    ZSF4 = 256 (InitGeom) and, by default, InitGeom's DQA.  A game that
    sets its own depth cue (TAIIKU's taiiku_depth_set: SetDQA) moves DQA;
    when no block has InitGeom's, the ZSF pair alone is used."""
    pat = struct.pack("<I", 341)
    hits, loose = [], []
    i = raw.find(pat)
    while i >= 0:
        # cr29 = ZSF3 at block offset (32 + 29) * 4
        base = i - (32 + 29) * 4
        if base >= 0:
            words = struct.unpack_from('<64I', raw, base)
            if words[32 + 29] == 341 and words[32 + 30] == 256:
                (hits if words[32 + 27] & 0xFFFF == dqa else loose).append((base, words))
        i = raw.find(pat, i + 1)
    return hits or loose


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def s32(v):
    return v - (1 << 32) if v & 0x80000000 else v


def describe(words):
    d, c = words[:32], words[32:]
    def mat(base):
        w = c[base:base + 5]
        return [[s16(w[0]), s16(w[0] >> 16), s16(w[1])],
                [s16(w[1] >> 16), s16(w[2]), s16(w[2] >> 16)],
                [s16(w[3]), s16(w[3] >> 16), s16(w[4])]]
    out = {
        'RT': mat(0), 'TR': [s32(c[5]), s32(c[6]), s32(c[7])],
        'LLM': mat(8), 'BK': [s32(c[13]), s32(c[14]), s32(c[15])],
        'LCM': mat(16), 'FC': [s32(c[21]), s32(c[22]), s32(c[23])],
        'OF': [s32(c[24]), s32(c[25])], 'H': c[26] & 0xFFFF, 'DQA': s16(c[27]), 'DQB': s32(c[28]),
        'ZSF3': s16(c[29]), 'ZSF4': s16(c[30]), 'FLAG': '%08x' % c[31],
        'RGBC': '%08x' % d[6], 'IR0': s16(d[8]), 'RGB0': '%08x' % d[20], 'RGB1': '%08x' % d[21],
        'RGB2': '%08x' % d[22], 'MAC': [s32(d[24]), s32(d[25]), s32(d[26]), s32(d[27])],
        'IR': [s16(d[9]), s16(d[10]), s16(d[11])], 'SZ': [d[16], d[17], d[18], d[19]],
        'VXY0': [s16(d[0]), s16(d[0] >> 16), s16(d[1])],
    }
    return out
