# SPDX-License-Identifier: GPL-3.0-or-later
"""GsGetLw / GsGetLws / GsGetLs over a parent chain (GsCOORDINATE2.super).

The matched libgs bodies read the 32-bit PS1 parent handle as a host
pointer (sign-extended, or 8 bytes wide), so any coordinate with a super
walked garbage on the host.  pc/tests/gs_coord_harness.c runs the port's
adapters (src/main/pc-port/GsGetL*.c + gs_coord_walk.c) and the matched
GsMulCoord2/3 over the HAL; this test checks the matrices and the cached
workm/flg updates against the retail walk modelled below.

Negative control: COORD_SOURCES='src/main/matched/GsGetLw.c
src/main/matched/GsGetLws.c src/main/matched/GsGetLs.c' must fail.
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
PORT = ['src/main/pc-port/GsGetLw.c', 'src/main/pc-port/GsGetLws.c',
        'src/main/pc-port/GsGetLs.c', 'src/main/pc-port/gs_coord_walk.c']


def mat_words(rot, t):
    halves = list(rot) + [0]
    w = [(halves[2 * i] & 0xFFFF) | ((halves[2 * i + 1] & 0xFFFF) << 16)
         for i in range(5)]
    return [ref.s32(x) for x in w] + list(t)


def mat_parse(w):
    halves = []
    for x in w[:5]:
        halves += [ref.s16(x), ref.s16(x >> 16)]
    return halves[:9], [ref.s32(x) for x in w[5:8]]


def mulrot(a, b):
    return [max(-0x8000, min(0x7FFF, sum(a[3 * i + k] * b[3 * k + j]
                                         for k in range(3)) >> 12))
            for i in range(3) for j in range(3)]


def apply_lv(r, v):
    return [ref.s32(sum(r[3 * i + k] * v[k] for k in range(3)) >> 12)
            for i in range(3)]


def mulcoord3(a, b):
    """GsMulCoord3(a0, a1): a0 = a0 * a1 (rotation), a0.t += a0.R * a1.t."""
    (ar, at), (br, bt) = a, b
    local = apply_lv(ar, bt)
    return mulrot(ar, br), [ref.s32(x + y) for x, y in zip(local, at)]


def mulcoord2(a, b):
    """GsMulCoord2(a0, a1): a1 = a0 * a1, a1.t = a0.R * a1.t + a0.t."""
    (ar, at), (br, bt) = a, b
    local = apply_lv(ar, bt)
    return mulrot(ar, br), [ref.s32(x + y) for x, y in zip(local, at)]


def walk(recs, q):
    """Retail GsGetLw chain walk; mutates recs' flg/workm like retail."""
    stack, final, node = [], 100, q
    while True:
        stack.append(node)
        if recs[node]['flg'] == 0:
            final = len(stack) - 1
        if recs[node]['super'] < 0:
            break
        node = recs[node]['super']
    counter = len(stack) - 1
    if counter == 0:
        out = recs[stack[0]]['coord']
    elif final == 100:
        counter, out = 0, recs[stack[0]]['workm']
    elif final == counter:
        out = recs[stack[counter]]['coord']
    else:
        counter = final + 1
        out = recs[stack[final + 1]]['workm']
    while counter > 0:
        r = recs[stack[counter - 1]]
        out = mulcoord3(out, r['coord'])
        r['workm'], r['flg'] = out, 1
        counter -= 1
    return out


class GsCoordTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.dir = Path(cls.temp.name)
        cls.binary = cls.dir / 'coord'
        sources = shlex.split(os.environ.get('COORD_SOURCES', '')) or PORT
        run(['cc', '-std=gnu17', '-D_GNU_SOURCE', '-O1', '-fno-strict-aliasing',
             '-w', '-Ipc/include', '-Ipc/include/game', '-I' + decomp('include'),
             '-include', 'pc/include/game/types.h',
             *shlex.split(os.environ.get('COORD_CFLAGS', '')),
             'pc/tests/gs_coord_harness.c', 'pc/tests/gte_test_tables.c',
             'pc/hal/gte.c', decomp('src/main/matched/GsMulCoord2.c'),
             decomp('src/main/matched/GsMulCoord3.c'), *sources, '-lm',
             '-o', str(cls.binary)], cwd=ROOT, timeout=60).require_ok()
        cls.n = 0

    def case(self, recs, q, ws):
        type(self).n += 1
        src, dst = self.dir / ('i%d' % self.n), self.dir / ('o%d' % self.n)
        words = mat_words(*ws) + [len(recs)]
        for r in recs:
            words += ([r['flg']] + mat_words(*r['coord']) + mat_words(*r['workm'])
                      + [0, r['super']])
        words.append(q)
        src.write_bytes(struct.pack('<%di' % len(words), *words))
        result = run([str(self.binary), str(src), str(dst)], timeout=30)
        self.assertTrue(result.ok, 'harness failed: %s' % (result.reason or result.returncode))
        out = list(struct.unpack('<%di' % (len(dst.read_bytes()) // 4), dst.read_bytes()))

        expect_lw = walk([dict(r) for r in recs], q)
        light_recs = [dict(r) for r in recs]
        expect_light = walk(light_recs, q)
        expect_screen = mulcoord2(ws, expect_light)
        final_recs = [dict(r) for r in recs]
        expect_ls = mulcoord2(ws, walk(final_recs, q))
        self.assertEqual(mat_parse(out[0:8]), mat_parse(mat_words(*expect_lw)),
                         'GsGetLw')
        self.assertEqual(mat_parse(out[8:16]), mat_parse(mat_words(*expect_light)), 'GsGetLws light')
        self.assertEqual(mat_parse(out[16:24]), mat_parse(mat_words(*expect_screen)), 'GsGetLws screen')
        self.assertEqual(mat_parse(out[24:32]), mat_parse(mat_words(*expect_ls)), 'GsGetLs')
        for i, r in enumerate(final_recs):
            w = out[32 + 19 * i:32 + 19 * (i + 1)]
            self.assertEqual(w[0], r['flg'], 'flg %d' % i)
            self.assertEqual(mat_parse(w[9:17]), mat_parse(mat_words(*r['workm'])), 'workm %d' % i)

    def chain(self, rng, flgs):
        recs = []
        for i, flg in enumerate(flgs):
            rot = ref.rot_matrix(*[rng.randint(-2048, 2048) for _ in range(3)])
            t = [rng.randint(-3000, 3000) for _ in range(3)]
            junk = (ref.rot_matrix(1, 2, 3), [7, 8, 9])
            recs.append({'flg': flg, 'coord': (rot, t), 'workm': junk,
                         'super': i + 1 if i + 1 < len(flgs) else -1})
        return recs

    def test_single_coordinate(self):
        rng = random.Random(1)
        ws = (ref.rot_matrix(0, 2048, 0), [0, 0, 1000])
        self.case(self.chain(rng, [0]), 0, ws)
        self.case(self.chain(rng, [1]), 0, ws)

    def test_parent_chain_recomputes_stale_links(self):
        rng = random.Random(2)
        ws = (ref.rot_matrix(100, 2000, -50), [10, -20, 1500])
        for flgs in ([0, 0, 0], [1, 1, 1], [1, 0, 1], [0, 1, 1], [1, 1, 0],
                     [0, 0, 1, 1, 0]):
            self.case(self.chain(rng, flgs), 0, ws)

    def test_query_from_the_middle_of_a_chain(self):
        rng = random.Random(3)
        ws = (ref.rot_matrix(0, 0, 0), [0, 0, 0])
        self.case(self.chain(rng, [0, 0, 0, 0]), 1, ws)


if __name__ == '__main__':
    unittest.main()
