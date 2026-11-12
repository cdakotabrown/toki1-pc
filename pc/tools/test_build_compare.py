# SPDX-License-Identifier: GPL-3.0-or-later
"""pc/tools/build_compare.py: the comparison itself (trace rows, PS1 RAM words).

The end-to-end check is a run: `build_compare.py SCRIPT --b <other binary>`
(the 2026-10-31 runs are in docs/pc-port/RENDER-PARITY-HANDOFF.md).  These
tests pin what counts as a difference: any trace field of any row, a
missing row, and any PS1 RAM word except one holding a host code address
of the respective binary in both runs.
"""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_compare as bc  # noqa: E402

ROW = 'frame=%d pad=0000 tick=%d now_step=46 frame_counter=%08x rand_seed=%s crashes=0\n'


class TraceDiff(unittest.TestCase):
    def write(self, rows):
        fd, path = tempfile.mkstemp(suffix='.trace')
        with os.fdopen(fd, 'w') as fh:
            fh.write('# observation=pre_irq\n')
            fh.writelines(rows)
        self.addCleanup(os.unlink, path)
        return path

    def rows(self, n, seed_at=None):
        return [ROW % (100 + i, 1000 * i, i, 'e8488bbd' if seed_at is not None and i >= seed_at
                       else '0b620000') for i in range(n)]

    def test_identical(self):
        self.assertIsNone(bc.first_trace_diff(self.write(self.rows(50)), self.write(self.rows(50))))

    def test_first_differing_field(self):
        n, da, db, keys = bc.first_trace_diff(self.write(self.rows(50)),
                                              self.write(self.rows(50, seed_at=17)))
        self.assertEqual(n, 18)
        self.assertEqual(keys, ['rand_seed'])
        self.assertEqual(da['frame'], '117')

    def test_missing_rows(self):
        n, da, db, keys = bc.first_trace_diff(self.write(self.rows(50)), self.write(self.rows(40)))
        self.assertEqual(n, 41)
        self.assertIn('row count', keys[0])


class RamDiff(unittest.TestCase):
    TEXT_A, TEXT_B = (0x400000, 0x900000), (0x400000, 0x980000)

    def test_host_code_pointers_are_not_differences(self):
        a = (0x56ffa0).to_bytes(4, 'little') + b'\0' * 4
        b = (0x5700e0).to_bytes(4, 'little') + b'\0' * 4
        self.assertEqual(bc.ram_diffs(a, b, self.TEXT_A, self.TEXT_B), [])

    def test_game_words_are(self):
        a = b'\0' * 8 + (0x620b).to_bytes(4, 'little')
        b = b'\0' * 8 + (0x620a).to_bytes(4, 'little')
        self.assertEqual(bc.ram_diffs(a, b, self.TEXT_A, self.TEXT_B), [0x80000008])

    def test_one_side_pointer_is(self):
        a = (0x56ffa0).to_bytes(4, 'little')
        b = (0x0).to_bytes(4, 'little')
        self.assertEqual(bc.ram_diffs(a, b, self.TEXT_A, self.TEXT_B), [0x80000000])


if __name__ == '__main__':
    unittest.main()
