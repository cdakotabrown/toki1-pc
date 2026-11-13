# SPDX-License-Identifier: GPL-3.0-or-later
"""A memory-card save the game did not write is clamped into the game's
range when it loads (pc/hal/save_sanitize.c).

The game trusts its saves (save_check only sums the bytes, and the sum is
the save's own), and in this port some fields reach native code: the
player's names are copied into small buffers on the native stack, and the
club, album and date-place numbers index tables of pointers.  These tests
load, through the game's own title -> load -> album 1 path
(pc/tools/scenario.py):

* a crafted album -- names without a terminator, club 15, a reserved date
  at place 31 -- and a system file whose last_album is 0x47: the load
  completes, the night schedule runs, and the log says what was clamped;
* the same album as the game wrote it: nothing is clamped (negative
  control: the clamp is a no-op for the game's own saves).

Needs the built port and pc/assets; skipped otherwise.
"""
import os
from pathlib import Path
import shutil
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scenario  # noqa: E402
import tokimeki_album as alb  # noqa: E402

ROOT = Path(scenario.ROOT)
BINARY = ROOT / 'pc/build/tokimeki-pc'
ASSETS = ROOT / 'pc/assets'
OUT = ROOT / 'build/save-sanitize'

SCENARIO = """\
start album
album date=95/5/15
{fields}
expect now_step=0x34 now_sub_step=1 within 900 hold 120
"""

CRAFTED = ('album p_sei=hex:82a082a282a482a6 p_mei=hex:82a882a982ab82ad '
           'p_adana=hex:82af82b182b382b582b782b982bb82bd club=15 reserved.0=0x20104BFF')


def system_last_album(card, value):
    """set the system file's last_album (record byte 38) in both copies,
    with the sums the game checks"""
    blk = alb.find_file(card, 'BISLPS-000650')     # slot 0: the system file
    for off in alb.RECORD_OFFSETS:
        base = blk * alb.BLOCK + off
        rec = card[base:base + alb.RECORD_SIZE]
        if struct.unpack_from('<I', rec, 0)[0] != alb.checksum(rec):
            continue
        rec[38] = value
        struct.pack_into('<I', rec, 0, alb.checksum(rec))
        card[base:base + alb.RECORD_SIZE] = rec


@unittest.skipUnless(BINARY.is_file() and (ASSETS / 'raw/PSX.EXE').is_file(),
                     'needs the built port and pc/assets')
class SaveSanitize(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True, exist_ok=True)

    def run_case(self, name, fields, last_album=None):
        path = OUT / f'{name}.txt'
        path.write_text(SCENARIO.format(fields=fields))
        sc = scenario.parse(str(path))
        real = scenario.build_card

        def build(sc_, template_card, template_ram, syms, dest):
            real(sc_, template_card, template_ram, syms, dest)
            if last_album is not None:
                card = bytearray(Path(dest).read_bytes())
                system_last_album(card, last_album)
                Path(dest).write_bytes(bytes(card))

        scenario.build_card = build
        try:
            res = scenario.run(sc, str(OUT), str(BINARY), 900, False)
        finally:
            scenario.build_card = real
        log = (OUT / name / 'run.log').read_text(errors='replace')
        return res, log

    def test_crafted_save_is_clamped_and_plays(self):
        res, log = self.run_case('crafted', CRAFTED, last_album=0x47)
        self.assertTrue(res.get('reached') and res.get('held'), (res, log[-3000:]))
        # three names, the club, the date place
        self.assertIn('[SAVE] the loaded album had 5 field(s) out of the game\'s range', log)
        self.assertIn('[SAVE] the loaded system file had 1 field(s) out of the game\'s range', log)

    def test_the_games_own_save_is_untouched(self):
        res, log = self.run_case('genuine', '')
        self.assertTrue(res.get('reached') and res.get('held'), (res, log[-3000:]))
        self.assertNotIn('[SAVE]', log)


if __name__ == '__main__':
    unittest.main()
