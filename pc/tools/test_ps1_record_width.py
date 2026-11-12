# SPDX-License-Identifier: GPL-3.0-or-later
"""mod_w / srn_w records at their console layout (RECORD-PTR overrides).

mod_init and srn_init (reached on every boot) fill 20- / 40-byte records
whose pointer members are 4-byte PS1 cells; the matched C declares them as
pointers, so on LP64 the record grows (24 / 48 bytes, the pointer 8-aligned)
and every later member moves.  mod_set_gpu (all-s32 view) and
srn_vram_set (raw byte offsets) read the console layout.

Each case compiles the function as pc/Makefile does, links it with a 2 MB
PS1 RAM at 0x80000000 and fills record 1 from a source block at
0x80100000:

  fix:              the override writes the console layout (record 1 at
                    +20 / +40, the PS1 address at +4 / +8, the bytes after
                    it in place);
  negative control: the matched source writes the LP64 layout.
"""
import struct
import unittest

import test_ps1_pointer_reads as reads
import test_ps1_pointer_width as width

S = width.S

HARNESS = r"""
#include <stdio.h>
#include <sys/mman.h>
typedef unsigned int u32;
void FUNC(int idx, u32 *src, int arg);
int main(void)
{
    unsigned char *ram = mmap((void *)0x80000000UL, 0x200000,
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    u32 *src = (u32 *)0x80100000UL;
    unsigned char *rec = (unsigned char *)BASE;
    int i;
    if (ram != (unsigned char *)0x80000000UL) return 2;
    src[0] = 0xA1B2C3D4; src[1] = 0x11223344; src[2] = 0x55667788;
    src[3] = 0x99AABBCC; src[4] = 0xDDEEFF00;
    FUNC(1, src, 1);
    for (i = 0; i < SIZE * 3; i++) printf("%02x", rec[i]);
    printf("\n");
    return 0;
}
"""


def run(func, source, base, size):
    text = (HARNESS.replace('FUNC', func).replace('BASE', '0x%XUL' % base)
            .replace('SIZE', str(size)))
    ok, out = reads.build_and_run(func, source, text)
    return ok, bytes.fromhex(out.split()[-1]) if ok else b''


class ModInit(unittest.TestCase):
    BASE = S['mod_w']

    def check(self, source):
        ok, mem = run('mod_init', source, self.BASE, 20)
        self.assertTrue(ok)
        return mem[20:40]

    def test_override_console_layout(self):
        r = self.check('src/main/pc-port/mod_init.c')
        f0, f4 = struct.unpack_from('<iI', r, 0)
        self.assertEqual((f0, f4), (1, 0x80100010))
        self.assertEqual(r[8], 0xA1)                  # f8 = src[0] >> 24
        self.assertEqual(r[12:16], bytes([0x11, 0x22, 0x33, 0x44]))

    def test_matched_lp64_layout(self):
        r = self.check('src/main/matched/mod_init.c')
        self.assertNotEqual(struct.unpack_from('<I', r, 4)[0], 0x80100010)


class SrnInit(unittest.TestCase):
    BASE = S['srn_w']

    def check(self, source):
        ok, mem = run('srn_init', source, self.BASE, 40)
        self.assertTrue(ok)
        return mem[40:80]

    def test_override_console_layout(self):
        r = self.check('src/main/pc-port/srn_init.c')
        self.assertEqual(struct.unpack_from('<I', r, 8)[0], 0x80100014)   # w8 = src + 5
        self.assertEqual(r[13], 0xC3)                                      # b13
        self.assertEqual(r[14:18], bytes([0x11, 0x22, 0x33, 0x44]))       # b14..b17
        self.assertEqual(struct.unpack_from('<iiI', r, 28),
                         (0xDDEEFF00 - (1 << 32), 0x99AABBCC - (1 << 32), S['mod_w'] + 20))

    def test_matched_lp64_layout(self):
        r = self.check('src/main/matched/srn_init.c')
        self.assertNotEqual(r[14:18], bytes([0x11, 0x22, 0x33, 0x44]))


if __name__ == '__main__':
    unittest.main()
