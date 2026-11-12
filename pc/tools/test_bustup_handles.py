# SPDX-License-Identifier: GPL-3.0-or-later
"""The bust-up picture handles are stored at their PS1 width.

bustup_clut / bustup_img / bustup_img2 / bustup_img3 are 32-bit PS1
handles at 0x8012DD14 / DD18 / DD1C / DD20 (pc/ld/psx_globals.syms).  The
matched writers declare them `u8 *`; on LP64 each store is 8 bytes and
also writes the following word.  In the matched order (clut, img3, img,
img2) the img2 store zeroes bustup_img3 and the img3 store zeroes
D_8012DD24.  week_day_bustup_init then gave the lesson-day character
(class_w slot 96) a null header: it never animated, cal_cw_mode bit 0
never cleared, and week_day_main0 ran its 601-frame timeout every lesson
day (retail: 46 frames).

Each case links the function the way pc/Makefile links the port
(--just-symbols=pc/ld/psx_globals.syms, a 2 MB PS1 RAM at 0x80000000
built from pc/assets/raw/PSX.EXE, which holds the four tables) and checks
the stored handles against the retail tables:
bust_up_ptr_set(3) is the first lesson day of the scripted playthrough;
retail stores 0x801E0400 / 0x801E0408 / 0x801E0438 (img3 / img / img2,
read off the reference emulator's trace of cal_cw_hdr / cal_cw_img / cal_cw_anim_tbl).

  fix:              src/main/pc-port/<f>.c stores every handle and leaves
                    D_8012DD24 alone;
  negative control: src/main/matched/<f>.c, linked the same way, zeroes
                    bustup_img3 (and D_8012DD24).
"""
from pathlib import Path
import struct
import tempfile
import unittest

from safe_process import run
from psx_test_ram import PSX_EXE, retail_ram

ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pc_paths import decomp, spell  # noqa: E402
CFLAGS = ['-std=gnu17', '-fpermissive', '-w', '-funsigned-char',
          '-fno-strict-aliasing', '-O2', '-D_GNU_SOURCE', '-DPC_PORT',
          '-include', 'pc/include/game/types.h',
          '-I', 'pc/include/game', '-I', 'pc/include', '-I', decomp('include')]
RAM = 0x80000000
RAM_SIZE = 0x200000
BASE = 0x801E0000
SENTINEL = 0xA5A55A5A

HARNESS = r"""
#include <stdio.h>
#include <sys/mman.h>
extern void ENTRY(int);
int main(int argc, char **argv)
{
    FILE *f;
    unsigned char *ram = mmap((void *)0x80000000UL, 0x200000,
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (ram != (unsigned char *)0x80000000UL) return 2;
    f = fopen(argv[1], "rb");
    if (!f || fread(ram, 1, 0x200000, f) != 0x200000) return 3;
    fclose(f);
    *(unsigned int *)0x8012DD24UL = SENTINEL;
    ENTRY(ARG);
    f = fopen(argv[2], "wb");
    fwrite(ram, 1, 0x200000, f);
    fclose(f);
    return 0;
}
"""

# function -> (table address, entries, arguments to try)
FUNCS = {
    'bust_up_ptr_set': (0x800D0460, 25, (3, 0, 27)),
    'bustup_addr_set': (0x800D04F8, 13, (0, 4, 14)),
    'bustup_addr_set2': (0x800D0548, 32, (4, 31, 33)),
    'bustup_addr_set3': (0x800D0608, 3, (2, 0, 4)),
}


def sh(cmd):
    r = run([str(c) for c in cmd], cwd=ROOT, timeout=120)
    if not r.ok:
        raise RuntimeError('%s\n%s' % (' '.join(map(str, cmd)), r.stdout[-3000:]))
    return r


def handles(func, source, arg, ram):
    with tempfile.TemporaryDirectory() as d:
        tmp = Path(d)
        obj = tmp / 'f.o'
        sh(['gcc'] + CFLAGS + ['-c', ROOT / spell(source), '-o', obj])
        harness = tmp / 'main.c'
        harness.write_text(HARNESS.replace('ENTRY', func).replace('ARG', str(arg))
                           .replace('SENTINEL', '0x%Xu' % SENTINEL))
        exe = tmp / 'run'
        sh(['gcc', '-std=gnu17', '-O0', '-w', '-no-pie', '-o', exe, harness, obj,
            '-Wl,--allow-multiple-definition',
            '-Wl,--just-symbols=%s' % (ROOT / 'pc/ld/psx_globals.syms')])
        image_in, image_out = tmp / 'ram.in', tmp / 'ram.out'
        image_in.write_bytes(ram)
        sh([exe, image_in, image_out])
        return struct.unpack_from('<5I', image_out.read_bytes(), 0x8012DD14 - RAM)


@unittest.skipUnless(PSX_EXE.is_file(), 'needs pc/assets/raw/PSX.EXE')
class BustupHandles(unittest.TestCase):
    RAM_IMAGE = retail_ram() if PSX_EXE.is_file() else b''

    def expected(self, table, entries, arg):
        row = struct.unpack_from('<3H', self.RAM_IMAGE,
                                 table - RAM + 6 * (arg % entries))
        # clut, img, img2, img3, D_8012DD24
        return (BASE, BASE + row[1], BASE + row[2], BASE + row[0], SENTINEL)

    def test_retail_first_lesson_day(self):
        want = self.expected(0x800D0460, 25, 3)
        self.assertEqual(want[1:4], (0x801E0408, 0x801E0438, 0x801E0400))

    def test_override_stores_ps1_handles(self):
        for func, (table, entries, args) in FUNCS.items():
            for arg in args:
                with self.subTest(func=func, arg=arg):
                    got = handles(func, 'src/main/pc-port/%s.c' % func, arg,
                                  self.RAM_IMAGE)
                    self.assertEqual(got, self.expected(table, entries, arg))

    def test_matched_clobbers_img3(self):
        """Negative control: the matched 8-byte stores lose bustup_img3."""
        for func, (table, entries, args) in FUNCS.items():
            with self.subTest(func=func):
                got = handles(func, 'src/main/matched/%s.c' % func, args[0],
                              self.RAM_IMAGE)
                want = self.expected(table, entries, args[0])
                self.assertEqual(got[3], 0)          # img3 zeroed
                self.assertEqual(got[4], 0)          # D_8012DD24 zeroed
                self.assertNotEqual(got, want)


if __name__ == '__main__':
    unittest.main()
