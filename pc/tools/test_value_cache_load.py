# SPDX-License-Identifier: GPL-3.0-or-later
"""value_cache_find_or_load queues the requested file (cd_read_start's 3rd arg).

Retail passes the file in a2 (`or a2,a0,zero` at 0x800577A4, live up to
both `jal cd_read_start`); the matched C declares a two-argument
cd_read_start, so natively the third argument is whatever the host register
holds.  The PC override spells it out.  The harness poisons the host's third
argument register before each call so a dropped argument cannot pass by
accident.

Negative control: VALUE_CACHE_SOURCE=src/main/matched/value_cache_find_or_load.c
fails (the queued file is the poison, not the request).
"""
import os
from pathlib import Path
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pc_paths import DECOMP  # noqa: E402

HARNESS = r'''
#include <stdio.h>

s32 date_effect_init_tbl0 = 0, date_effect_init_tbl1 = 0;
s32 date2_intro_script_tbl0 = 0, date2_intro_script_tbl1 = 0;
u8 bg_change_tbl0 = 0, bg_change_tbl1 = 0;
static long got_kind, got_dest, got_file, calls;
void cd_read_start(long kind, long dest, long file) {
    got_kind = kind; got_dest = dest; got_file = file; calls++;
}
extern s32 value_cache_find_or_load(s32, s32);
/* leave a poison value in the third argument register */
__attribute__((noinline)) long poison(long a, long b, long c) {
    __asm__ volatile("" :: "r"(c)); return a + b + c;
}
int main(void) {
    s32 r;
    int path;
    for (path = 0; path < 2; path++) {
        s32 file = path ? 0x3F00 : 0x3FE9;
        poison(1, 2, 0x5A5A5A5A);
        r = value_cache_find_or_load(file, path);
        printf("path %d r=%d kind=%ld dest=%lx file=%lx\n", path, r, got_kind,
               got_dest, got_file);
        if (r != -1 || got_kind != 10 || (got_dest & 0xFFFFFFFF) != 0x80162000 ||
            (got_file & 0xFFFFFFFF) != (unsigned)file) return 1;
    }
    /* a cached entry is not requested again */
    calls = 0;
    if (value_cache_find_or_load(0x3FE9, 0) != 1 || calls) return 1;
    return 0;
}
'''


class ValueCacheLoadTests(unittest.TestCase):
    def test_requested_file_reaches_cd_read_start(self):
        source = os.environ.get('VALUE_CACHE_SOURCE',
                                str(ROOT / 'src/main/pc-port/value_cache_find_or_load.c'))
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            (p / 'h.c').write_text(HARNESS)
            exe = p / 'h'
            run(['gcc', '-std=gnu17', '-O1', '-w', '-funsigned-char', '-I', str(ROOT / 'pc/include/game'),
                 '-I', str(DECOMP / 'include'), '-include', str(ROOT / 'pc/include/game/types.h'),
                 str(p / 'h.c'), source, '-o', str(exe)]).require_ok()
            r = run([str(exe)])
            self.assertEqual(r.returncode, 0, r.stdout)


if __name__ == '__main__':
    unittest.main()
