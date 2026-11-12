# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path
import tempfile
import unittest

from ps1_pointer_cells import adapt_pointer_cells
from safe_process import run


class PointerCellTests(unittest.TestCase):
    def test_extern_pointer_array_has_four_byte_stride(self):
        source = '''typedef unsigned char u8;
typedef unsigned int u32;
extern u8 *table[];
int change(void) {
    u8 *p;
    p = table[1];
    if ((unsigned long)p != 0x80154321UL) return 1;
    table[0] = p + 3;
    return 0;
}
'''
        fixture = '''unsigned int table[3] = {0x80123456U, 0x80154321U, 0xdeadbeefU};
extern int change(void);
int main(void) {
    if (change()) return 1;
    return table[0] != 0x80154324U || table[1] != 0x80154321U || table[2] != 0xdeadbeefU;
}
'''
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            (p / 'access.c').write_text(adapt_pointer_cells(source))
            (p / 'test.c').write_text(fixture)
            run(['gcc', '-std=c89', '-O2', '-fno-pie', '-no-pie',
                 str(p / 'access.c'), str(p / 'test.c'), '-o', str(p / 'test')]).require_ok()
            run([str(p / 'test')]).require_ok()

    def test_nested_loads_and_word_sized_stores(self):
        source = '''typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
u32 cells[3];
void change(void) {
    u8 *p;
    p = *(u8 **)(cells);
    if ((unsigned long)p != 0x80123456UL) fail();
    *(u8 **)(cells) = p + 3;
    *(u16 **)(cells) += 2;
    p = *(u8 **)(*(u8 **)(cells + 1) + 4);
    if ((unsigned long)p != 0x80154321UL) fail();
}
'''
        fixture = '''#include <stdlib.h>
u32 nested[2] = {123, 0x80154321U};
void fail(void) { exit(1); }
int main(void) {
    cells[0] = 0x80123456U;
    cells[1] = (u32)(unsigned long)nested;
    cells[2] = 0xdeadbeefU;
    change();
    return cells[0] != 0x8012345dU || cells[2] != 0xdeadbeefU;
}
'''
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            (p / 'test.c').write_text('void fail(void);\n' + adapt_pointer_cells(source) + fixture)
            run(['gcc', '-std=c89', '-O2', '-fno-pie', '-no-pie',
                 str(p / 'test.c'), '-o', str(p / 'test')]).require_ok()
            run([str(p / 'test')]).require_ok()

    def test_typedef_and_struct_cells_keep_neighbor_words(self):
        source = '''typedef unsigned int u32;
typedef struct Packet { u32 words[3]; } Packet;
extern Packet *table[];
u32 cursor[2] = {0x85000000U, 0xdeadbeefU};
int change(void) {
    Packet *p;
    p = *(Packet **)(cursor);
    if ((unsigned long)p != 0x85000000UL) return 1;
    *(Packet **)(cursor) = p + 1;
    *(struct Packet **)(cursor) += 2;
    *(const Packet **)(cursor) -= 1;
    p = table[1];
    table[0] = p + 1;
    return 0;
}
'''
        fixture = '''typedef unsigned int u32;
u32 table[3] = {0x85000000U, 0x85001000U, 0xdeadbeefU};
extern u32 cursor[2];
extern int change(void);
int main(void) {
    if (change()) return 1;
    return cursor[0] != 0x85000018U || cursor[1] != 0xdeadbeefU ||
           table[0] != 0x8500100cU || table[1] != 0x85001000U ||
           table[2] != 0xdeadbeefU;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'access.c').write_text(adapt_pointer_cells(source))
            (path / 'test.c').write_text(fixture)
            run(['gcc', '-std=c89', '-O2', '-fno-strict-aliasing',
                 '-fno-pie', '-no-pie', str(path / 'access.c'),
                 str(path / 'test.c'), '-o', str(path / 'test')]).require_ok()
            run([str(path / 'test')]).require_ok()

    def test_nested_subscript_is_carried_whole(self):
        # TACO taco_rec_step's call: the cell index itself subscripts an
        # array.  Before the balanced match the rewrite closed the cell at
        # the first `]` and emitted `(... + (s[idx)))).k]` (not C); a
        # subscript continued on the next line (TEL tel_slot_disp) was
        # skipped and read 8 bytes.
        source = '''typedef unsigned int u32;
typedef struct { int pad; int k; } S;
extern int *table[];
extern int *other[];
extern S s[];
int read(int idx) {
    return table[s[idx].k][1] + *other[table[0][0]] + *other[(idx + 1)
                                                            % 2];
}
'''
        fixture = '''typedef unsigned int u32;
typedef struct { int pad; int k; } S;
int row0[2] = {1, 7};
int row1[2] = {3, 40};
int leaf = 500;
u32 table[3];
u32 other[2];
S s[2] = {{0, 1}, {0, 0}};
extern int read(int);
int main(void) {
    table[0] = (u32)(unsigned long)row0;
    table[1] = (u32)(unsigned long)row1;
    table[2] = 0xdeadbeefU;
    other[1] = (u32)(unsigned long)&leaf;
    return read(0) != 1040;
}
'''
        out = adapt_pointer_cells(source)
        self.assertNotIn('table[', out.split('read(int idx)')[1])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'access.c').write_text(out)
            (path / 'test.c').write_text(fixture)
            run(['gcc', '-std=c89', '-O2', '-fno-pie', '-no-pie',
                 str(path / 'access.c'), str(path / 'test.c'),
                 '-o', str(path / 'test')]).require_ok()
            run([str(path / 'test')]).require_ok()

    def test_cell_address_left_as_written(self):
        # `&t[i]` names the cell's address; rewriting its subscript into the
        # widening load would take the address of an rvalue (OPTION
        # option_menuf_main1, TACO taco_band_bright / taco_band_chan_set).
        # `a && t[i]` is still an access.
        source = ('extern u8 *t[];\n'
                  'void f(int i) { u8 **p; p = &t[i]; p = & t[0]; '
                  'if (i && t[i]) g(); }\n')
        out = adapt_pointer_cells(source)
        self.assertIn('p = &t[i];', out)
        self.assertIn('p = & t[0];', out)
        self.assertIn('i && ((u8 *)(unsigned long)(*(u32 *)((u32 *)t + (i))))', out)

    def test_comments_and_native_pointers_unchanged(self):
        source = '/* *(u8 **)(addr) */\nvoid f(u8 **p) { *p = 0; }\n'
        self.assertEqual(adapt_pointer_cells(source), source)

    def test_unknown_store_expression_fails(self):
        with self.assertRaises(ValueError):
            adapt_pointer_cells('x = *(u8 **)(addr) = value;')


if __name__ == '__main__':
    unittest.main()
