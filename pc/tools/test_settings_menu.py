# SPDX-License-Identifier: GPL-3.0-or-later
"""The in-game settings menu, against the real binary (headless).

The menu (pc/hal/menu.c, drawn and driven by pc/hal/present.c) is a
presenter overlay: it never draws into PS1 VRAM and holds the game in the
pause loop while it is open.  PC_PRESENT_HASH logs a hash of every window
the presenter shows, tagged `menu` while the menu is open; TOKI_INPUT_SCRIPT
`key` lines queue real SDL key events (the same path as typed keys).

Checked here:
  - a run that never presses the menu key shows no menu frame at all;
  - opening the menu, moving through it and closing it without a change
    inserts menu frames and nothing else: every other frame, in order, is
    the closed run's (the game did not advance, the keys never reached it,
    and closing restores the picture exactly);
  - each key press redraws the menu (the selection moves);
  - a change applies live (scanlines show after the menu closes) and, this
    being an automated run, is NOT written: the settings file and the config
    directory stay untouched.
The model, the write-back into settings.ini and the input hold-off are unit
tested in pc/tests/test_settings_menu.c (make -f pc/Makefile test).
Needs the built port and pc/assets; skipped otherwise.
"""
import os
from pathlib import Path
import tempfile
import unittest

from safe_process import run
import game_platform  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
BINARY = game_platform.GAME   # PC_TEST_PLATFORM=windows: the .exe under Wine
ASSETS = ROOT / 'pc/assets'

QUIT_TICK = 300
NOTE_FRAMES = 96           # "Settings not saved" is an error note (shown with toasts off): 1600 ms at 60 frames/s
OPEN_TICK = 150


def hashes(path):
    rows = []
    for line in Path(path).read_text().splitlines():
        parts = line.split()
        rows.append((parts[1], parts[3] if len(parts) > 3 else ''))
    return rows


@unittest.skipUnless(BINARY.is_file() and (ASSETS / 'raw/PSX.EXE').is_file(),
                     'needs the built port and pc/assets')
class SettingsMenu(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.closed = cls.run_port('closed', f'{QUIT_TICK} quit\n')

    @classmethod
    def run_port(cls, name, script, packs=()):
        tmp = Path(cls.tmp.name) / name
        cfg = tmp / 'cfg'
        cfg.mkdir(parents=True)
        for entry in packs:                  # 'name/' a folder, else a file
            path = cfg / 'textures/packs' / entry.rstrip('/')
            path.parent.mkdir(parents=True, exist_ok=True)
            if entry.endswith('/'):
                path.mkdir()
            else:
                path.write_text('not a pack')
        settings = cfg / 'settings.ini'
        settings.write_text('# the player\'s file\n[display]\nscanlines = 0\n')
        before = settings.stat().st_mtime_ns
        work = tmp / 'work'
        (work / 'pc').mkdir(parents=True)
        (work / 'pc/assets').symlink_to(ASSETS)
        (tmp / 'input.txt').write_text(script)
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(('PC_', 'TOKI_', 'SCENE', 'AUTOPLAY'))}
        env.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy',
                   TOKI_CONFIG_DIR=str(cfg), TOKI_SETTINGS=str(settings),
                   TOKI_INPUT_SCRIPT=str(tmp / 'input.txt'),
                   PC_PRESENT_HASH=str(tmp / 'hash.txt'))
        result = run([str(BINARY), str(work / 'pc/assets')], cwd=work, env=env,
                     timeout=180, max_output=64 * 1024**2)
        result.require_ok()
        return {
            'cfg_dir': cfg,
            'out': result.stdout,
            'frames': hashes(tmp / 'hash.txt'),
            'cfg': sorted(p.name for p in cfg.iterdir()),
            'settings_unchanged': settings.stat().st_mtime_ns == before,
        }

    def test_closed_run_never_shows_the_menu(self):
        run_ = self.closed
        self.assertGreater(len(run_['frames']), 50)
        self.assertNotIn('menu', {tag for _, tag in run_['frames']})
        self.assertNotIn('[PRESENT] settings menu', run_['out'])

    def test_menu_pauses_the_game_and_restores_the_picture(self):
        run_ = self.run_port('browse', f'{OPEN_TICK} key Ctrl+O Down Down Escape\n'
                                       f'{QUIT_TICK} quit\n')
        menu = [h for h, tag in run_['frames'] if tag == 'menu']
        rest = [h for h, tag in run_['frames'] if tag != 'menu']
        self.assertIn('[PRESENT] settings menu open', run_['out'])
        self.assertIn('[PRESENT] settings menu closed', run_['out'])
        # opened, then one redraw per Down; Escape closes without a frame
        self.assertEqual(len(menu), 3, run_['frames'])
        self.assertEqual(len(set(menu)), 3)            # the selection moved
        # every other frame is the closed run's, in order
        self.assertEqual(rest, [h for h, _ in self.closed['frames']])
        self.assertNotIn('settings saved', run_['out'])
        self.assertNotIn('not saved', run_['out'])     # nothing changed
        self.assertTrue(run_['settings_unchanged'])

    def test_change_applies_live_and_is_not_saved_in_a_harness_run(self):
        # FULLSCREEN, SCALE, FILTER, SCANLINES: two steps of 10%
        run_ = self.run_port('change', f'{OPEN_TICK} key Ctrl+O Down Down Down Right Right '
                                       f'Escape\n{QUIT_TICK} quit\n')
        frames = run_['frames']
        closed = [h for h, _ in self.closed['frames']]
        first_menu = next(i for i, (_, tag) in enumerate(frames) if tag == 'menu')
        after = [h for h, tag in frames[first_menu:] if tag != 'menu']
        self.assertEqual([h for h, _ in frames[:first_menu]], closed[:first_menu])
        # the game resumed where it was, now with scanlines over it
        self.assertEqual(len(after), len(closed) - first_menu)
        self.assertNotEqual(after[-1], closed[-1])
        self.assertIn('[PRESENT] settings not saved: automated run', run_['out'])
        self.assertTrue(run_['settings_unchanged'])
        self.assertEqual(run_['cfg'], ['settings.ini'])
        # the "settings not saved" note is timed in presented frames in an
        # automated run (1600 ms at 60/s: 96 frames),
        # not in wall time, so
        # the presenter hashes do not depend on the host's speed
        self.assertEqual(sum(1 for _, tag in frames if tag == 'toast'), NOTE_FRAMES)

    def test_texture_pack_choices_are_sorted_folders(self):
        # The chooser lists the folders under textures/packs in name order.
        # readdir's own order is the file system's (hash order on ext4, name
        # order on NTFS), and Windows puts desktop.ini / Thumbs.db files in
        # folders: neither is a pack.
        run_ = self.run_port('packs', f'{OPEN_TICK} key Ctrl+O Escape\n{OPEN_TICK + 20} quit\n',
                             packs=('zeta/', 'Alpha/', 'mid/', 'desktop.ini', 'Thumbs.db'))
        lines = [l for l in run_['out'].splitlines() if 'texture pack choices:' in l]
        self.assertTrue(lines, run_['out'][-2000:])
        self.assertTrue(lines[-1].endswith('choices: [None] [Alpha] [mid] [zeta]'), lines)
        # control: the files really are in the folder the game listed
        self.assertIn('desktop.ini', os.listdir(run_['cfg_dir'] / 'textures/packs'))


if __name__ == '__main__':
    unittest.main()
