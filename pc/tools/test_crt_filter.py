# SPDX-License-Identifier: GPL-3.0-or-later
"""The presenter's CRT filter, against the real binary (headless).

The filter (pc/hal/present.c pc_present_crt, math in pc/hal/present_crt.c)
is presentation only: it changes how the finished picture is put in the
window and nothing else.  PC_FRAME_HASH hashes the emulated picture (VRAM
and the packed scanout) every frame; PC_PRESENT_HASH hashes the window the
player sees.  TOKI_CRT / TOKI_CRT_CURVATURE select the filter in an
automated run (which ignores settings.ini).

Checked here:
  - off (TOKI_CRT=off, and an unknown name) presents exactly what a run
    without the key presents: every window hash is the same;
  - every preset, flat and curved, changes the presented window and leaves
    every emulated frame hash identical, in both profiles (and the negative
    control: the enhanced profile DOES change the emulated hashes, so the
    comparison can fail);
  - the screenshot hotkey still saves the plain game picture, byte for
    byte the unfiltered run's, while PC_PRESENT_WINDOW_SHOT keeps the
    filtered window;
  - under 2x the filter steps aside (the window is what off shows);
  - the settings menu's CRT FILTER row turns it on live, the game does not
    advance while the menu is open, and an automated run saves nothing.
The math is unit tested in pc/tests/test_crt.c (make -f pc/Makefile test).
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

QUIT_TICK = 300
ACT_TICK = 150
SHOT_TICK = 200


def lines(path):
    return Path(path).read_text().splitlines()


def present(path):
    rows = []
    for line in lines(path):
        parts = line.split()
        rows.append((parts[1], parts[2], parts[3] if len(parts) > 3 else ''))
    return rows


@unittest.skipUnless(BINARY.is_file() and (ASSETS / 'raw/PSX.EXE').is_file(),
                     'needs the built port and pc/assets')
class CrtFilter(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.base = cls.run_port('base', f'{QUIT_TICK} quit\n')

    @classmethod
    def run_port(cls, name, script, **env_extra):
        tmp = Path(cls.tmp.name) / name
        cfg = tmp / 'cfg'
        cfg.mkdir(parents=True)
        settings = cfg / 'settings.ini'
        settings.write_text('[display]\ncrt = off\n')
        before = settings.read_text()
        work = tmp / 'work'
        (work / 'pc').mkdir(parents=True)
        (work / 'pc/assets').symlink_to(ASSETS)
        (tmp / 'input.txt').write_text(script)
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(('PC_', 'TOKI_', 'SCENE', 'AUTOPLAY'))}
        env.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy',
                   TOKI_CONFIG_DIR=str(cfg), TOKI_SETTINGS=str(settings),
                   TOKI_INPUT_SCRIPT=str(tmp / 'input.txt'),
                   PC_PRESENT_HASH=str(tmp / 'present.txt'),
                   PC_FRAME_HASH=str(tmp / 'frame.txt'))
        env.update(env_extra)
        result = run([str(BINARY), str(work / 'pc/assets')], cwd=work, env=env,
                     timeout=300, max_output=64 * 1024**2)
        result.require_ok()
        shot = re.search(r'\[PRESENT\] screenshot: (\S+)', result.stdout)
        return {
            'out': result.stdout,
            'present': present(tmp / 'present.txt'),
            'frames': lines(tmp / 'frame.txt'),
            'shot': Path(shot.group(1)).read_bytes() if shot else None,
            'settings_unchanged': settings.read_text() == before,
            'dir': tmp,
        }

    def window(self, run_):
        return [h for h, _, _ in run_['present']]

    def test_base_run_is_long_enough(self):
        self.assertGreater(len(self.base['frames']), 50)
        self.assertEqual(len(self.base['present']), len(self.base['frames']))
        self.assertNotIn('[PRESENT] crt ', self.base['out'])

    def test_off_presents_exactly_as_before(self):
        for value in ('off', 'trinitron'):
            with self.subTest(value=value):
                run_ = self.run_port('off-' + value, f'{QUIT_TICK} quit\n', TOKI_CRT=value)
                self.assertEqual(run_['present'], self.base['present'])
                self.assertEqual(run_['frames'], self.base['frames'])
                self.assertNotIn('[PRESENT] crt ', run_['out'])

    def test_presets_change_the_window_never_the_game(self):
        for preset, curve in (('lite', '0'), ('aperture', '0'), ('shadow', '0'),
                              ('shadow', '1')):
            with self.subTest(preset=preset, curve=curve):
                run_ = self.run_port(f'{preset}-{curve}', f'{QUIT_TICK} quit\n',
                                     TOKI_CRT=preset, TOKI_CRT_CURVATURE=curve)
                self.assertIn(f'[PRESENT] crt {preset} 640x480', run_['out'])
                # the emulated frames: identical, frame for frame
                self.assertEqual(run_['frames'], self.base['frames'])
                # the window: same count and size, but filtered pictures
                ours, base = self.window(run_), self.window(self.base)
                self.assertEqual(len(ours), len(base))
                self.assertEqual({s for _, s, _ in run_['present']}, {'640x480'})
                changed = sum(a != b for a, b in zip(ours, base))
                self.assertGreater(changed, len(base) // 2)

    def test_negative_control_a_game_change_changes_the_frames(self):
        # the frame-hash comparison above can fail: something that really
        # changes the emulated run (the enhanced profile's timing; a START
        # press on the logo is ignored by the game, measured) shows up
        run_ = self.run_port('enhanced', f'{QUIT_TICK} quit\n', TOKI_PROFILE='enhanced')
        self.assertNotEqual(run_['frames'], self.base['frames'])
        # and the filter is in both profiles
        crt = self.run_port('enhanced-crt', f'{QUIT_TICK} quit\n', TOKI_PROFILE='enhanced',
                            TOKI_CRT='aperture')
        self.assertEqual(crt['frames'], run_['frames'])
        self.assertNotEqual(self.window(crt), self.window(run_))

    def test_screenshot_is_the_plain_picture(self):
        script = f'{SHOT_TICK} key Ctrl+P\n{QUIT_TICK} quit\n'
        plain = self.run_port('shot-plain', script)
        shot_dir = Path(self.tmp.name) / 'shot-crt'
        crt = self.run_port('shot-crt', script, TOKI_CRT='shadow', TOKI_CRT_CURVATURE='1',
                            PC_PRESENT_WINDOW_SHOT=str(shot_dir / 'window.png'))
        self.assertIsNotNone(plain['shot'])
        self.assertEqual(crt['shot'], plain['shot'])
        self.assertEqual(crt['frames'], plain['frames'])
        # the window shot is the filtered window, toast and all
        window = (shot_dir / 'window.png').read_bytes()
        self.assertNotEqual(window, crt['shot'])
        self.assertIn('[PRESENT] window shot:', crt['out'])

    def test_under_2x_the_filter_steps_aside(self):
        small = dict(PC_PRESENT_WINDOW='600x450')
        off = self.run_port('small-off', f'{QUIT_TICK} quit\n', **small)
        on = self.run_port('small-on', f'{QUIT_TICK} quit\n', TOKI_CRT='aperture', **small)
        self.assertEqual({s for _, s, _ in off['present']}, {'600x450'})
        self.assertEqual(on['present'], off['present'])
        self.assertNotIn('[PRESENT] crt ', on['out'])

    def test_menu_turns_it_on_live_and_saves_nothing_here(self):
        # FULLSCREEN, SCALE, FILTER, SCANLINES, CRT FILTER: OFF -> LITE
        run_ = self.run_port('menu', f'{ACT_TICK} key Ctrl+O Down Down Down Down Right '
                                     f'Escape\n{QUIT_TICK} quit\n')
        rows = run_['present']
        base = self.window(self.base)
        first = next(i for i, (_, _, tag) in enumerate(rows) if tag == 'menu')
        after = [h for h, _, tag in rows[first:] if tag != 'menu']
        self.assertEqual([h for h, _, _ in rows[:first]], base[:first])
        self.assertIn('[PRESENT] crt lite 640x480', run_['out'])
        # the game was held while the menu was open: it resumes where it
        # was, every later emulated frame as in the plain run ...
        self.assertEqual(run_['frames'], self.base['frames'])
        self.assertEqual(len(after), len(base) - first)
        # ... and every later window is filtered
        self.assertTrue(all(a != b for a, b in zip(after, base[first:])))
        self.assertIn('[PRESENT] settings not saved: automated run', run_['out'])
        self.assertTrue(run_['settings_unchanged'])


if __name__ == '__main__':
    unittest.main()
