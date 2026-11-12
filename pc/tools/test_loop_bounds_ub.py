# SPDX-License-Identifier: GPL-3.0-or-later
"""GCC must not shorten the matched C's loops from out-of-bounds indexing.

The matched C declares views sized for IDO, not for an optimiser that
reasons about object bounds: bg_init's `Save { u8 pad0[4332]; u8
flag[1]; }` over screen_w is indexed flag[i] for i in start..end.  With
GCC's default -faggressive-loop-optimizations, flag[1] is undefined
behaviour, so GCC concluded the loop runs at most once: bg_init(0, 2)
cleared only Bg[0] and screen_w+4332.  Bg slot 1, set by the album screen
(step 19), stayed enabled into name entry and every later screen, and
vblank_function sorted it each frame: its 8bpp cells over the 16bpp
picture in VRAM pages 11-15 were the corrupt message window on the lesson
and holiday screens.  pc/Makefile now compiles with
-fno-aggressive-loop-optimizations.

The test compiles the matched bg_init.c with the CFLAGS line of
pc/Makefile, links it as the port does (psx_globals.syms, a 2 MB PS1 RAM
at 0x80000000), runs bg_init(0, 2) over a dirty Bg/screen_w and checks
both slots and both enable bytes are reset (what IDO's loop does).
Negative control: the same CFLAGS without the flag leave slot 1 alone.
"""
from pathlib import Path
import re
import struct
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pc_paths import DECOMP  # noqa: E402
RAM = 0x80000000
FLAG = '-fno-aggressive-loop-optimizations'

HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
extern void bg_init(int, int);
int main(int argc, char **argv)
{
    FILE *f;
    unsigned char *ram = mmap((void *)0x80000000UL, 0x200000,
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (ram != (unsigned char *)0x80000000UL) return 2;
    memset((void *)0x8012A5B0UL, 0x5A, 72);          /* Bg[0], Bg[1] */
    memset((void *)0x800EF84CUL, 0x08, 2);           /* screen_w+4332/4333 */
    bg_init(0, 2);
    f = fopen(argv[1], "wb");
    fwrite((void *)0x8012A5B0UL, 1, 72, f);
    fwrite((void *)0x800EF84CUL, 1, 2, f);
    fclose(f);
    return 0;
}
"""


def makefile_cflags():
    text = (ROOT / 'pc/Makefile').read_text()
    m = re.search(r'^CFLAGS = (.*?)(?<!\\)\n', text, re.M | re.S)
    flags = m.group(1).replace('\\\n', ' ').split()
    return flags


def run_bg_init(flags):
    with tempfile.TemporaryDirectory() as d:
        tmp = Path(d)
        obj = tmp / 'bg_init.o'
        run(['gcc'] + flags + ['-c', str(DECOMP / 'src/main/matched/bg_init.c'),
                               '-o', str(obj)], cwd=ROOT, timeout=60).require_ok()
        main = tmp / 'main.c'
        main.write_text(HARNESS)
        exe = tmp / 'run'
        run(['gcc', '-O0', '-w', '-no-pie', '-o', str(exe), str(main), str(obj),
             '-Wl,--allow-multiple-definition',
             '-Wl,--just-symbols=%s' % (ROOT / 'pc/ld/psx_globals.syms')],
            cwd=ROOT, timeout=60).require_ok()
        out = tmp / 'out.bin'
        run([str(exe), str(out)], cwd=ROOT, timeout=30).require_ok()
        return out.read_bytes()


class LoopBounds(unittest.TestCase):
    # PrimRec: every member cleared, its pad byte (+19) left as it was
    SLOT = struct.pack('<I', 0x80000000) + bytes(15) + b'\x5a' + bytes(16)
    CLEARED = SLOT * 2 + b'\x80\x80'

    def test_makefile_keeps_the_flag(self):
        self.assertIn(FLAG, makefile_cflags())

    def test_bg_init_clears_both_slots(self):
        self.assertEqual(run_bg_init(makefile_cflags()), self.CLEARED)

    def test_negative_control_without_flag(self):
        flags = [f for f in makefile_cflags() if f != FLAG]
        got = run_bg_init(flags)
        self.assertEqual(got[:36], self.CLEARED[:36])        # slot 0 cleared
        self.assertEqual(got[36:72], b'\x5a' * 36)           # slot 1 untouched
        self.assertEqual(got[72:], b'\x80\x08')


if __name__ == '__main__':
    unittest.main()
