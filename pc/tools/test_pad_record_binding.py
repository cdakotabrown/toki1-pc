# SPDX-License-Identifier: GPL-3.0-or-later
"""The pad HAL writes the libpad records the game reads.

The matched game reads port 1/2 through the names pad1_buf / pad2_buf.  In
the game build pc/src/layout_aliases.ld binds those names to their retail
offsets inside screen_w, which overrides pc/src/input_globals.c's views of
ps1_pad1_rec / ps1_pad2_rec.  This links the real pad_device.c and
input_globals.c with the pad lines of the real layout_aliases.ld against a
screen_w-sized block and checks that a pressed button lands in the bytes
the game reads.

Negative control: PAD_DEVICE_SOURCE=<older pad_device.c> (the version that
wrote ps1_pad1_rec) fails: the game-visible record stays all zero.
"""
import os
from pathlib import Path
import re
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pc_paths import DECOMP  # noqa: E402

HARNESS = r'''
#include <stdio.h>
#include "types.h"
#include "pad_device.h"
u8 screen_w[16384];
extern u8 pad1_buf[8], pad2_buf[8];
int main(void) {
    pad_device_config cfg = { 1.0, 1, PADDEV_PAD, 1 };
    u8 *p1 = screen_w + 0x2970, *p2 = screen_w + 0x29B0;
    if (pad1_buf != p1 || pad2_buf != p2) { puts("binding"); return 3; }
    pad_device_init(&cfg);
    pad_device_pad_buttons(0x0800u | 0x0040u);  /* START + CROSS (HAL bits) */
    pad_device_tick();
    printf("%02x %02x %02x %02x | %02x %02x\n", p1[0], p1[1], p1[2], p1[3],
           p2[0], p2[1]);
    /* status answered, digital pad, START (byte 2 bit 3) and CROSS
     * (byte 3 bit 6) low; port 2 empty */
    return !(p1[0] == 0x00 && p1[1] == 0x41 && p1[2] == 0xF7 &&
             p1[3] == 0xBF && p2[0] == 0xFF && p2[1] == 0xFF);
}
'''


class PadRecordBindingTests(unittest.TestCase):
    def test_pressed_button_reaches_game_record(self):
        aliases = (ROOT / 'pc/src/layout_aliases.ld').read_text()
        lines = [l for l in aliases.splitlines()
                 if re.match(r'pad[12]_buf = screen_w \+ 0x[0-9A-F]+;', l)]
        self.assertEqual(len(lines), 2, 'layout_aliases.ld no longer binds pad1_buf/pad2_buf')
        source = os.environ.get('PAD_DEVICE_SOURCE', str(ROOT / 'pc/hal/pad_device.c'))
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            (p / 'h.c').write_text(HARNESS)
            (p / 'pad.ld').write_text('\n'.join(lines) + '\n')
            exe = p / 'h'
            run(['gcc', '-std=gnu17', '-fno-pie', '-no-pie', '-w', '-funsigned-char',
                 '-I', str(ROOT / 'pc/include/game'), '-I', str(ROOT / 'pc/include'),
                 '-I', str(DECOMP / 'include'), '-include', str(ROOT / 'pc/include/game/types.h'),
                 str(p / 'h.c'), source, str(ROOT / 'pc/src/input_globals.c'),
                 str(p / 'pad.ld'), '-o', str(exe)]).require_ok()
            r = run([str(exe)])
            self.assertEqual(r.returncode, 0, f'game-visible record wrong: {r.stdout!r}')


if __name__ == '__main__':
    unittest.main()
