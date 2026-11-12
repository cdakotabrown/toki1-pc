# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent reference model of the PS1 GTE and the libgs unlit TMD packet
builders, for tests of pc/hal/gte.c and src/main/pc-port/tmd_gte_builders.c.

Written from public documentation, not from the port's C:
  * GTE arithmetic, FLAG bits and the UNR reciprocal divide: psx-spx, "GTE".
  * GPU polygon packets (F3/F4/G3/G4/FT3/FT4/GT3/GT4): psx-spx, "GPU Render
    Polygon Commands"; Sony TMD primitive layouts: the TMD file format.
  * RotMatrix / SquareRoot0/12 / InitGeom: the retail LIBGTE listings in
    asm/main/matched/ (M = Rx * Ry * Rz, product negated before the shift).

Checked against retail: all 12 title-card FT4 packets of a retail RAM image
(scene 18, frame_counter 1105) are reproduced word for word by this model
from that image's object coordinates and GsWSMATRIX
(docs/pc-port/GTE-BUILDERS.md, 2026-10-30 geometry entry).
"""
import math

MASK32 = 0xFFFFFFFF


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def s32(v):
    v &= MASK32
    return v - 0x100000000 if v & 0x80000000 else v


# ---- tables (synthetic stand-ins; pc/tests/gte_test_tables.c uses the same)
def cossin_table():
    out = []
    for i in range(4096):
        a = 2.0 * math.pi * i / 4096.0
        s = int(math.floor(4096.0 * math.sin(a) + 0.5))
        c = int(math.floor(4096.0 * math.cos(a) + 0.5))
        out.append((s & 0xFFFF) | ((c & 0xFFFF) << 16))
    return out


def sqrt_table():
    return [int(math.floor(4096.0 * math.sqrt((i + 64) / 64.0) + 0.5))
            for i in range(192)]


COSSIN = cossin_table()
SQRT = sqrt_table()


# ---- LIBGTE leaves ----------------------------------------------------
def cossin(a):
    if a >= 0:
        w = COSSIN[a & 0xFFF]
        s = s16(w)
    else:
        w = COSSIN[(-a) & 0xFFF]
        s = -s16(w)
    return s, s32(w) >> 16


def rot_axis(axis, a, m):
    """RotMatrixX/Y/Z (LIBGTE FGO_04/05/06 listings, TACO/DATE overlays):
    m (9 entries) multiplied in place by the rotation about one axis.
    X and Z use (sin, cos) from cossin(a); Y uses -sin.  Rows (p, q) are
    (1, 2) for X, (0, 2) for Y, (0, 1) for Z:
      p' = s16((c*p - s*q) >> 12), q' = s16((s*p + c*q) >> 12)
    with 32-bit wrapping products and sums."""
    s, c = cossin(a)
    if axis == 'y':
        s = -s
    p, q = {'x': (1, 2), 'y': (0, 2), 'z': (0, 1)}[axis]
    out = list(m)
    for k in range(3):
        rp, rq = m[3 * p + k], m[3 * q + k]
        out[3 * p + k] = s16(s32(c * rp - s * rq) >> 12)
        out[3 * q + k] = s16(s32(s * rp + c * rq) >> 12)
    return out


def mul12(a, b):
    return s32(a * b) >> 12


def negmul12(a, b):
    return s32(-(a * b)) >> 12


def rot_matrix(vx, vy, vz):
    """RotMatrix (FGO_01): returns 9 entries, row major."""
    sx, cx = cossin(vx)
    sy, cy = cossin(vy)
    sz, cz = cossin(vz)
    m = [0] * 9
    m[2] = sy
    m[5] = negmul12(cy, sx)
    m[8] = mul12(cy, cx)
    m[0] = mul12(cz, cy)
    m[1] = negmul12(sz, cy)
    t = mul12(cz, -sy)
    m[3] = mul12(sz, cx) - mul12(t, sx)
    m[6] = mul12(sz, sx) + mul12(t, cx)
    t = mul12(sz, -sy)
    m[4] = mul12(cz, cx) + mul12(t, sx)
    m[7] = mul12(cz, sx) - mul12(t, cx)
    return [s16(v) for v in m]


def _lzcs(a):
    v = (~a if a < 0 else a) & MASK32
    n = 0
    while n < 32 and not (v & 0x80000000):
        v = (v << 1) & MASK32
        n += 1
    return n


def _sqrt_parts(a, bias):
    lz = _lzcs(a)
    if lz == 32:
        return None
    even = lz & ~1
    shift = (bias - even) >> 1
    if even - 24 >= 0:
        top = s32(a << (even - 24))
    else:
        top = a >> (24 - even)
    return SQRT[top - 64], shift


def square_root0(a):
    p = _sqrt_parts(a, 31)
    if p is None:
        return 0
    t, shift = p
    return s32(((t << (shift & 31)) & MASK32) >> 12)


def square_root12(a):
    p = _sqrt_parts(a, 19)
    if p is None:
        return 0
    t, shift = p
    if shift >= 0:
        return s32(t << (shift & 31))
    return s32((t & MASK32) >> ((-shift) & 31))


# ---- GTE perspective pipeline ------------------------------------------
UNR = [max(0, (0x40000 // (i + 0x100) + 1) // 2 - 0x101) for i in range(257)]


class Gte:
    """RTPS/RTPT/NCLIP/AVSZ3/AVSZ4 with InitGeom's ZSF3 = 341, ZSF4 = 256,
    DQA = -4194, DQB = 0x01400000."""

    def __init__(self, rot, trans, h=1000, ofx=0, ofy=0):
        self.rot = [rot[0:3], rot[3:6], rot[6:9]]
        self.tr = list(trans)
        self.h, self.ofx, self.ofy = h, ofx << 16, ofy << 16
        self.zsf3, self.zsf4, self.dqa, self.dqb = 341, 256, -4194, 0x01400000
        self.sz = [0, 0, 0, 0]
        self.sxy = [0, 0, 0]
        self.flag = 0
        self.ir0 = 0

    def _divide(self, sz3):
        if self.h >= sz3 * 2:
            self.flag |= 1 << 17
            return 0x1FFFF
        z = 0
        while z < 16 and not (sz3 << z) & 0x8000:
            z += 1
        n = self.h << z
        d = sz3 << z
        u = UNR[(d - 0x7FC0) >> 7] + 0x101
        d = ((0x2000080 - d * u) & MASK32) >> 8
        d = ((0x0000080 + d * u) & MASK32) >> 8
        return min(0x1FFFF, (n * d + 0x8000) >> 16)

    def _mac0(self, v):
        if v > 0x7FFFFFFF:
            self.flag |= 1 << 16
        elif v < -0x80000000:
            self.flag |= 1 << 15
        return v

    def _rtp(self, v, last):
        ir = []
        mac = []
        for i in range(3):
            acc = self.tr[i] * 4096 + sum(self.rot[i][j] * v[j] for j in range(3))
            if acc > 0x7FFFFFFFFFF:
                self.flag |= 1 << (30 - i)
            elif acc < -0x80000000000:
                self.flag |= 1 << (27 - i)
            m = acc >> 12
            mac.append(m)
            if m < -0x8000 or m > 0x7FFF:
                self.flag |= 1 << (24 - i)
            ir.append(max(-0x8000, min(0x7FFF, m)))
        sz = mac[2]
        if sz < 0 or sz > 0xFFFF:
            self.flag |= 1 << 18
        sz = max(0, min(0xFFFF, sz))
        self.sz = self.sz[1:] + [sz]
        n = self._divide(sz)
        xy = []
        for k, bit in ((0, 14), (1, 13)):
            o = self.ofx if k == 0 else self.ofy
            q = self._mac0(n * ir[k] + o) >> 16
            if q < -0x400 or q > 0x3FF:
                self.flag |= 1 << bit
            xy.append(max(-0x400, min(0x3FF, q)))
        self.sxy = self.sxy[1:] + [(xy[0] & 0xFFFF) | ((xy[1] & 0xFFFF) << 16)]
        if last:
            d = self._mac0(n * self.dqa + self.dqb) >> 12
            if d < 0 or d > 0x1000:
                self.flag |= 1 << 12
            self.ir0 = max(0, min(0x1000, d))

    def rtps(self, v):
        self.flag = 0
        self._rtp(v, True)

    def rtpt(self, v0, v1, v2):
        self.flag = 0
        self._rtp(v0, False)
        self._rtp(v1, False)
        self._rtp(v2, True)

    def flags(self):
        f = self.flag & 0x7FFFF000
        if f & 0x7F87E000:
            f |= 0x80000000
        return f

    def error(self):
        return bool(self.flags() & 0x80000000)

    def nclip(self):
        (x0, y0), (x1, y1), (x2, y2) = [(s16(v), s16(v >> 16)) for v in self.sxy]
        self.flag = 0
        return s32(self._mac0(x0 * y1 + x1 * y2 + x2 * y0
                              - x0 * y2 - x1 * y0 - x2 * y1))

    def _otz(self, v):
        self.flag = 0
        v = self._mac0(v) >> 12
        if v < 0 or v > 0xFFFF:
            self.flag |= 1 << 18
        return max(0, min(0xFFFF, v))

    def avsz3(self):
        return self._otz(self.zsf3 * sum(self.sz[1:]))

    def avsz4(self):
        return self._otz(self.zsf4 * sum(self.sz))


# ---- GTE lighting (psx-spx "GTE Lighting / Color Calculation") ---------
# Control registers as the libgte setters write them: SetLightMatrix /
# SetColorMatrix load LLM / LCM verbatim; SetBackColor(r,g,b) and
# SetFarColor(r,g,b) store r<<4, g<<4, b<<4 into RBK.. / RFC.. (retail
# listings asm/main/matched/SetBackColor.s, SetFarColor.s).

class Light:
    """LLM, LCM (9 entries, row major), BK and FC as register values."""

    def __init__(self, llm, lcm, bk=(0, 0, 0), fc=(0, 0, 0)):
        self.llm, self.lcm = list(llm), list(lcm)
        self.bk, self.fc = list(bk), list(fc)


def _lm_sat(v, lm, flag, i):
    lo = 0 if lm else -0x8000
    if v < lo or v > 0x7FFF:
        flag[0] |= 1 << (24 - i)
    return max(lo, min(0x7FFF, v))


def _mac44(v, flag, i):
    if v > 0x7FFFFFFFFFF:
        flag[0] |= 1 << (30 - i)
    elif v < -0x80000000000:
        flag[0] |= 1 << (27 - i)
    return v


def _colour(v, flag, i):
    if v < 0 or v > 0xFF:
        flag[0] |= 1 << (21 - i)
    return max(0, min(0xFF, v))


def light_vertex(gte, light, normal, rgbc, sf=1, lm=1, cue=False):
    """One vertex of NCCS/NCCT (cue=False) or NCDS/NCDT (cue=True), sf/lm as
    the command bits.  Returns the colour FIFO word (r | g<<8 | b<<16 |
    code<<24) and updates gte.flag / gte.ir.  The stages, per psx-spx:
      IR  = (LLM * V) >> sf*12                         (lm saturation)
      IR  = (BK << 12 + LCM * IR) >> sf*12             (lm saturation)
      MAC = (RGB * IR) << 4
      NCD: IR' = ((FC << 12) - MAC) >> sf*12 (lm=0 saturation);
           MAC = IR' * IR0 + MAC
      MAC >>= sf*12; FIFO = sat8(MAC >> 4) | CODE; IR = sat(MAC, lm)."""
    shift = 12 if sf else 0
    flag = [gte.flag]
    ir = []
    for i in range(3):
        acc = _mac44(sum(light.llm[3 * i + k] * normal[k] for k in range(3)), flag, i)
        ir.append(_lm_sat(acc >> shift, lm, flag, i))
    ir2 = []
    for i in range(3):
        acc = _mac44(light.bk[i] * 0x1000 + sum(light.lcm[3 * i + k] * ir[k] for k in range(3)),
                     flag, i)
        ir2.append(_lm_sat(acc >> shift, lm, flag, i))
    rgb = [rgbc & 0xFF, (rgbc >> 8) & 0xFF, (rgbc >> 16) & 0xFF]
    mac = [(rgb[i] * ir2[i]) << 4 for i in range(3)]
    if cue:
        for i in range(3):
            d = _lm_sat(_mac44((light.fc[i] << 12) - mac[i], flag, i) >> shift, 0, flag, i)
            mac[i] = _mac44(d * gte.ir0 + mac[i], flag, i)
    mac = [m >> shift for m in mac]
    out = [_colour(mac[i] >> 4, flag, i) for i in range(3)]
    gte.ir = [_lm_sat(mac[i], lm, flag, i) for i in range(3)]
    gte.flag = flag[0]
    return out[0] | (out[1] << 8) | (out[2] << 16) | (rgbc & 0xFF000000)


def ncs(gte, light, normals, rgbc, cue=False):
    """NCCS/NCDS (one normal) or NCCT/NCDT (three); returns the FIFO words in
    order (the last one is RGB2)."""
    gte.flag = 0
    return [light_vertex(gte, light, n, rgbc, cue=cue) for n in normals]


# ---- unlit TMD builders -------------------------------------------------
# name: (TMD code, prim bytes, quad, textured, gouraud, vertex index offset)
FAMILIES = {
    'NF3': (0x21, 16, False, False, False, 0x08),
    'NF4': (0x29, 16, True, False, False, 0x08),
    'NG3': (0x31, 24, False, False, True, 0x10),
    'NG4': (0x39, 28, True, False, True, 0x14),
    'TNF3': (0x25, 28, False, True, False, 0x14),
    'TNF4': (0x2D, 32, True, True, False, 0x18),
    'TNG3': (0x35, 36, False, True, True, 0x1C),
    'TNG4': (0x3D, 44, True, True, True, 0x24),
}


def word(b, off):
    return int.from_bytes(b[off:off + 4], 'little')


def half(b, off):
    return int.from_bytes(b[off:off + 2], 'little')


def gpu_packet(name, prim, sxy, ton):
    """The GPU polygon packet body (without the OT tag) the builder emits:
    psx-spx polygon layout -- colour/command word, then per vertex
    [colour (Gouraud, after the first)] xy [uv (textured)]."""
    code, _size, quad, tex, gour, _voff = FAMILIES[name]
    nv = 4 if quad else 3
    if tex:
        uv = [word(prim, 4 + 4 * i) for i in range(nv)]
        rgb0 = 4 + 4 * nv
        # The TMD mode byte's bit 0 (raw texture) only selects the run; the
        # GPU command is mode >> 1 << 1 with GsTON as semi-transparency.
        cmd = (((code | (ton << 1)) >> 1) << 25) & MASK32
        first = (word(prim, rgb0) & 0xFFFFFF) | cmd
        rgbs = [word(prim, rgb0 + 4 * i) for i in range(nv)] if gour else None
    else:
        first = word(prim, 4) | (ton << 25)
        rgbs = [word(prim, 4 + 4 * i) for i in range(nv)] if gour else None
    out = [first & MASK32]
    for i in range(nv):
        if gour and i > 0:
            out.append(rgbs[i])
        out.append(sxy[i])
        if tex:
            out.append(uv[i])
    return out


def build(name, prims, verts, count, ton, gte, ot_offset, shift):
    """Returns [(prim index, otz, packet body words)] for the emitted prims,
    in emission order."""
    code, size, quad, tex, gour, voff = FAMILIES[name]
    emitted = []
    for k in range(count):
        p = prims[k * size:(k + 1) * size]
        pair = word(p, voff)
        v = [verts[pair & 0xFFFF], verts[pair >> 16], verts[half(p, voff + 4)]]
        gte.rtpt(*v)
        if gte.error() or gte.nclip() <= 0:
            continue
        sxy = list(gte.sxy)
        if quad:
            gte.rtps(verts[half(p, voff + 6)])
            if gte.error():
                continue
            otz = gte.avsz4()
            sxy.append(gte.sxy[2])
        else:
            otz = gte.avsz3()
        bucket = (((otz - ot_offset) & MASK32) >> shift) & 0xFFFF
        emitted.append((k, otz, bucket, gpu_packet(name, p, sxy, ton)))
    return emitted


# ---- lit TMD builders (the "L" arm of TmdF3 / TmdTF3 / TmdTG3) ----------
# Lit prim layouts (TMD, light-source calculation on): each vertex word is
# normal index (low half) | vertex index (high half).
#   F3   +4 rgb|mode        +8 n0|v0  +C v1|v2
#   TF3  +4 uv0|cba +8 uv1|tsb +C uv2  +10 n0|v0  +14 v1|v2
#   TG3  +4 uv0|cba +8 uv1|tsb +C uv2  +10 n0|v0  +14 n1|v1  +18 n2|v2
# Flat lit packets take the TMD colour word | GsTON << 25; textured ones
# light 0x808080 (the texture is modulated) under the command byte
# (mode | GsTON << 1).  RTPT; FLAG bit 31; NCLIP; AVSZ3; then NCCS (one
# face normal) or NCCT (three vertex normals), sf=1 lm=1.
LIT = {
    # name: (TMD code, prim bytes, normals per prim, packet words incl. tag)
    'F3L': (0x20, 16, 1, 5),
    'TF3L': (0x24, 24, 1, 8),
    'TG3L': (0x34, 28, 3, 10),
    # the fog arm (TmdTG3 with GsLIGHT_MODE 1): NCDT instead of NCCT
    'TG3LFG': (0x34, 28, 3, 10),
}


def build_lit(name, prims, verts, normals, count, ton, gte, light, ot_offset, shift):
    """Returns [(prim index, otz, bucket, packet body words)] for the emitted
    prims, in emission order."""
    _code, size, _nn, _words = LIT[name]
    out = []
    for k in range(count):
        p = prims[k * size:(k + 1) * size]
        if name == 'F3L':
            a, b = word(p, 8), word(p, 12)
            vi, ni = [a >> 16, b & 0xFFFF, b >> 16], [a & 0xFFFF]
            rgbc = (word(p, 4) | (ton << 25)) & MASK32
        else:
            ws = [word(p, 16 + 4 * i) for i in range(3)]
            if name == 'TF3L':
                vi, ni = [ws[0] >> 16, ws[1] & 0xFFFF, ws[1] >> 16], [ws[0] & 0xFFFF]
            else:
                vi, ni = [w >> 16 for w in ws], [w & 0xFFFF for w in ws]
            rgbc = 0x808080 | (((p[3] | (ton << 1)) & 0xFF) << 24)
        gte.rtpt(*[verts[i] for i in vi])
        if gte.error() or gte.nclip() <= 0:
            continue
        otz = gte.avsz3()
        sxy = list(gte.sxy)
        cols = ncs(gte, light, [normals[i] for i in ni], rgbc, cue=name.endswith('FG'))
        bucket = (((otz - ot_offset) & MASK32) >> shift) & 0xFFFF
        if name == 'F3L':
            body = [cols[-1]] + sxy
        elif name == 'TF3L':
            body = [cols[-1], sxy[0], word(p, 4), sxy[1], word(p, 8), sxy[2], word(p, 12)]
        else:
            body = []
            for i in range(3):
                body += [cols[i], sxy[i], word(p, 4 + 4 * i)]
        out.append((k, otz, bucket, body))
    return out
