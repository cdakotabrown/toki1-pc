# SPDX-License-Identifier: GPL-3.0-or-later
"""GsSetFlatLight reads PS1-width light records and writes PS1-width matrices.

The matched C declares Sony's MATRIX { short m[3][3]; long t[3]; } and
GsF_LIGHT { long vx, vy, vz; u8 r, g, b, pad; }.  `long` is 8 bytes on the
host, so the light direction came from +0/+8/+16 of the 16-byte PS1
record and the 48-byte matrix copies overran GsLIGHTWSMATRIX (0x80131740,
32 bytes) into _LC (0x80131760).  src/main/pc-port/GsSetFlatLight.c uses
s32.  The expected light matrix rows / colour columns follow libgs:
row id = -v * 4096 / SquareRoot0(|v|^2) (C division), colour column id =
c * 4096 / 255 -- with retail's SquareRoot0 these give the GsLIGHTWSMATRIX
and LCM read out of a retail TACO save state (rows 4137 / 2904, LCM 4096).

The harness lays GsLIGHTWSMATRIX and _LC out contiguously as in PS1 RAM
and fills the light records as the game does (TACO taco_light_init /
taco_light_set2).  Negative control: GSFL_SOURCE=src/main/matched/
GsSetFlatLight.c.
"""
import os
from pathlib import Path
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

HARNESS = r'''#include "hal.h"
#include <stdio.h>
#include <string.h>

extern void gte_test_tables_init(void);
void debug_log(const char *fmt, ...) { (void)fmt; }
void *hal_ps1_ptr(const void *p) { return (void *)p; }
char libgs_flatlight_len0_msg[] = "GsSetFlatLight len = 0\n";

/* PS1 RAM order: GsLIGHTWSMATRIX at 0x80131740, _LC at 0x80131760. */
struct { s32 lws[8]; s32 lc[8]; s32 guard[4]; } g_ram __asm__("g_ram");
__asm__(".globl GsLIGHTWSMATRIX\n.set GsLIGHTWSMATRIX, g_ram\n"
        ".globl _LC\n.set _LC, g_ram + 32\n");

typedef struct { s32 vx, vy, vz; u8 r, g, b, pad; } Rec;
extern void GsSetFlatLight(s32 id, Rec *lt);

int main(int argc, char **argv)
{
    static Rec rec[3];
    MATRIX lcm;
    s32 vals[3][6];
    int i, k;
    FILE *f;
    (void)argc;
    gte_test_tables_init();
    if (sscanf(argv[1], "%d,%d,%d,%d,%d,%d;%d,%d,%d,%d,%d,%d;%d,%d,%d,%d,%d,%d",
               &vals[0][0], &vals[0][1], &vals[0][2], &vals[0][3], &vals[0][4], &vals[0][5],
               &vals[1][0], &vals[1][1], &vals[1][2], &vals[1][3], &vals[1][4], &vals[1][5],
               &vals[2][0], &vals[2][1], &vals[2][2], &vals[2][3], &vals[2][4], &vals[2][5]) != 18)
        return 2;
    memset(&g_ram, 0, sizeof g_ram);
    for (k = 0; k < 4; k++) g_ram.guard[k] = 0x5a5a5a5a;
    for (i = 0; i < 3; i++) {
        rec[i].vx = vals[i][0]; rec[i].vy = vals[i][1]; rec[i].vz = vals[i][2];
        rec[i].r = (u8)vals[i][3]; rec[i].g = (u8)vals[i][4]; rec[i].b = (u8)vals[i][5];
        rec[i].pad = 0xEE;
        GsSetFlatLight(i, &rec[i]);
    }
    ReadColorMatrix(&lcm);
    f = fopen(argv[2], "w");
    for (k = 0; k < 8; k++) fprintf(f, "%d ", (int)g_ram.lws[k]);
    for (k = 0; k < 8; k++) fprintf(f, "%d ", (int)g_ram.lc[k]);
    for (k = 0; k < 4; k++) fprintf(f, "%d ", (int)g_ram.guard[k]);
    for (k = 0; k < 9; k++) fprintf(f, "%d ", (int)lcm.m[k / 3][k % 3]);
    fclose(f);
    return 0;
}
'''


def cdiv(a, b):
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b > 0) else -q


def halves(words):
    out = []
    for w in words:
        out += [ref.s16(w), ref.s16(w >> 16)]
    return out


class GsSetFlatLightTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        d = Path(cls.temp.name)
        (d / 'h.c').write_text(HARNESS)
        cls.binary = d / 'fl'
        cls.out = d / 'out.txt'
        src = os.environ.get('GSFL_SOURCE', 'src/main/pc-port/GsSetFlatLight.c')
        run(['cc', '-std=gnu17', '-D_GNU_SOURCE', '-O1', '-fno-strict-aliasing', '-w',
             '-Ipc/include', '-Ipc/include/game', '-I' + decomp('include'),
             '-include', 'pc/include/game/types.h',
             *shlex.split(os.environ.get('GSFL_CFLAGS', '')),
             str(d / 'h.c'), 'pc/tests/gte_test_tables.c', 'pc/hal/gte.c', src,
             decomp('src/main/matched/gte_read_lc.c'), 'src/main/pc-port/gte_set_lc.c',
             '-lm', '-o', str(cls.binary)], cwd=ROOT, timeout=60).require_ok()

    def case(self, lights):
        spec = ';'.join(','.join(str(v) for v in l) for l in lights)
        run([str(self.binary), spec, str(self.out)], timeout=30).require_ok()
        vals = [int(x) for x in self.out.read_text().split()]
        lws, lc, guard, lcm = vals[0:8], vals[8:16], vals[16:20], vals[20:29]
        rows, cols = [], [[0] * 3 for _ in range(3)]
        for i, (vx, vy, vz, r, g, b) in enumerate(lights):
            n = ref.square_root0(vx * vx + vy * vy + vz * vz)
            rows.append([cdiv(-v * 4096, n) for v in (vx, vy, vz)])
            for k, c in enumerate((r, g, b)):
                cols[k][i] = c * 4096 // 255
        self.assertEqual(halves(lws[:5])[:9], sum(rows, []), 'GsLIGHTWSMATRIX rows')
        self.assertEqual(halves(lc[:5])[:9], sum(cols, []), '_LC colour columns')
        self.assertEqual(lcm, sum(cols, []), 'GTE LCM = _LC')
        self.assertEqual(guard, [0x5a5a5a5a] * 4, 'nothing written past _LC')

    def test_taco_light_init(self):
        self.case([(0, 0, -100, 255, 255, 255), (0, 100, -100, 255, 255, 255),
                   (-100, 0, -100, 255, 255, 255)])

    def test_taco_light_set2(self):
        self.case([(20, 100, -100, 176, 176, 176), (20, -100, 100, 128, 128, 128),
                   (-20, 20, -100, 96, 96, 96)])

    def test_distinct_components(self):
        self.case([(3, -7, 11, 1, 2, 3), (-500, 250, 125, 200, 100, 50),
                   (1, 1, 1, 255, 0, 128)])


if __name__ == '__main__':
    unittest.main()
