# SPDX-License-Identifier: GPL-3.0-or-later
"""The test platform switch (pc/tools/game_platform.py).

  * PC_TEST_PLATFORM selects the game binary (Linux default, windows = the
    exe); another value is refused;
  * wrap(): a Windows .exe becomes `wine EXE` in a private WINEPREFIX under
    build/wine-test/runs, with DISPLAY / WAYLAND_DISPLAY removed and SDL on
    the dummy drivers unless chosen (nothing reaches the desktop); the
    cleanup stops that prefix's wineserver and deletes it;
  * the Linux game gets the dummy drivers by default, and keeps a driver a
    test chose;
  * negative control: any other program passes through untouched.
No Wine is started: the prefix copy and wineserver are replaced by fakes.
"""
import os
from pathlib import Path
import subprocess
import sys
import unittest
from unittest import mock

import game_platform as gp

TOOLS = Path(__file__).resolve().parent


def platform_of(value):
    env = dict(os.environ, PC_TEST_PLATFORM=value)
    code = 'import game_platform as g; print(g.PLATFORM, g.GAME.name)'
    return subprocess.run([sys.executable, '-c', code], cwd=TOOLS, env=env,
                          capture_output=True, text=True)


class Selection(unittest.TestCase):
    def test_default_and_windows(self):
        self.assertEqual(platform_of('').stdout.split(), ['linux', 'tokimeki-pc'])
        self.assertEqual(platform_of('windows').stdout.split(), ['windows', 'tokimeki-pc.exe'])

    def test_unknown_platform_refused(self):
        r = platform_of('amiga')
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('expected linux or windows', r.stdout + r.stderr)


class Wrap(unittest.TestCase):
    def setUp(self):
        self.base = {'DISPLAY': ':0', 'WAYLAND_DISPLAY': 'wayland-0', 'HOME': '/h', 'X': '1'}

    def test_exe_runs_in_a_private_headless_prefix(self):
        prefix = gp.WINE_DIR / 'runs' / 'fake'
        with mock.patch.object(gp, 'new_prefix', return_value=prefix), \
                mock.patch.object(gp, 'stop_prefix') as stop:
            cmd, env, cleanup = gp.wrap(['/x/tokimeki-pc.exe', '/a'], self.base)
            self.assertEqual(cmd, ['wine', '/x/tokimeki-pc.exe', '/a'])
            self.assertEqual(env['WINEPREFIX'], str(prefix))
            self.assertNotIn('DISPLAY', env)
            self.assertNotIn('WAYLAND_DISPLAY', env)
            self.assertEqual(env['SDL_VIDEODRIVER'], 'dummy')
            self.assertEqual(env['X'], '1')
            self.assertIn('DISPLAY', self.base)              # the caller's dict untouched
            stop.assert_not_called()
            cleanup()
            stop.assert_called_once_with(prefix)

    def test_exe_keeps_a_chosen_driver(self):
        with mock.patch.object(gp, 'new_prefix', return_value=Path('/p')), \
                mock.patch.object(gp, 'stop_prefix'):
            _, env, _ = gp.wrap(['a.EXE'], dict(self.base, SDL_VIDEODRIVER=''))
        self.assertEqual(env['SDL_VIDEODRIVER'], '')
        self.assertNotIn('DISPLAY', env)

    def test_linux_game_defaults_to_dummy(self):
        cmd, env, _ = gp.wrap([gp.LINUX_GAME, 'x'], self.base)
        self.assertEqual(cmd, [str(gp.LINUX_GAME), 'x'])
        self.assertEqual((env['SDL_VIDEODRIVER'], env['SDL_AUDIODRIVER']), ('dummy', 'dummy'))
        _, env, _ = gp.wrap([gp.LINUX_GAME], dict(self.base, SDL_VIDEODRIVER='x11'))
        self.assertEqual(env['SDL_VIDEODRIVER'], 'x11')

    def test_negative_control_other_programs_untouched(self):
        with mock.patch.object(gp, 'new_prefix') as new:
            cmd, env, _ = gp.wrap(['gcc', 'a.exe.c'], self.base)
        new.assert_not_called()
        self.assertEqual(cmd, ['gcc', 'a.exe.c'])
        self.assertIs(env, self.base)


if __name__ == '__main__':
    unittest.main()
