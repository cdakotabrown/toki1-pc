# SPDX-License-Identifier: GPL-3.0-or-later
"""Skip intro ([enhanced] skip_intro, pc/hal/enhance.c) on the real port.

The assist presses START the way a player would, on the screens whose own
code skips on it (the boot movie, now_step 16: movie_main stops on
pad_trig & 0x860; the title animation, 18/1: title_move_show /
title_white_out / title_white_in go to the title menu), and stops at the
title menu.  It prints every tap ([INTRO] START at game frame F ...).

* the same input from a script: a run with the assist and a run with no
  assist whose PC_INPUT presses START at exactly the printed frames (held
  the same 2 frames) have the same machine at game frame 150 (PS1 RAM,
  MMIO, VRAM, SPU RAM, every device model) -- the assist is input and
  nothing else;
* it reaches the title menu (18/2) and stays there: no tap is made there,
  the menu is not answered;
* earlier: without it the title menu comes at VBlank ~2045 (enhanced), with
  it within ~80;
* negative controls: the same taps one frame later give another machine
  (the comparison sees a difference of one frame of input); without the
  assist the title menu is far off at frame 150; the player's
  settings file asking for it is ignored in an automated run (harness
  rule), and so is the switch under parity unless the environment forces
  it.

Needs the built port and pc/assets; skipped otherwise.
"""
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import savestate_roundtrip as rt  # noqa: E402
import savestate_tool as sst  # noqa: E402
from profile_bench import read_trace  # noqa: E402

ROOT = Path(rt.ROOT)
BINARY = ROOT / 'pc/build/tokimeki-pc'
ASSETS = ROOT / 'pc/assets'
T = 150                      # game frame of the machine comparison
TAP = re.compile(r'\[INTRO\] START at game frame (\d+) \(step (\d+)/(\d+)/(\d+)\), held (\d+) frames')
START_PSX = 0x0008           # PC_INPUT's PSX layout


def script(frames, hold=2):
    return f'hold {hold}\n' + ''.join(f'{f}:{START_PSX:04x}\n' for f in frames)


@unittest.skipUnless(BINARY.is_file() and (ASSETS / 'raw/PSX.EXE').is_file(),
                     'needs the built port and pc/assets')
class SkipIntro(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix='skip-intro-test-')
        cls.syms = sst.Symbols(str(BINARY))
        cls.assist = cls.run_case('assist', script([]), {'TOKI_SKIP_INTRO': '1'})
        cls.taps = [tuple(int(x) for x in m) for m in TAP.findall(cls.assist['text'])]

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    @classmethod
    def run_case(cls, tag, inp, extra, until=T, profile='enhanced'):
        d = os.path.join(cls.tmp, tag)
        os.makedirs(d, exist_ok=True)
        state = os.path.join(d, 'at.tks')
        with open(os.path.join(d, 'cmds'), 'w') as fh:
            fh.write(f'at {T} peek now_step 1\nat {T} peek now_sub_step 1\n'
                     f'at {T} save {state}\nat {until} quit\n')
        env = dict(PC_DBG_CMDS=os.path.join(d, 'cmds'), PC_TRACE=os.path.join(d, 'trace'),
                   PC_PARITY_DIR=os.path.join(d, 'parity'), PC_PARITY_FRAMES='1',
                   TOKI_SAVE_DIR=os.path.join(d, 'saves'))
        if profile:
            env['TOKI_PROFILE'] = profile
        if inp is not None:
            with open(os.path.join(d, 'input'), 'w') as fh:
                fh.write(inp)
            env['PC_INPUT'] = os.path.join(d, 'input')
        env.update(extra)
        rc, text = rt.run_port(str(BINARY), os.path.join(d, 'work'), os.path.join(d, 'log'), env, 900)
        assert rc == 0, text[-3000:]
        steps = [int(v) for v in re.findall(r'peek now_(?:sub_)?step 1\n\[DBG\] ok 0x\w+ 1 \w+ u=(\d+)', text)]
        rows = read_trace(os.path.join(d, 'trace'))
        title = next((r for r in rows if r['step'] == 18 and r['sub'] == 2), None)
        return dict(text=text, state=state, steps=steps, rows=rows, title=title)

    def machine_diff(self, a, b):
        rep = sst.diff_states(sst.read_state(a), sst.read_state(b), self.syms)
        return {r: sum(h - l for l, h in rs) for r, rs in rep.items() if rs and r != 'stak'}

    def test_1_taps_on_the_movie_and_the_title_animation_only(self):
        self.assertEqual([t[1:4] for t in self.taps], [(16, 0, 0), (18, 1, 0)], self.taps)
        self.assertTrue(all(t[4] == 2 for t in self.taps))
        self.assertIn('[INTRO] done at game frame', self.assist['text'])
        # at the title menu and still there: nothing answered it
        self.assertEqual(self.assist['steps'], [18, 2])
        after = [r for r in self.assist['rows'] if r['game'] >= self.taps[-1][0] + 2]
        self.assertTrue(after and all(r['step'] == 18 and r['sub'] == 2 for r in after), after[:3])

    def test_2_a_script_with_the_same_presses_gives_the_same_machine(self):
        frames = [t[0] for t in self.taps]
        human = self.run_case('script', script(frames), {})
        self.assertNotIn('[INTRO] START', human['text'])
        self.assertEqual(human['steps'], [18, 2])
        self.assertEqual(self.machine_diff(self.assist['state'], human['state']), {})
        self.assertEqual(human['title']['vblank'], self.assist['title']['vblank'])
        # negative control: one frame later is another machine
        late = self.run_case('late', script([f + 1 for f in frames]), {})
        self.assertNotEqual(self.machine_diff(self.assist['state'], late['state']), {})

    def test_3_reaches_the_title_menu_much_earlier(self):
        plain = self.run_case('plain', script([]), {}, until=1600)
        # the movie is one main-loop iteration: at the first boundary after
        # frame 150 it has just ended (step 0), the title menu is far off
        self.assertNotEqual(plain['steps'], [18, 2], 'without the assist')
        self.assertIsNotNone(plain['title'])
        self.assertLess(self.assist['title']['vblank'] * 10, plain['title']['vblank'],
                        (self.assist['title'], plain['title']))
        print(f"\n[skip_intro] title menu at VBlank {self.assist['title']['vblank']} "
              f"with the assist, {plain['title']['vblank']} without (enhanced)")

    def test_4_settings_and_parity_do_not_turn_it_on_in_automated_runs(self):
        harness = self.run_case('harness', script([]), {'TOKI_SET': 'enhanced.skip_intro=true'})
        self.assertNotIn('[INTRO]', harness['text'])
        self.assertNotEqual(harness['steps'], [18, 2])
        parity = self.run_case('parity', script([]), {}, profile=None)
        self.assertNotIn('[INTRO]', parity['text'])


if __name__ == '__main__':
    unittest.main()
