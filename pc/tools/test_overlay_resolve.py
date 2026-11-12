# SPDX-License-Identifier: GPL-3.0-or-later
"""overlay_resolve: a retail table word may name overlay OR main-binary code.

DATE's date_main0 copies its handler table D_8015BF30 (ps1_function_tables
widens each word through overlay_resolve); the table names overlay handlers
and main-binary ones (cd_wait_timeout 0x8007C814, dec_bg_load_check
0x8007EE3C, ...).  overlay_resolve looked only in the loaded overlay, so
those calls were replaced by the `return 0` no-op and the date's background
load never finished (sub-sub-step 4 forever).

The case compiles pc/hal/overlay.c against a one-entry overlay table and a
one-entry main map and resolves both kinds of address.
  fix:              both resolve; an address in neither gives the no-op;
  negative control: the previous overlay.c (git HEAD~ of this change,
                    passed by path) resolves the main-binary word to the
                    no-op.
"""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pc_paths import DECOMP  # noqa: E402

HARNESS = r"""
#include <stdio.h>
#include "overlay_symtab.h"
#include "../src/psx_main_map.h"
void ClearImage(void *r, unsigned char a, unsigned char b, unsigned char c) {}
unsigned int PadRead(int p) { return 0; }
void goto_step(int s) {}
void draw_sprt(int x, int y, int w, int h, unsigned char u, unsigned char v, int a, int b, int c) {}
void debug_log(const char *f, ...) {}
static int ov_fn(void) { return 1; }
static int main_fn(void) { return 2; }
static const OverlaySym tab[] = { { 0x80140000u, (void *)ov_fn } };
const OverlayTable g_overlay_tables[] = { { "XOV", tab, 1 } };
const int g_overlay_table_count = 1;
const PsxFnMap g_psx_main_map[] = { { 0x8007C814u, (void *)main_fn } };
const int g_psx_main_map_n = 1;
void *overlay_resolve(unsigned int addr);
int main(void)
{
    int (*f)(void);
    hal_overlay_set_loaded("./pc/assets/raw/CDROM/EXEDIR/XOV.EXN");
    f = (int (*)(void))overlay_resolve(0x80140000u); printf("%d ", f());
    f = (int (*)(void))overlay_resolve(0x8007C814u); printf("%d ", f());
    f = (int (*)(void))overlay_resolve(0x80150000u); printf("%d\n", f());
    return 0;
}
"""


def run(source):
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        (d / 'main.c').write_text(HARNESS)
        exe = d / 'run'
        r = subprocess.run(['gcc', '-std=gnu17', '-w', '-D_GNU_SOURCE', '-DPC_PORT',
                            '-include', str(ROOT / 'pc/include/game/types.h'),
                            '-I', str(ROOT / 'pc/include/game'), '-I', str(ROOT / 'pc/include'),
                            '-I', str(ROOT / 'pc/hal'), '-I', str(DECOMP / 'include'),
                            '-no-pie', '-Wl,--unresolved-symbols=ignore-all',
                            '-o', str(exe), str(d / 'main.c'), str(source)],
                           capture_output=True, text=True, cwd=ROOT)
        if r.returncode:
            raise RuntimeError(r.stderr[-3000:])
        return subprocess.run([str(exe)], capture_output=True, text=True).stdout.split()


class OverlayResolve(unittest.TestCase):
    def test_overlay_and_main_words(self):
        self.assertEqual(run(ROOT / 'pc/hal/overlay.c'), ['1', '2', '0'])

    def test_negative_control_previous_lookup(self):
        old = os.environ.get('OVERLAY_SOURCE')
        if not old:
            self.skipTest('set OVERLAY_SOURCE=<previous pc/hal/overlay.c> for the negative control')
        self.assertEqual(run(old), ['1', '0', '0'])


if __name__ == '__main__':
    unittest.main()
