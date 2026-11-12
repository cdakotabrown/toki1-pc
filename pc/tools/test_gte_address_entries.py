# SPDX-License-Identifier: GPL-3.0-or-later
"""Address-named LIBGTE entries reach the HAL's GTE, not return-0 stubs.

GsSortSprite calls ReadGeomScreen (0x800A5174) and RotTransPers4
(0x800A5260).  Until 2026-10-31 the PC link resolved both to `return 0`
stubs from pc/src/missing_funcs_gen.c, so every scaled/rotated sprite got uninitialised
screen coordinates (the lesson screen's rolling stat digits vanished).

The harness projects four corners through RotTransPers4 at z = H (unit
perspective) and checks every output against the GTE arithmetic; the
outputs start as a sentinel, so a stub fails.  GTE_ENTRIES_SOURCE selects
another definition of the two entries for negative controls.
"""
import os
from pathlib import Path
import re
import shlex
import tempfile
import unittest
from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pc_paths import decomp  # noqa: E402

HARNESS = r'''#include "hal.h"
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    static const s16 xs[4] = {-4, 4, -4, 4}, ys[4] = {-8, -8, 8, 8};
    MATRIX m;
    SVECTOR v[4];
    s32 sxy[4], p = 0x5a5a5a5a, flag = 0x5a5a5a5a, sz, i;
    int j;

    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++) m.m[i][j] = (s16)(i == j ? 4096 : 0);
    m.m[0][0] = 2048;                   /* X scale 1/2: a squashed sprite */
    m.t[0] = 100; m.t[1] = 50; m.t[2] = 512;
    SetRotMatrix(&m);
    SetTransMatrix(&m);
    SetGeomScreen(512);
    SetGeomOffset(160, 120);
    if (ReadGeomScreen() != 512) {
        fprintf(stderr, "ReadGeomScreen = %d, not 512\n",
                (int)ReadGeomScreen());
        return 1;
    }
    for (i = 0; i < 4; i++) {
        v[i].vx = xs[i]; v[i].vy = ys[i]; v[i].vz = 0;
        sxy[i] = 0x5a5a5a5a;
    }
    sz = RotTransPers4(&v[0], &v[1], &v[2], &v[3],
                       &sxy[0], &sxy[1], &sxy[2], &sxy[3], &p, &flag);
    for (i = 0; i < 4; i++) {
        s32 want = (u16)(160 + 100 + xs[i] / 2) |
                   ((u32)(u16)(120 + 50 + ys[i]) << 16);
        if (sxy[i] != want) {
            fprintf(stderr, "corner %d: sxy %08x, want %08x\n", (int)i,
                    (unsigned)sxy[i], (unsigned)want);
            return 1;
        }
    }
    if (sz != 512 >> 2 || p == 0x5a5a5a5a || flag == 0x5a5a5a5a) {
        fprintf(stderr, "otz %d p %08x flag %08x\n", (int)sz, (unsigned)p,
                (unsigned)flag);
        return 1;
    }
    return 0;
}
'''


class GteAddressEntryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        (folder / 'harness.c').write_text(HARNESS)
        entries = Path(os.environ.get('GTE_ENTRIES_SOURCE',
                                      ROOT / 'pc/hal/gte_address_entries.c'))
        cls.binary = folder / 'entries'
        run(['cc', '-std=gnu17', '-D_GNU_SOURCE', '-O1', '-fno-strict-aliasing',
             '-ffunction-sections', '-fdata-sections',
             '-Ipc/include', '-Ipc/include/game', '-I' + decomp('include'),
             *shlex.split(os.environ.get('GTE_CFLAGS', '')),
             str(folder / 'harness.c'), 'pc/tests/gte_test_tables.c',
             'pc/hal/gte.c', str(entries),
             '-Wl,--gc-sections', '-lm', '-o', str(cls.binary)],
            cwd=ROOT, timeout=60).require_ok()

    def test_entries_project_through_the_gte(self):
        run([str(self.binary)], timeout=10).require_ok()

    def test_generated_stubs_do_not_shadow_the_entries(self):
        text = (ROOT / 'pc/src/missing_funcs_gen.c').read_text()
        for name in ('ReadGeomScreen', 'RotTransPers4'):
            self.assertIsNone(re.search(r'\b%s\b' % name, text), name)


if __name__ == '__main__':
    unittest.main()
