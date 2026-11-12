# SPDX-License-Identifier: GPL-3.0-or-later
"""Signed 32-bit PS1 addresses become zero-extended pointers
(pc/tools/adapt_ps1_addresses.py).

On the console `(u8 *)base` with `s32 base = 0x80100000` IS 0x80100000;
gcc on LP64 sign-extends it to 0xFFFFFFFF80100000.  pc/src/main.c used to
fold such a value back at the fault, which repairs a dereference but not a
comparison, a range check or a pointer difference.  Each case compiles a
program with the game flags twice -- the source as written (negative
control) and the adapter's output -- and runs it:

  fix:              the adapted program prints the console's answers;
  negative control: the program as written gets the comparisons wrong
                    (and its dereference faults: no fold here).
"""
import os
import subprocess
import tempfile
import unittest

import adapt_ps1_addresses as adapter

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pc_paths import decomp  # noqa: E402
FLAGS = ['-std=gnu17', '-funsigned-char', '-D_GNU_SOURCE', '-DPC_PORT',
         '-include', 'pc/include/game/types.h', '-I', 'pc/include/game',
         '-I', 'pc/include', '-I', decomp('include')]

PROGRAM = r"""
#include <stdio.h>
#include <sys/mman.h>
#define ENTRY(i) ((u8 *)(base + (i) * 8))
s32 base;
s32 table[2];
static void *as_ptr(void *p) { return p; }
static u8 *pick(s32 a) { return a; }
int main(void)
{
    u8 *ram = mmap((void *)0x80000000UL, 0x200000, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    u8 *lo, *hi, *e;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (ram != (u8 *)0x80000000UL) return 2;
    base = 0x80100000;
    table[1] = 0x80100010;
    lo = ram + 0x100000;
    hi = ram + 0x100100;
    e = ENTRY(2);                                   /* macro body cast */
    printf("in_range %d\n", e >= lo && e < hi);    /* the range check */
    printf("diff %ld\n", (long)(e - lo));           /* pointer difference */
    printf("eq %d\n", (u8 *)table[1] == lo + 16);   /* comparison */
    printf("implicit %d\n", as_ptr(table[1]) == (void *)(lo + 16));
    printf("ret %d\n", pick(table[1]) == lo + 16);
    *(u8 *)(table[1] + 1) = 7;                      /* dereference */
    printf("store %d\n", lo[17]);
    return 0;
}
"""

EXPECT = 'in_range 1\ndiff 16\neq 1\nimplicit 1\nret 1\nstore 7\n'

# TT tt_hit_check / tt_scene_hit_step: an s32 PS1 address passed through
# the overlay generator's unprototyped view of a prototyped function whose
# parameter is a pointer.  No conversion exists in the AST: the int goes
# in a 32-bit register half, the callee reads all 64 bits, and gcc had
# filled them by sign extension (the s32 was also used as a 64-bit value).
CALL_PROGRAM = r"""
#include <stdio.h>
#include <sys/mman.h>
static u8 *lo;
__attribute__((noinline)) s32 hit(u8 *p, s32 k)
{
    return p >= lo && p < lo + 0x100 && p[0] == 7 ? k : -1;
}
#define HIT ((s32 (*)())hit)
s32 cells[4];
long spread;
__attribute__((noinline)) s32 scan(s32 j)
{
    s32 v = cells[j];
    spread += v;                 /* the value is also needed sign-extended */
    return HIT(v, j);
}
int main(void)
{
    u8 *ram = mmap((void *)0x80000000UL, 0x200000, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (ram != (u8 *)0x80000000UL) return 2;
    lo = ram + 0x100000;
    lo[16] = 7;
    cells[1] = 0x80100010;
    printf("hit %d\n", scan(1));
    return 0;
}
"""


def build_and_run(src, work, name):
    exe = os.path.join(work, name)
    r = subprocess.run(['gcc', '-w', '-O2', '-fpermissive', '-no-pie'] + FLAGS +
                       [src, '-o', exe], cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        raise AssertionError(r.stderr)
    return subprocess.run([exe], capture_output=True, text=True, timeout=30)


class AdapterTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.work = self.tmp.name

    def write(self, name, text):
        path = os.path.join(self.work, name)
        with open(path, 'w') as fh:
            fh.write(text)
        return path

    def adapt(self, src):
        out, desc = adapter.adapt(src, FLAGS, self.work)
        path = os.path.join(self.work, 'adapted_' + os.path.basename(src))
        with open(path, 'w') as fh:
            fh.write(out)
        return path, out, desc

    def test_fix_and_negative_control(self):
        src = self.write('prog.c', PROGRAM)
        path, out, desc = self.adapt(src)
        self.assertIn('((u8 *)(unsigned int)(base + (i) * 8))', out)
        self.assertIn('as_ptr((unsigned int)(table[1]))', out)
        self.assertIn('return (unsigned int)(a);', out)
        self.assertTrue(out.startswith('#line 1 "%s"\n' % src))
        self.assertEqual(len(out.splitlines()), len(PROGRAM.splitlines()) + 1)
        r = build_and_run(path, self.work, 'fixed')
        self.assertEqual((r.returncode, r.stdout), (0, EXPECT))
        # negative control: as written, the comparisons are wrong and the
        # dereference faults
        r = build_and_run(src, self.work, 'orig')
        self.assertTrue(r.stdout.startswith('in_range 0\n'), r.stdout)
        self.assertNotEqual(r.returncode, 0)

    def test_unprototyped_call_argument(self):
        """an s32 address passed to a pointer parameter through
        `((s32 (*)())f)` becomes `(void *)(unsigned int)(...)`; a literal,
        an integer parameter and a prototyped call are untouched.
        Negative control: as written the callee gets 0xFFFFFFFF80100010."""
        src = self.write('call.c', CALL_PROGRAM)
        path, out, desc = self.adapt(src)
        self.assertIn('return HIT((void *)(unsigned int)(v), j);', out)
        self.assertEqual(len(desc), 1)
        r = build_and_run(path, self.work, 'call_fixed')
        self.assertEqual((r.returncode, r.stdout), (0, 'hit 1\n'))
        r = build_and_run(src, self.work, 'call_orig')
        self.assertNotEqual(r.stdout, 'hit 1\n')

    def test_line_directives_keep_lines(self):
        body = PROGRAM.replace('    printf("ret %d\\n"',
                               '#line 500\n    printf("ret %d\\n"')
        src = self.write('lined.c', '#line 1 "orig/lined.c"\n' + body)
        path, out, _ = self.adapt(src)
        self.assertTrue(out.startswith('#line 1 "orig/lined.c"\n'))
        self.assertIn('#line 500\n', out)
        self.assertIn('return (unsigned int)(a);', out)
        r = build_and_run(path, self.work, 'lined')
        self.assertEqual((r.returncode, r.stdout), (0, EXPECT))

    def test_unsigned_and_nonnegative_constants_untouched(self):
        src = self.write('u.c', 'u32 a; void f(void) { u8 *p = (u8 *)a; '
                                'u8 *q = (u8 *)16; s32 b; u8 *r = (u8 *)-1; (void)b; }\n')
        _, out, desc = self.adapt(src)
        self.assertIn('(u8 *)a;', out)
        self.assertIn('(u8 *)16;', out)
        self.assertIn('(u8 *)(unsigned int)-1;', out)
        self.assertEqual(len(desc), 1)

    def test_refuses_mixed_macro(self):
        src = self.write('m.c', '#define P(x) ((u8 *)(x))\n'
                                'u8 *f(s32 a, long b) { return P(a) + (P(b) - P(b)); }\n')
        with self.assertRaises(adapter.Refused):
            self.adapt(src)

    def test_refuses_header_macro(self):
        hdr = self.write('h.h', '#define HP(x) ((u8 *)(x))\n')
        src = self.write('h.c', '#include "%s"\nu8 *f(s32 a) { return HP(a); }\n' % hdr)
        with self.assertRaises(adapter.Refused):
            self.adapt(src)

    def test_macro_call_operand(self):
        src = self.write('c.c', '#define CALL ((s32 (*)())g)\n'
                                's32 g(s32 a) { return a; }\n'
                                'int f(u8 *p) { return CALL(1, 2) == p; }\n')
        _, out, _ = self.adapt(src)
        self.assertIn('return (unsigned int)(CALL(1, 2)) == p;', out)


if __name__ == '__main__':
    unittest.main()
