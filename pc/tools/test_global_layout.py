# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute generated accesses against adjacent PS1-sized storage."""
from collections import defaultdict
from pathlib import Path
import tempfile
import unittest

from generate_overlay_modules import (apply_global_views, canonical_cell_shape,
                                      global_declaration)
from safe_process import run

PS1_VIEW_H = Path(__file__).resolve().parents[1] / 'include' / 'ps1_view.h'


class GlobalLayoutTests(unittest.TestCase):
    def compile_and_run(self, source, extra=()):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            (p / 'test.c').write_text(source)
            # generated views spell `&name` as PS1_VIEW(name) (pc/include/ps1_view.h)
            run(['gcc', '-std=c89', '-O2', '-fno-pie', '-no-pie', '-include', str(PS1_VIEW_H),
                 str(p / 'test.c'), *extra, '-o', str(p / 'test')]).require_ok()
            run([str(p / 'test')]).require_ok()

    def test_pointer_cell_reads_and_stores_preserve_neighbor(self):
        source = '''extern unsigned char *cell;
void change(void) {
    unsigned char *p;
    p = cell;
    if ((unsigned long)p != 0x80123456UL) fail();
    cell = (unsigned char *)0x80154321UL;
}
'''
        shape = canonical_cell_shape('unsigned char * @')
        adapted = apply_global_views(source, {'cell': shape}, {}, defaultdict(dict))
        fixture = '''typedef unsigned int u32;
#include <stdlib.h>
u32 cells[2] = {0x80123456U, 0xdeadbeefU};
extern u32 cell __attribute__((alias("cells")));
void fail(void) { exit(1); }
'''
        self.compile_and_run(fixture + adapted + '''
int main(void) {
    change();
    return cells[0] != 0x80154321U || cells[1] != 0xdeadbeefU;
}
''')

    def test_relocated_array_has_only_retail_alignment(self):
        # A native definition normally gives GCC enough alignment to emit
        # movaps. The linker binds it to a PS1 address ending in 4 instead.
        source = '''#define _GNU_SOURCE
#include <sys/mman.h>
#include <string.h>
''' + global_declaration('data', 'int @[12]') + '''
int data[12] = {0};
void clear(void) { int i; for (i=0;i<12;i++) data[i]=0; }
int main(void) {
    unsigned char *p;
    volatile unsigned char *v;
    p = mmap((void *)0x30000000UL, 4096, PROT_READ|PROT_WRITE,
             MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) return 2;
    memset(p, 0x5a, 56);
    clear();
    v = p;
    return v[3] != 0x5a || v[4] || v[51] || v[52] != 0x5a;
}
'''
        makefile = (Path(__file__).resolve().parents[1] / 'Makefile').read_text()
        flags = next(line.split('=', 1)[1].split() for line in makefile.splitlines()
                     if line.startswith('PS1_LAYOUT_CFLAGS ='))
        self.compile_and_run(source, (*flags, '-Wl,--defsym,data=0x30000004'))


if __name__ == '__main__':
    unittest.main()
