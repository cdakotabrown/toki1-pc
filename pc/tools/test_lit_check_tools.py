# SPDX-License-Identifier: GPL-3.0-or-later
"""gte_state.gte_regs and lit_retail_check.load_regs: the GTE
register block of a save state is found with InitGeom's DQA or, when the
game set its own (TAIIKU's SetDQA), by ZSF3/ZSF4 alone; the port's .gte
sidecar reads as the same 64 words."""
import os
import struct
import tempfile
import unittest

import gte_state as gs
import lit_retail_check as lit


def block(dqa):
    words = [0] * 64
    words[32 + 26] = 1000           # H
    words[32 + 27] = dqa & 0xFFFF   # DQA
    words[32 + 28] = 0x01400000     # DQB
    words[32 + 29] = 341            # ZSF3
    words[32 + 30] = 256            # ZSF4
    return words


class GteRegsTests(unittest.TestCase):
    def test_initgeom_dqa_preferred(self):
        raw = b'\x55' * 40 + struct.pack('<64I', *block(-3840)) + b'\0' * 12 \
            + struct.pack('<64I', *block(-4194))
        hits = gs.gte_regs(raw)
        self.assertEqual(len(hits), 1)
        self.assertEqual(gs.describe(hits[0][1])['DQA'], -4194)

    def test_game_dqa_found_by_zsf(self):
        raw = b'\x55' * 40 + struct.pack('<64I', *block(-3840))
        hits = gs.gte_regs(raw)
        self.assertEqual(len(hits), 1)
        d = gs.describe(hits[0][1])
        self.assertEqual((d['DQA'], d['DQB'], d['H']), (-3840, 0x01400000, 1000))

    def test_port_sidecar(self):
        with tempfile.TemporaryDirectory() as t:
            p = os.path.join(t, 'x.gte')
            with open(p, 'wb') as fh:
                fh.write(struct.pack('<64I', *block(-3840)))
            self.assertEqual(gs.describe(lit.load_regs(p))['DQA'], -3840)
            with open(p, 'wb') as fh:
                fh.write(b'\0' * 255)
            with self.assertRaises(SystemExit):
                lit.load_regs(p)


if __name__ == '__main__':
    unittest.main()
