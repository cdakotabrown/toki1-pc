#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The pad's menu mode survives a touch of the mouse (pc/hal/pad.c
menu_mode_keep, enhanced profile, TOKI_MENU_MODE_KEEP).

The game boots with menu_mode 1: the d-pad cycles through a menu's options.
pad_type_check sets menu_mode 0 (the free hand) when a mouse appears on
port 1 and, as on the console, never puts the old mode back.  On a PC a
bumped mouse presents one, so the pad steered the hand from then on.

  * enhanced: mouse, then a pad button -> menu_mode is 1 again;
  * negative control, TOKI_MENU_MODE_KEEP=0: it stays 0 (the game's rule);
  * parity profile: it stays 0.

The game runs headless from boot with a host-input script
(TOKI_INPUT_SCRIPT); `dump` prints port 1's device and menu_mode.
"""
import os
from pathlib import Path
import re
import shutil
import tempfile
import unittest

from safe_process import run
import game_platform

ROOT = Path(__file__).resolve().parents[2]
BINARY = game_platform.GAME
ASSETS = ROOT / 'pc/assets'

SCRIPT = '''300 dump
320 mouse 6 4
330 mouse 6 4
400 dump
420 pad l2
424 pad none
500 dump
510 quit
'''


def play(folder, name, profile, **sets):
    out = folder / name
    (out / 'saves').mkdir(parents=True)
    (out / 'cfg').mkdir()
    (out / 'keys.txt').write_text(SCRIPT)
    env = {k: v for k, v in os.environ.items() if not k.startswith(('TOKI_', 'PC_'))}
    env.update(TOKI_INPUT_SCRIPT=str(out / 'keys.txt'), TOKI_PROFILE=profile, PC_TURBO='1',
               TOKI_CONFIG_DIR=str(out / 'cfg'), TOKI_SAVE_DIR=str(out / 'saves'),
               SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', **sets)
    r = run([str(BINARY), str(ASSETS)], cwd=ROOT, env=env, timeout=300, max_output=16 * 1024**2)
    dumps = re.findall(r'\[PADSCRIPT\] tick=(\d+) port1=(\w+) .*? menu_mode=(\d+)', r.stdout)
    return [(int(t), dev, int(m) & 3) for t, dev, m in dumps], r


@unittest.skipUnless(BINARY.exists() and (ASSETS / 'raw/PSX.EXE').exists(),
                     'needs the built game and pc/assets')
class MenuModeKeep(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix='menu-mode-keep-', dir=ROOT / 'build'))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def check(self, name, profile, want_after, **sets):
        dumps, r = play(self.tmp, name, profile, **sets)
        self.assertEqual(len(dumps), 3, r.stdout[-2000:])
        (_, d0, m0), (_, d1, m1), (_, d2, m2) = dumps
        self.assertEqual((d0, m0), ('pad', 1), 'boot: the pad cycles through the options')
        self.assertEqual((d1, m1), ('mouse', 0), 'the game sets the hand for a mouse')
        self.assertEqual((d2, m2), ('pad', want_after))

    def test_enhanced_brings_the_mode_back(self):
        self.check('enhanced', 'enhanced', 1)

    def test_negative_control_switched_off(self):
        self.check('off', 'enhanced', 0, TOKI_MENU_MODE_KEEP='0')

    def test_parity_keeps_the_games_rule(self):
        self.check('parity', 'parity', 0)


if __name__ == '__main__':
    unittest.main()
