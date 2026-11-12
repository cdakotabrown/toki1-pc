# SPDX-License-Identifier: GPL-3.0-or-later
"""Regression for TT's packed animation table and cel-control branches."""
from pathlib import Path
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]


class TtCelTests(unittest.TestCase):
    def test_four_byte_tables_and_all_control_branches(self):
        source = '''#define _GNU_SOURCE
#include <sys/mman.h>
#include <string.h>
#include "types.h"
''' + f'#include "{ROOT}/pc/src/overlay/TT/anim_cel_step.c"\n' + '''
u32 tt_anim_tbl[2];
int main(void) {
    u8 *memory;
    Cel *cels;
    Obj o;
    memory = mmap((void *)0x86000000UL, 4096, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (memory == MAP_FAILED) return 1;
    if (sizeof(Anim) != 4 || sizeof(Cel) != 4) return 2;
    tt_anim_tbl[0] = 0xdeadbeefU;
    tt_anim_tbl[1] = (u32)(unsigned long)memory;
    *(u32 *)memory = (u32)(unsigned long)(memory + 16);
    *(u32 *)(memory + 4) = 0xdeadbeefU;
    cels = (Cel *)(memory + 16);
    cels[0].dur = 3;
    cels[1].dur = 5;
    cels[2].dur = 7;
    cels[255].dur = 9;
    memset(&o, 0, sizeof(o));
    o.anim = 1;
    o.flags = 0x82;
    o.frame = 99;
    anim_cel_step(&o);
    if (o.flags != 2 || o.frame || o.timer != 3) return 3;
    o.flags = 0x22;
    o.frame = 1;
    anim_cel_step(&o);
    if (o.flags != 2 || o.frame != 1 || o.timer != 5) return 4;
    o.flags = 0x42;
    anim_cel_step(&o);
    if (o.flags != 0x42 || o.timer != 5) return 5;
    o.flags = 2;
    anim_cel_step(&o);
    if (o.frame != 1 || o.timer != 4) return 6;
    o.timer = 1;
    anim_cel_step(&o);
    if (o.frame != 2 || o.timer != 7) return 7;
    cels[2].ctrl = 255;
    o.timer = 1;
    anim_cel_step(&o);
    if (o.flags != 3 || o.frame != 2 || o.timer) return 8;
    cels[2].ctrl = 1;
    o.timer = 1;
    anim_cel_step(&o);
    if (o.frame != 1 || o.timer != 5) return 9;
    o.frame = 0;
    o.timer = 1;
    cels[0].ctrl = 1;
    anim_cel_step(&o);
    if (o.frame != 255 || o.timer != 9) return 10;
    if (*(u32 *)(memory + 4) != 0xdeadbeefU) return 11;
    return munmap(memory, 4096) != 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.c').write_text(source)
            run(['gcc', '-std=c89', '-O2', '-fno-strict-aliasing',
                 '-fno-pie', '-no-pie', '-I', str(ROOT / 'pc/include/game'),
                 str(path / 'test.c'), '-o', str(path / 'test')]).require_ok()
            run([str(path / 'test')]).require_ok()


if __name__ == '__main__':
    unittest.main()
