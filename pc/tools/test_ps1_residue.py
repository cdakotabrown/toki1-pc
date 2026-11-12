# SPDX-License-Identifier: GPL-3.0-or-later
"""The host-residue mask (pc/tools/ps1_residue.py, pc/hal/parity.c
parity_mask_host_residue; PC_FRAME_HASH_RAM=game).

  * on a synthetic RAM block the mask clears exactly the DR_ENV words past
    the packet length, data[4..14] of every cdq_que record and data[1..3]
    of the set-mode (type 14) ones, and nothing else;
  * the C function (compiled from parity.c alone) clears the same bytes as
    the Python twin, on random blocks;
  * negative controls: a difference inside the DR_ENV packet, in data[0..3]
    of a read or set-filter record (the CD code reads them), in data[0] or the
    flag of a set-mode record, or anywhere else in RAM is still reported.
"""
import os
from pathlib import Path
import random
import tempfile
import unittest

import ps1_residue as pr
from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
SIZE = 0x200000
HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
void parity_mask_host_residue(unsigned char *ram);
int main(int argc, char **argv) {
    static unsigned char ram[0x200000];
    FILE *f = fopen(argv[1], "rb");
    if (!f || fread(ram, 1, sizeof ram, f) != sizeof ram) return 2;
    fclose(f);
    parity_mask_host_residue(ram);
    f = fopen(argv[2], "wb");
    if (!f || fwrite(ram, 1, sizeof ram, f) != sizeof ram) return 3;
    return fclose(f) ? 4 : 0;
}
'''


def block(seed, packet_len=6, types=None):
    rnd = random.Random(seed)
    ram = bytearray(rnd.randbytes(SIZE))
    ram[pr.DRAWENV + 0x1C + 3] = packet_len
    for r, t in enumerate(types or [rnd.choice((0, 2, 6, 13, 14)) for _ in range(48)]):
        ram[pr.CDQ_QUE + 24 * r] = t
    return bytes(ram)


def poke(ram, offset, value=0xA5):
    out = bytearray(ram)
    out[offset] ^= value
    return bytes(out)


class Mask(unittest.TestCase):
    def test_drawenv_words_past_the_packet(self):
        ram = block(1, packet_len=6, types=[0] * 48)
        m = pr.mask(ram)
        tag = pr.DRAWENV + 0x1C
        self.assertEqual(m[pr.DRAWENV:tag + 28], ram[pr.DRAWENV:tag + 28])   # tag + 6 words kept
        self.assertEqual(m[tag + 28:pr.DRAWENV + 92], bytes(92 - 0x1C - 28))
        self.assertEqual(m[pr.DRAWENV + 92:pr.DRAWENV + 200], ram[pr.DRAWENV + 92:pr.DRAWENV + 200])

    def test_full_packet_masks_nothing(self):
        ram = block(2, packet_len=15)
        tag = pr.DRAWENV + 0x1C
        self.assertEqual(pr.mask(ram)[tag:tag + 64], ram[tag:tag + 64])

    def test_cdq_set_mode_records_only(self):
        types = [14 if r % 3 == 0 else 6 for r in range(48)]
        ram = block(3, types=types)
        m = pr.mask(ram)
        for r, t in enumerate(types):
            rec = pr.CDQ_QUE + 24 * r
            with self.subTest(record=r, type=t):
                self.assertEqual(m[rec:rec + 9], ram[rec:rec + 9])        # type, pad, arg, data[0]
                self.assertEqual(m[rec + 23], ram[rec + 23])              # flag
                self.assertEqual(m[rec + 9:rec + 12], bytes(3) if t == 14 else ram[rec + 9:rec + 12])
                self.assertEqual(m[rec + 12:rec + 23], bytes(11))         # data[4..14]

    def test_residue_differences_are_ignored(self):
        ram = block(4, packet_len=6, types=[14] * 48)
        other = poke(poke(ram, pr.DRAWENV + 0x1C + 4 * 8), pr.CDQ_QUE + 24 * 18 + 16)
        self.assertNotEqual(ram, other)
        self.assertEqual(pr.differences(ram, other), [])

    def test_negative_controls_game_bytes_still_differ(self):
        ram = block(5, packet_len=6, types=[14] * 24 + [13] * 24)
        cases = {
            'inside the DR_ENV packet': pr.DRAWENV + 0x1C + 4 * 3,
            'the DRAWENV clip rect': pr.DRAWENV + 2,
            'a type-13 record data[1]': pr.CDQ_QUE + 24 * 30 + 9,
            'a type-13 record data[3]': pr.CDQ_QUE + 24 * 30 + 11,
            'a type-14 record data[0]': pr.CDQ_QUE + 24 * 2 + 8,
            'a type-14 record flag': pr.CDQ_QUE + 24 * 2 + 23,
            'a game global': 0x0EF6BF,
        }
        for name, off in cases.items():
            with self.subTest(name):
                self.assertEqual(pr.differences(ram, poke(ram, off)), [pr.RAM_BASE + (off & ~3)])


class CAgreesWithPython(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        d = Path(cls.tmp.name)
        (d / 'h.c').write_text(HARNESS)
        cls.binary = d / 'h'
        run(['cc', '-std=gnu17', '-O1', '-w', '-ffunction-sections', '-fdata-sections',
             '-I', str(ROOT / 'pc/include'), '-I', str(ROOT / 'pc/include/game'),
             '-I', str(ROOT / 'pc/hal'), '-include', 'types.h',
             str(d / 'h.c'), str(ROOT / 'pc/hal/parity.c'), '-Wl,--gc-sections',
             '-Wl,--unresolved-symbols=ignore-all', '-o', str(cls.binary)],
            timeout=120).require_ok()

    def test_random_blocks(self):
        d = Path(self.tmp.name)
        for seed in range(6):
            with self.subTest(seed=seed):
                ram = block(100 + seed, packet_len=random.Random(seed).randrange(0, 16))
                (d / 'in').write_bytes(ram)
                run([str(self.binary), str(d / 'in'), str(d / 'out')], timeout=30).require_ok()
                self.assertEqual((d / 'out').read_bytes(), pr.mask(ram))
                self.assertNotEqual(pr.mask(ram), ram)          # control: something was masked


if __name__ == '__main__':
    unittest.main()
