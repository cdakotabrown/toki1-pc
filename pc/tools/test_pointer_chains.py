# SPDX-License-Identifier: GPL-3.0-or-later
"""Pointer-chain views read every level as a 4-byte PS1 cell
(ps1_pointer_cells.adapt_chains, the overlay generator's rewrite).

SHOUGATU shougatu_serifu_step writes `(**D_80143BB4)[0]` with D_80143BB4 a
`u8 ***` cell: it holds the address of a table of 4-byte PS1 pointers to
4-byte PS1 pointers; the console reads table entry 0 (4 bytes), the cell it
names (4 bytes), and stores through that.  `*V` was rewritten as `V[0]`
one level only, so the second `*` read that cell and its neighbour as one
8-byte pointer.

  fix:              the adapted program stores through entry 0;
  negative control: the program with one level rewritten (the old
                    rewrite) dereferences the joined 8-byte value and
                    faults; the text keeps a `u8 **` dereference.
"""
import os
import subprocess
import tempfile
import unittest

from ps1_pointer_cells import adapt_chains, view_types

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

SRC = r'''
#include <stdio.h>
#include <sys/mman.h>
typedef unsigned int u32;
typedef unsigned char u8;
typedef u8 ***D_1__t1;
#define D_1__v1 ((D_1__t1)(unsigned long)*(u32 *)0x80100200ul)
int main(void)
{
    u32 *ram = mmap((void *)0x80000000UL, 0x200000, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    u8 *buf = (u8 *)0x80100100ul;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (ram != (u32 *)0x80000000UL) return 2;
    *(u32 *)0x80100200ul = 0x80100000u;      /* the cell: the table's address */
    *(u32 *)0x80100000ul = 0x80100180u;      /* table[0]: a u8 ** cell */
    *(u32 *)0x80100004ul = 0x80100190u;      /* table[1] */
    *(u32 *)0x80100180ul = 0x80100100u;      /* that cell: the buffer */
    *(u32 *)0x80100184ul = 0x80100110u;      /* its neighbour cell */
    (**D_1__v1)[0] = 'A';
    (**D_1__v1)[1] = 'B';
    printf("%c%c\n", buf[0], buf[1]);
    return 0;
}
'''


def old_rewrite(text, views):
    """The previous _unary_view_derefs: one level of `*V` only."""
    import re
    for v in views:
        text = re.sub(r'(?<![\w)\]])\*\s*\*\s*' + re.escape(v) + r'\b', '*%s[0]' % v, text)
    return text


class Chains(unittest.TestCase):
    def run_c(self, text):
        with tempfile.TemporaryDirectory() as d:
            src, exe = os.path.join(d, 'p.c'), os.path.join(d, 'p')
            with open(src, 'w') as fh:
                fh.write(text)
            subprocess.run(['gcc', '-w', '-O1', '-no-pie', src, '-o', exe], check=True)
            return subprocess.run([exe], capture_output=True, text=True, timeout=30)

    def test_double_deref_reads_both_cells(self):
        views = view_types(SRC)
        self.assertEqual(views, {'D_1__v1': ('u8', 3)})
        out = adapt_chains(SRC, views)
        self.assertNotIn('**D_1__v1', out)
        r = self.run_c(out)
        self.assertEqual((r.returncode, r.stdout), (0, 'AB\n'))

    def test_one_level_rewrite_faults(self):
        views = view_types(SRC)
        out = adapt_chains(old_rewrite(SRC, views), views)
        self.assertIn('(*((u8 **)', out.replace(' ', '').replace('(*((u8**)', '(*((u8 **)'))
        r = self.run_c(out)
        self.assertNotEqual(r.returncode, 0)

    def test_multiplication_is_not_a_deref(self):
        views = {'V__v1': ('u8', 2)}
        self.assertEqual(adapt_chains('x = a * *V__v1;', views),
                         'x = a * ((u8 *)(unsigned long)((u32 *)(V__v1))[0]);')


if __name__ == '__main__':
    unittest.main()
