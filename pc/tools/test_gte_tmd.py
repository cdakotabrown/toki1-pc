# SPDX-License-Identifier: GPL-3.0-or-later
"""GTE projection/lighting contracts and the TMD packet builders (the eight
unlit ones and the reached lit F3L/TF3L/TG3L: NCCS/NCCT with the LLM/LCM/BK
loaded through SetLightMatrix/SetColorMatrix/SetBackColor).

Runs the real pc/hal/gte.c and src/main/pc-port/tmd_gte_builders.c through
pc/tests/gte_tmd_harness.c and checks every output word against
pc/tools/gte_reference.py, an independent model written from the GTE and
GPU documentation (and checked word-for-word against retail title packets).

Negative controls: GTE_SOURCE=<old gte.c>, TMD_SOURCE=<old builders>.
Sanitizers: GTE_TMD_CFLAGS='-fsanitize=address,undefined ...'.
"""
import os
from pathlib import Path
import random
import shlex
import struct
import tempfile
import unittest

from safe_process import run
import gte_reference as ref

ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pc_paths import decomp  # noqa: E402
FAMILY_IDS = {'NF3': 0, 'NF4': 1, 'NG3': 2, 'NG4': 3,
              'TNF3': 4, 'TNF4': 5, 'TNG3': 6, 'TNG4': 7}
PKT_WORDS, OT_SLOTS = 1024, 256
PKT_OFF, ORG_OFF = 0x040000, 0x081000


def words(*values):
    return struct.pack('<%dI' % len(values), *[v & 0xFFFFFFFF for v in values])


def unpack(data):
    return list(struct.unpack('<%dI' % (len(data) // 4), data))


class GteTmdTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.dir = Path(cls.temp.name)
        cls.binary = cls.dir / 'gte_tmd'
        gte = Path(os.environ.get('GTE_SOURCE', ROOT / 'pc/hal/gte.c'))
        tmd = Path(os.environ.get('TMD_SOURCE',
                                  ROOT / 'src/main/pc-port/tmd_gte_builders.c'))
        run(['cc', '-std=gnu17', '-D_GNU_SOURCE', '-O1', '-fno-strict-aliasing',
             '-Ipc/include', '-Ipc/include/game', '-I' + decomp('include'),
             '-include', 'pc/include/game/types.h',
             *shlex.split(os.environ.get('GTE_TMD_CFLAGS', '')),
             'pc/tests/gte_tmd_harness.c', 'pc/tests/gte_test_tables.c',
             os.environ.get('GTE_ENTRIES_SOURCE', 'pc/hal/gte_address_entries.c'),
             str(gte), str(tmd), '-lm', '-o', str(cls.binary)],
            cwd=ROOT, timeout=60).require_ok()
        cls.count = 0

    def harness(self, mode, payload):
        type(self).count += 1
        src = self.dir / ('in%d' % self.count)
        dst = self.dir / ('out%d' % self.count)
        src.write_bytes(payload)
        run([str(self.binary), mode, str(src), str(dst)], timeout=30).require_ok()
        return unpack(dst.read_bytes())

    # ---- LIBGTE leaves ------------------------------------------------
    def test_rot_matrix_is_rx_ry_rz_with_retail_rounding(self):
        rng = random.Random(1995)
        angles = [(0, 0, 0), (1024, 0, 0), (0, 1024, 0), (0, 0, 1024),
                  (100, -200, 300), (-1, -2048, 4095), (-4096, 2047, -3000),
                  # title_obj_move, slot 0 at frame_counter 1104
                  (2042, -346, -4418 & 0x7FFF)]
        angles += [tuple(rng.randint(-32768, 32767) for _ in range(3))
                   for _ in range(300)]
        out = self.harness('rot', b''.join(words(*a) for a in angles))
        for i, a in enumerate(angles):
            got = [ref.s32(v) for v in out[9 * i:9 * i + 9]]
            self.assertEqual(got, ref.rot_matrix(*[ref.s16(x) for x in a]),
                             'RotMatrix%s' % (a,))

    def test_square_root_uses_the_table_approximation(self):
        rng = random.Random(65)
        values = [0, 1, 2, 3, 4, 63, 64, 65, 255, 256, 4096, 999999, 1000000,
                  0x10000, 0x7FFFFFFF] + [rng.randint(1, 0x7FFFFFFF)
                                          for _ in range(500)]
        out = self.harness('sqrt', words(*values))
        for i, v in enumerate(values):
            self.assertEqual(ref.s32(out[2 * i]), ref.square_root0(v), v)
            self.assertEqual(ref.s32(out[2 * i + 1]), ref.square_root12(v), v)

    # ---- perspective ---------------------------------------------------
    def rtp_case(self, rot, trans, h, ofx, ofy, groups):
        payload = words(*rot, *trans, h, ofx, ofy, len(groups))
        for g in groups:
            payload += b''.join(words(*v) for v in g)
        out = self.harness('rtp', payload)
        per = 4 * 4 + 6 + 2
        for n, g in enumerate(groups):
            o = out[per * n:per * (n + 1)]
            for i, v in enumerate(g):
                m = ref.Gte(rot, trans, h, ofx, ofy)
                m.rtps(v)
                self.assertEqual(o[4 * i:4 * i + 4],
                                 [m.sxy[2], m.ir0, m.flags(), m.sz[3] >> 2],
                                 'RotTransPers %s' % (v,))
            m = ref.Gte(rot, trans, h, ofx, ofy)
            m.rtpt(*g[:3])
            self.assertEqual(o[16:22], m.sxy + [m.ir0, m.flags(), m.sz[3] >> 2],
                             'RotTransPers3 %s' % (g[:3],))
            m.rtps(g[3])
            self.assertEqual(o[22], m.avsz3(), 'AverageZ3')
            self.assertEqual(o[23], m.avsz4(), 'AverageZ4')

    def test_projection_coordinates_flags_and_depth(self):
        rng = random.Random(1105)
        rot = ref.rot_matrix(300, -700, 1100)
        groups = [[(0, 0, 0), (64, 0, 0), (0, 64, 0), (64, 64, 0)],
                  # behind the eye: SZ clamps to 0 (bit 18 -> bit 31),
                  # divide overflow (bit 17, not part of bit 31)
                  [(0, 0, -3000), (0, 0, -900), (5, 5, -999), (1, 1, -1000)],
                  # far off-screen: SX/SY saturation (bits 14/13 -> bit 31)
                  [(30000, 0, 0), (0, -30000, 0), (-30000, 30000, 0), (0, 0, 0)]]
        groups += [[tuple(rng.randint(-2000, 2000) for _ in range(3))
                    for _ in range(4)] for _ in range(60)]
        self.rtp_case(rot, (12, -40, 1500), 1000, 160, 120, groups)

    def test_depth_uses_initgeom_scale_factors(self):
        # Identity view at z: four SZ of 4000 -> AVSZ4 = 4 * 4000 * 256 >> 12.
        self.rtp_case([4096, 0, 0, 0, 4096, 0, 0, 0, 4096], (0, 0, 4000),
                      1000, 160, 120, [[(0, 0, 0)] * 4])
        # OTZ saturates at 0xFFFF, not 4095 (SZ 60000 -> AVSZ4 15000).
        self.rtp_case([4096, 0, 0, 0, 4096, 0, 0, 0, 4096], (0, 0, 60000),
                      1000, 160, 120, [[(0, 0, 0)] * 4, [(0, 0, 9000)] * 4])

    # ---- TMD builders ---------------------------------------------------
    def make_prims(self, name, rng, quads):
        """quads: list of 4 vertex indices.  Returns prim bytes."""
        code, size, quad, tex, gour, voff = ref.FAMILIES[name]
        nv = 4 if quad else 3
        out = b''
        for q in quads:
            p = bytearray(size)
            p[0:2] = struct.pack('<H', len(quads))
            p[3] = code
            col = lambda: rng.randint(0, 0xFFFFFF)
            if tex:
                for i in range(nv):
                    p[4 + 4 * i:8 + 4 * i] = struct.pack('<I', rng.randint(0, 0xFFFFFFFF))
                rgb0 = 4 + 4 * nv
                n_rgb = nv if gour else 1
                for i in range(n_rgb):
                    p[rgb0 + 4 * i:rgb0 + 4 * i + 4] = struct.pack(
                        '<I', col() | (rng.randint(0, 255) << 24))
            else:
                n_rgb = nv if gour else 1
                for i in range(n_rgb):
                    top = code if i == 0 else rng.randint(0, 255)
                    p[4 + 4 * i:8 + 4 * i] = struct.pack('<I', col() | (top << 24))
            p[voff:voff + 4] = struct.pack('<HH', q[0], q[1])
            p[voff + 4:voff + 6] = struct.pack('<H', q[2])
            if quad:
                p[voff + 6:voff + 8] = struct.pack('<H', q[3])
            out += bytes(p)
        return out

    def tmd_case(self, name, ton, seed):
        rng = random.Random(seed)
        verts = [(-64, -90, 0), (64, -90, 0), (-64, 90, 0), (64, 90, 0),
                 (30000, 0, 0), (-100, -100, 50), (100, -100, 60),
                 (-100, 100, 70), (100, 100, 80), (0, 0, -9000)]
        verts += [tuple(rng.randint(-400, 400) for _ in range(3)) for _ in range(20)]
        quads = [(0, 1, 2, 3),        # front facing
                 (1, 0, 3, 2),        # back facing: NCLIP <= 0
                 (0, 4, 2, 3),        # off screen: FLAG bit 31 after RTPT
                 (0, 1, 2, 4),        # quads: v3 off screen, dropped after RTPS
                 (5, 6, 7, 8),
                 (0, 1, 2, 9)]        # v3 behind the eye
        quads += [tuple(rng.randint(10, len(verts) - 1) for _ in range(4))
                  for _ in range(24)]
        prims = self.make_prims(name, rng, quads)
        rot = ref.rot_matrix(rng.randint(-200, 200), rng.randint(-200, 200),
                             rng.randint(-4096, 4096))
        trans = (rng.randint(-50, 50), rng.randint(-50, 50), 1200)
        shift, ot_offset = 2, 0
        payload = words(FAMILY_IDS[name], len(quads), shift, ton, *rot, *trans,
                        1000, 160, 120, ot_offset, len(verts))
        payload += b''.join(words(*v) for v in verts)
        payload += words(len(prims)) + prims
        out = self.harness('tmd', payload)
        base, cursor = out[0], out[1]
        pkt, org = out[2:2 + PKT_WORDS], out[2 + PKT_WORDS:]

        m = ref.Gte(rot, trans, 1000, 160, 120)
        emitted = ref.build(name, prims, verts, len(quads), ton, m, ot_offset, shift)
        self.assertTrue(emitted, 'case emits nothing')
        self.assertLess(len(emitted), len(quads), 'case culls nothing')
        expect_org = [0xA5000000 | (i * 0x10) for i in range(OT_SLOTS)]
        offset = 0
        for k, otz, bucket, body in emitted:
            addr = (base + PKT_OFF + 4 * offset) & 0xFFFFFF
            old = expect_org[bucket]
            tag = (len(body) << 24) | (old & 0xFFFFFF)
            self.assertEqual(pkt[offset:offset + 1 + len(body)], [tag] + body,
                             '%s prim %d packet' % (name, k))
            expect_org[bucket] = (old & 0xFF000000) | addr
            offset += 1 + len(body)
        # Dropped primitives consume no packet space (retail cursor rule).
        self.assertEqual(cursor, PKT_OFF + 4 * offset, '%s cursor' % name)
        self.assertEqual(org, expect_org, '%s OT' % name)

    def test_nf3(self):
        self.tmd_case('NF3', 0, 1)

    def test_nf4(self):
        self.tmd_case('NF4', 1, 2)

    def test_ng3(self):
        self.tmd_case('NG3', 0, 3)

    def test_ng4(self):
        self.tmd_case('NG4', 1, 4)

    def test_tnf3(self):
        self.tmd_case('TNF3', 1, 5)

    def test_tnf4(self):
        self.tmd_case('TNF4', 0, 6)

    def test_tng3(self):
        self.tmd_case('TNG3', 1, 7)

    def test_tng4(self):
        self.tmd_case('TNG4', 0, 8)

    def test_overlay_rot_matrix_axis_entries(self):
        """TACO_/DATE_RotMatrixX/Y/Z (the overlays' verbatim LIBGTE FGO_04/
        05/06) rotate in place like the listings; they were return-0 stubs."""
        rng = random.Random(1031)
        angles = [0, 1, 1024, -1024, 2048, -1, 4095, -4096, 5000, -70000, 333]
        angles += [rng.randint(-100000, 100000) for _ in range(30)]
        mats = [[4096, 0, 0, 0, 4096, 0, 0, 0, 4096],
                [1024, 0, 0, 0, 1024, 0, 0, 0, 1024],
                [-32768, 32767, 1, 2, -3, 4096, 77, -4096, 32767]]
        mats += [[rng.randint(-32768, 32767) for _ in range(9)] for _ in range(4)]
        cases = [(e, a, m) for e in range(6) for a in angles for m in mats]
        payload = b''.join(words(e, a, *m) for e, a, m in cases)
        out = self.harness('rotaxis', payload)
        for i, (e, a, m) in enumerate(cases):
            got = [ref.s32(v) for v in out[10 * i:10 * i + 9]]
            self.assertEqual(got, ref.rot_axis('xyz'[e % 3], a, m), (e, a, m))
            self.assertEqual(out[10 * i + 9], 1, 'returns its matrix')

    # ---- lit builders (NCCS / NCCT) -------------------------------------
    LIT_IDS = {'F3L': 0, 'TF3L': 1, 'TG3L': 2, 'TG3LFG': 3}

    def make_lit_prims(self, name, rng, tris, nnorm):
        code, size, nn, _words = ref.LIT[name]
        out = b''
        for t in tris:
            p = bytearray(size)
            p[0:2] = struct.pack('<H', len(tris))
            p[3] = code
            ns = [rng.randrange(nnorm) for _ in range(3)]
            if name == 'F3L':
                p[4:8] = struct.pack('<I', rng.randint(0, 0xFFFFFF) | (code << 24))
                p[8:12] = struct.pack('<HH', ns[0], t[0])
                p[12:16] = struct.pack('<HH', t[1], t[2])
            else:
                for i in range(3):
                    p[4 + 4 * i:8 + 4 * i] = struct.pack('<I', rng.randint(0, 0xFFFFFFFF))
                if name == 'TF3L':
                    p[16:20] = struct.pack('<HH', ns[0], t[0])
                    p[20:24] = struct.pack('<HH', t[1], t[2])
                else:
                    for i in range(3):
                        p[16 + 4 * i:20 + 4 * i] = struct.pack('<HH', ns[i], t[i])
            out += bytes(p)
        return out

    def lit_case(self, name, ton, seed, llm, lcm, back, far=(0, 0, 0),
                 dqa=-4194, dqb=0x01400000):
        rng = random.Random(seed)
        verts = [(-64, -90, 0), (64, -90, 0), (-64, 90, 0), (30000, 0, 0)]
        verts += [tuple(rng.randint(-400, 400) for _ in range(3)) for _ in range(24)]
        norms = [(0, 0, -4096), (0, 0, 4096), (4096, 0, 0), (0, -4096, 0),
                 (2896, 2896, 0), (-2896, 0, -2896)]
        norms += [tuple(rng.randint(-4096, 4096) for _ in range(3)) for _ in range(10)]
        tris = [(0, 1, 2),         # front facing
                (1, 0, 2),         # back facing: NCLIP <= 0
                (0, 3, 2)]         # off screen: FLAG bit 31
        tris += [tuple(rng.randint(4, len(verts) - 1) for _ in range(3))
                 for _ in range(30)]
        prims = self.make_lit_prims(name, rng, tris, len(norms))
        rot = ref.rot_matrix(rng.randint(-200, 200), rng.randint(-200, 200),
                             rng.randint(-4096, 4096))
        trans = (rng.randint(-50, 50), rng.randint(-50, 50), 1200)
        shift, ot_offset = 2, 0
        payload = words(self.LIT_IDS[name], len(tris), shift, ton, *rot, *trans,
                        1000, 160, 120, ot_offset, *llm, *lcm, *back, *far,
                        dqa & 0xFFFFFFFF, dqb, len(verts))
        payload += b''.join(words(*v) for v in verts)
        payload += words(len(norms)) + b''.join(words(*v) for v in norms)
        payload += words(len(prims)) + prims
        out = self.harness('lit', payload)
        base, cursor = out[0], out[1]
        pkt, org = out[2:2 + PKT_WORDS], out[2 + PKT_WORDS:]

        m = ref.Gte(rot, trans, 1000, 160, 120)
        m.dqa, m.dqb = dqa, dqb
        light = ref.Light(llm, lcm, [(b << 4) for b in back], [(f << 4) for f in far])
        emitted = ref.build_lit(name, prims, verts, norms, len(tris), ton, m, light,
                                ot_offset, shift)
        self.assertTrue(emitted, 'case emits nothing')
        self.assertLess(len(emitted), len(tris), 'case culls nothing')
        expect_org = [0xA5000000 | (i * 0x10) for i in range(OT_SLOTS)]
        offset = 0
        for k, otz, bucket, body in emitted:
            addr = (base + PKT_OFF + 4 * offset) & 0xFFFFFF
            old = expect_org[bucket]
            tag = (len(body) << 24) | (old & 0xFFFFFF)
            got = pkt[offset:offset + 1 + len(body)]
            self.assertEqual(got, [tag] + body, '%s prim %d packet' % (name, k))
            expect_org[bucket] = (old & 0xFF000000) | addr
            offset += 1 + len(body)
        self.assertEqual(cursor, PKT_OFF + 4 * offset, '%s cursor' % name)
        self.assertEqual(org, expect_org, '%s OT' % name)
        return emitted

    # GsSetFlatLight-shaped matrices: rows = -direction * 4096 / |d|,
    # columns of the colour matrix = light colour * 4096 / 255.
    TACO_LLM = [0, 0, -4655, 3076, -1096, -3267, -1097, -3077, -3267]
    TACO_LCM = [4096] * 9

    def test_f3l(self):
        self.lit_case('F3L', 0, 11, self.TACO_LLM, self.TACO_LCM, (128, 128, 128))

    def test_tf3l(self):
        self.lit_case('TF3L', 0, 12, self.TACO_LLM, self.TACO_LCM, (128, 128, 128))

    def test_tg3l(self):
        self.lit_case('TG3L', 0, 13, self.TACO_LLM, self.TACO_LCM, (128, 128, 128))

    # TAIIKU's fog (taiiku_depth_set: SetDQA / SetDQB / SetFarColor), dumped
    # from the port at the ball-rolling race: DQA -3840, DQB 0x01400000,
    # FC (1, 1, 1) << 4.  A nearer DQB pushes IR0 up so the cue shows.
    def test_tg3lfg(self):
        self.lit_case('TG3LFG', 0, 14, self.TACO_LLM, self.TACO_LCM, (128, 128, 128),
                      (1, 1, 1), -3840, 0x01400000)

    def test_tg3lfg_cues_toward_the_far_colour(self):
        """NCDT, not NCCT: with a bright far colour and a large IR0 the
        colours move toward it (a TG3L emission would not match)."""
        lfg = self.lit_case('TG3LFG', 0, 15, self.TACO_LLM, self.TACO_LCM, (0, 0, 0),
                            (255, 0, 255), -100, 0x00C00000)
        flat = self.lit_case('TG3L', 0, 15, self.TACO_LLM, self.TACO_LCM, (0, 0, 0))
        self.assertNotEqual([b for *_, b in lfg], [b for *_, b in flat])

    def test_lit_semi_transparency_and_colour_saturation(self):
        # Coloured lights, a strong back colour (FIFO saturates at 0xFF,
        # IR at 0x7FFF) and no ambient (back-facing normals clamp to 0).
        llm = [-2896, 0, -2896, 0, 4096, 0, 1000, -3000, 2000]
        lcm = [2827, 2056, 4096, 2827, 0, 1542, 4096, 2056, 0]
        for i, name in enumerate(sorted(self.LIT_IDS)):
            self.lit_case(name, 1, 20 + i, llm, lcm, (255, 200, 0))
            self.lit_case(name, 0, 30 + i, llm, lcm, (0, 0, 0))

    def test_lit_colours_vary(self):
        """Guard against a vacuous model: the lit colours must differ by
        normal (the lighting really participates)."""
        emitted = self.lit_case('TG3L', 0, 40, self.TACO_LLM, self.TACO_LCM, (0, 0, 0))
        cols = {body[0] & 0xFFFFFF for _k, _o, _b, body in emitted}
        self.assertGreater(len(cols), 3)

    def test_semi_transparency_switches_each_family(self):
        for i, name in enumerate(sorted(FAMILY_IDS)):
            self.tmd_case(name, 1 - (i & 1), 100 + i)


if __name__ == '__main__':
    unittest.main()
