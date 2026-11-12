# SPDX-License-Identifier: GPL-3.0-or-later
"""The player settings file can never reach an automated run.

The scene matrix, playthroughs, explorer and parity tools drive the port
through PC_TRACE* / PC_INPUT / TOKI_INPUT_SCRIPT with the SDL dummy driver.
pc/hal/settings.c treats any of those as a harness run and ignores the
settings file, TOKI_SETTINGS and TOKI_SET.  This test hands the real binary a
hostile settings file (every button unbound, the mouse pinned, volume 0,
another profile, fullscreen, stretch) and checks the run sees the defaults
and writes nothing into the config directory.

The parsing, defaults, overrides, remapping and scaling math themselves are
unit-tested in pc/tests/test_settings.c (make -f pc/Makefile test).
Needs the built port and pc/assets; skipped otherwise.
"""
import os
from pathlib import Path
import re
import tempfile
import unittest

from safe_process import run
import game_platform  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
BINARY = game_platform.GAME   # PC_TEST_PLATFORM=windows: the .exe under Wine
ASSETS = ROOT / 'pc/assets'

HOSTILE = """
profile = "enhanced"
[display]
fullscreen = true
scale = stretch
scanlines = 100
[audio]
master = 0
voice = 0
[input]
device = mouse
mouse = false
controller = false
[keyboard]
circle = none
cross = none
up = none
[hotkeys]
quit = "Ctrl+Q"
"""


@unittest.skipUnless(BINARY.is_file() and (ASSETS / 'raw/PSX.EXE').is_file(),
                     'needs the built port and pc/assets')
class HarnessIgnoresPlayerSettings(unittest.TestCase):
    def run_port(self, extra_env, script):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            cfg = tmp / 'cfg'
            cfg.mkdir()
            settings = cfg / 'settings.ini'
            settings.write_text(HOSTILE)
            before = settings.stat().st_mtime_ns
            work = tmp / 'work'
            (work / 'pc').mkdir(parents=True)
            (work / 'pc/assets').symlink_to(ASSETS)
            (tmp / 'input.txt').write_text(script)
            env = {k: v for k, v in os.environ.items()
                   if not k.startswith(('PC_', 'TOKI_', 'SCENE', 'AUTOPLAY'))}
            env.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy',
                       TOKI_CONFIG_DIR=str(cfg), TOKI_SETTINGS=str(settings),
                       TOKI_SET='audio.master=0;profile=enhanced',
                       TOKI_INPUT_SCRIPT=str(tmp / 'input.txt'), **extra_env)
            result = run([str(BINARY), str(work / 'pc/assets')], cwd=work, env=env,
                         timeout=120, max_output=64 * 1024**2)
            out = result.stdout
            self.assertEqual(sorted(p.name for p in cfg.iterdir()), ['settings.ini'])
            self.assertEqual(settings.stat().st_mtime_ns, before)
            return out

    def check(self, out):
        self.assertRegex(out, r'\[SETTINGS\] automated run \(\w+\): user settings ignored')
        self.assertNotIn('[SPU] player volume', out)
        self.assertNotIn('[PRESENT] window', out)   # the historical 640x480 window
        dumps = re.findall(r'\[PADSCRIPT\] tick=(\d+) port1=(\w+)', out)
        self.assertTrue(dumps, out[-2000:])
        # input.device=mouse in the file would pin port 1 to a mouse
        self.assertEqual({port for _, port in dumps}, {'pad'})

    def test_input_script_run_sees_defaults(self):
        out = self.run_port({}, '20 pad cross\n40 dump\n41 pad none\n60 dump\n70 quit\n')
        self.check(out)

    def test_trace_frames_run_sees_defaults(self):
        out = self.run_port({'PC_TRACE_FRAMES': '90', 'PC_TURBO': '1'},
                            '40 dump\n80 dump\n')
        self.check(out)


if __name__ == '__main__':
    unittest.main()
