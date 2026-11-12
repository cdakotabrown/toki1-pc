#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests of the controls legend (pc/hal/controls_view.c, controls_text.c).

The binding-to-label text is unit-tested in pc/tests/test_controls_legend.c
(`make -f pc/Makefile test`): defaults, controller_layout = confirm_south,
remaps, unbinding, unknown names.

InGame (the built port, date_save.txt up to Shiori's phone call):
  * the legend opened with its hotkey (Ctrl+H) on the first day, scrolled
    and closed with the hotkey, and from the settings menu (GUIDE >
    Controls) on Sunday 9 Apr, closed with Esc, changes nothing the game
    sees: per-frame VRAM / scanout / RAM hashes and the game's audio equal
    the run where it stays closed, in the enhanced and the parity profile;
  * the legend shown is the input HAL's live keymap (the defaults in an
    automated run) for all 14 PS1 buttons;
  * the one-time hint: shown once and recorded in its marker file; with
    the marker already written it is not shown again;
  * TOKI_CONTROLS=0 (negative control): no page, no hint, the hotkey does
    nothing.
Window shots are kept with TEST_CONTROLS_SHOTS=DIR.
"""
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from safe_process import run  # noqa: E402
import game_platform  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
BINARY = game_platform.GAME
EXE = ROOT / 'pc/assets/raw/PSX.EXE'

PS1_ROWS = ['Circle (confirm)', 'Cross (cancel)', 'Triangle', 'Square', 'D-pad up',
            'D-pad down', 'D-pad left', 'D-pad right', 'L1', 'R1', 'L2', 'R2', 'START',
            'SELECT']
DEFAULT_ROWS = {
    'Circle (confirm)': 'Enter, Space, Keypad Enter | B (right)',
    'Cross (cancel)': 'Backspace, Esc | A (bottom)',
    'Triangle': 'E | Y (top)',
    'Square': 'Q | X (left)',
    'D-pad up': 'Up arrow | D-pad up, Left stick up',
    'L2': 'Z | LT',
    'START': 'F1, S | Start',
    'SELECT': 'Tab | Back',
}


def call_script(path):
    """date_save.txt up to Shiori's phone call (the calendar)"""
    out = []
    for line in (ROOT / 'pc/tools/playthrough/date_save.txt').read_text().splitlines():
        out.append(line)
        if line.startswith('wait calendar '):
            out.append('end 60')
            break
    Path(path).write_text('\n'.join(out) + '\n')


def lines(path):
    return Path(path).read_text().splitlines() if Path(path).exists() else []


def present(path):
    return [(p[1], p[3] if len(p) > 3 else '') for p in (l.split() for l in lines(path))]


# the hotkey: open, scroll (Down, Enter = page down, Up), the hotkey again
# closes; the menu: Ctrl+O, Up Up (the last entry is GAME DATA AND ABOUT, the
# one before it Controls), Return, Down, Esc back to the menu, Esc closes
def keys(first, sunday, shots=False):
    P = 'Ctrl+P ' if shots else ''
    return (f'{first} key Ctrl+H {P}Down Down {P}Return {P}Up Ctrl+H\n'
            f'{sunday} key Ctrl+O Up Up Return {P}Down {P}Escape Escape\n')


@unittest.skipUnless(BINARY.is_file() and EXE.is_file(), 'needs the built port and pc/assets')
class InGame(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.dir = Path(cls.temp.name)
        cls.script = cls.dir / 'call.txt'
        call_script(cls.script)
        cls.closed = cls.play('closed', {})
        cls.used = cls.play('used', {}, keys(3590, 4100), hint='fresh')
        cls.parity_closed = cls.play('parity_closed', {}, profile='parity')
        # parity reaches the same screens later (test_guide.py)
        cls.parity_used = cls.play('parity_used', {}, keys(5300, 6000), profile='parity',
                                   hint='shown')
        cls.off = cls.play('off', {'TOKI_CONTROLS': '0'}, keys(3590, 4100), hint='fresh')
        if os.environ.get('TEST_CONTROLS_SHOTS'):
            cls.play('shots', {}, keys(3590, 4100, shots=True))

    @classmethod
    def play(cls, name, settings, keys=None, profile='enhanced', hint=None):
        out = cls.dir / name
        out.mkdir()
        files = dict(frames=out / 'frames.txt', audio=out / 'audio.txt',
                     present=out / 'present.txt', log=out / 'controls.txt', cfg=out / 'cfg',
                     hint=out / 'state.ini')
        files['cfg'].mkdir()
        (out / 'saves').mkdir()
        home = out / 'home'
        home.mkdir()
        if hint == 'shown':
            files['hint'].write_text('[state]\ncontrols_hint = shown\n')
        sets = dict(PC_FRAME_HASH=files['frames'], PC_FRAME_HASH_RAM='game',
                    PC_AUDIO_HASH=files['audio'], PC_PRESENT_HASH=files['present'],
                    TOKI_CONTROLS_LOG=files['log'], TOKI_CONFIG_DIR=files['cfg'],
                    TOKI_SAVE_DIR=out / 'saves', **settings)
        if hint:
            sets['TOKI_CONTROLS_HINT'] = files['hint']
        if keys:
            (out / 'keys.txt').write_text(keys)
            sets['TOKI_INPUT_SCRIPT'] = out / 'keys.txt'
            sets['PC_PRESENT_WINDOW_SHOT'] = out / f'{name}-%d.png'
        cmd = ['.venv/bin/python3' if (ROOT / '.venv/bin/python3').exists() else 'python3',
               'pc/tools/playthrough.py', str(cls.script), '--out', str(out / 'run'),
               '--bin', str(BINARY), '--profile', profile]
        for k, v in sets.items():
            cmd += ['--set', f'{k}={v}']
        # never the owner's config / data folders
        env = dict(os.environ, HOME=str(home), XDG_CONFIG_HOME=str(home / '.config'),
                   XDG_DATA_HOME=str(home / '.local/share'), SDL_VIDEODRIVER='dummy',
                   SDL_AUDIODRIVER='dummy')
        r = run(cmd, cwd=ROOT, env=env, timeout=1800, max_output=16 * 1024**2)
        log = out / 'run/date.pc.log'
        files['out'] = log.read_text(errors='replace') if log.exists() else ''
        files['result'] = r
        shot = os.environ.get('TEST_CONTROLS_SHOTS')
        if shot:
            Path(shot).mkdir(parents=True, exist_ok=True)
            for p in out.glob(f'{name}*.png'):
                shutil.copy(p, Path(shot) / p.name)
        return files

    def test_runs_pass(self):
        for name in ('closed', 'used', 'parity_closed', 'parity_used', 'off'):
            r = getattr(self, name)['result']
            self.assertTrue(r.ok, f'{name}: {r.stdout[-1500:]}')

    def test_the_legend_opened_both_ways(self):
        for run_ in (self.used, self.parity_used):
            out = run_['out']
            self.assertEqual(out.count('[CONTROLS] open'), 2, out[-3000:])
            self.assertEqual(out.count('[PRESENT] settings menu open (controls)'), 1)
            self.assertNotIn('settings menu full', out)

    def test_the_legend_is_the_live_keymap(self):
        blocks = Path(self.used['log']).read_text().split('--- controls\n')[1:]
        self.assertEqual(len(blocks), 2)
        for b in blocks:
            rows = [l.split(' | ', 1) for l in b.splitlines() if ' | ' in l]
            self.assertEqual([r[0] for r in rows], PS1_ROWS)
            got = dict(rows)
            for k, v in DEFAULT_ROWS.items():
                self.assertEqual(got[k], v, k)
            self.assertIn('note: Mouse:', b)
            self.assertIn('note: Controller (', b)

    def test_using_the_legend_changes_nothing_the_game_sees(self):
        for used, closed in ((self.used, self.closed), (self.parity_used, self.parity_closed)):
            self.assertGreater(len(lines(closed['frames'])), 1000)
            self.assertEqual(lines(used['frames']), lines(closed['frames']))
            self.assertEqual(lines(used['audio']), lines(closed['audio']))
            frames = present(used['present'])
            self.assertGreaterEqual(len([h for h, tag in frames if tag == 'menu']), 8)
            game = [(h, tag) for h, tag in frames if tag != 'menu']
            ref = present(closed['present'])
            self.assertEqual(len(game), len(ref))
            differ = [i for i, ((h, tag), (r, _)) in enumerate(zip(game, ref)) if h != r]
            self.assertTrue(all(game[i][1] == 'toast' for i in differ), differ[:20])

    def test_hint_once(self):
        self.assertEqual(self.used['out'].count('[CONTROLS] first-run hint shown'), 1)
        self.assertIn('note: Press Ctrl+H to see the controls', self.used['out'])
        # toasts are off by default, yet the hint is drawn (it is a note)
        self.assertGreater(sum(1 for _, tag in present(self.used['present']) if tag == 'toast'), 0)
        self.assertIn('controls_hint = shown', Path(self.used['hint']).read_text())
        # already recorded: not again
        self.assertNotIn('first-run hint', self.parity_used['out'])
        self.assertEqual(Path(self.parity_used['hint']).read_text(), '[state]\ncontrols_hint = shown\n')

    def test_negative_control_switched_off(self):
        out = self.off['out']
        self.assertNotIn('[CONTROLS]', out)
        self.assertNotIn('(controls)', out)
        self.assertEqual(lines(self.off['log']), [])
        self.assertFalse(Path(self.off['hint']).exists())


if __name__ == '__main__':
    unittest.main()
