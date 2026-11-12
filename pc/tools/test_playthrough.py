# SPDX-License-Identifier: GPL-3.0-or-later
"""playthrough.py: script parsing, the numeric PC_INPUT it writes, card reads,
milestone failure reporting (against a fake port binary)."""
import os
from pathlib import Path
import stat
import tempfile
import unittest

import playthrough as pt

ROOT = Path(__file__).resolve().parents[2]

SCRIPT = '''hold 3
trace cursor_cw_x:2
phase one
wait menu now_step=0x12 now_sub_step=2 within 100   # comment
+30 start
+5 left*40
check named p_sei:8=82a082a200000000
end 10
card one SYS
phase two keep-card
wait again now_step!=0
same player menu again
'''


class PlaythroughTests(unittest.TestCase):
    def parsed(self, text=SCRIPT):
        with tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False) as fh:
            fh.write(text)
        try:
            return pt.parse(fh.name)
        finally:
            os.unlink(fh.name)

    def test_parse_and_numeric_script(self):
        s = self.parsed()
        self.assertEqual(s['hold'], 3)
        self.assertEqual([p['name'] for p in s['phases']], ['one', 'two'])
        self.assertTrue(s['phases'][1]['keep'])
        self.assertEqual(s['cards'], [('one', ['SYS'])])
        self.assertEqual(s['sames'], [('player', 0x6C, 'menu', 'again')])
        text = pt.numeric_phase(s['phases'][0], s['hold'], shots=False)
        self.assertIn('wait menu now_step:1=0x12 now_sub_step:1=0x2 within 100', text)
        self.assertIn('+30:0008', text)
        self.assertIn('+5:0080*40', text)
        self.assertIn('wait named p_sei:8=82a082a200000000 within 2', text)
        self.assertIn('end 10', text)
        self.assertIn('wait again now_step:1!=0x0', pt.numeric_phase(s['phases'][1], 3, False))
        self.assertEqual(pt.trace_syms(s), {'cursor_cw_x': 2, 'p_sei': 8})

    def test_point_verb(self):
        # the presses the hand-written scripts spell out for (-81,+59)
        s = self.parsed('phase p\nwait w now_step=1 within 9\npoint -81 59\npoint -150 -10\n')
        items = [it for it in s['phases'][0]['items'] if it['kind'] == 'press']
        lu = pt.BUTTONS['left'] | pt.BUTTONS['up']
        self.assertEqual([(i['delta'], i['mask'], i['hold']) for i in items],
                         [(30, lu, 80), (82, pt.BUTTONS['right'], 23), (25, pt.BUTTONS['down'], 53),
                          (30, lu, 80), (82, pt.BUTTONS['down'], 30)])
        for bad in ('point -80 59', 'point -81 101', 'point -160 0'):
            with self.assertRaises(pt.ScriptError):
                self.parsed('phase p\nwait w now_step=1 within 9\n' + bad + '\n')

    def test_masked_condition(self):
        c = pt.parse_cond('basyo_pack:2&0xF000=0x8000')
        self.assertEqual(c['mask'], 0xF000)
        self.assertEqual(pt.cond_text(c), 'basyo_pack:2&0xf000=0x8000')
        self.assertTrue(pt.holds({'basyo_pack': 0x8100}, [c]))
        self.assertFalse(pt.holds({'basyo_pack': 0x9000}, [c]))
        self.assertTrue(pt.holds({'basyo_pack': 0x8000}, [pt.parse_cond('basyo_pack:2=0x8000')]))
        self.assertFalse(pt.holds({'basyo_pack': 0x8100}, [pt.parse_cond('basyo_pack:2=0x8000')]))
        with self.assertRaises(pt.ScriptError):
            pt.parse_cond('player:8&0xFF=00')

    def test_mash_wait(self):
        s = self.parsed('hold 2\nphase a\nwait w now_step=0x34 within 900 mash cross/40\n')
        it = s['phases'][0]['items'][0]
        self.assertEqual(it['mash'], ([(0x4000, 0)], 40))
        self.assertEqual(it['within'], 900)
        self.assertIn('wait w now_step:1=0x34 within 900 mash 4000/40',
                      pt.numeric_phase(s['phases'][0], 2, False))
        # a cycle: tap, move the hand 23 frames, tap, move it back
        s = self.parsed('hold 2\nphase a\nwait w now_step=1 mash circle,down*23,circle,up*23/45\n')
        it = s['phases'][0]['items'][0]
        self.assertIn('mash 2000,0040*23,2000,0010*23/45', pt.numeric_phase(s['phases'][0], 2, False))
        got = pt.mash_presses(it['mash'], 100, 300, 2)
        self.assertEqual(got[0], (145, 0x2000))
        self.assertEqual([f for f, m in got if m == 0x0040], list(range(190, 212)))
        self.assertEqual(got[-1][0], 280 + 21)      # the 4th press, held 23 (22 inputs)
        # a press held longer than K delays the next one to H + 5
        s = self.parsed('hold 2\nphase a\nwait w now_step=1 mash left+up*95,circle/45\n')
        got = pt.mash_presses(s['phases'][0]['items'][0]['mash'], 0, 400, 2)
        self.assertEqual([f for f, m in got if m == 0x2000], [145, 290])
        self.assertEqual(got[0][0], 45)
        for bad in ('phase a\nwait w now_step=1 mash cross/2\n',   # K not above hold
                    'phase a\nwait w now_step=1 mash ' + ','.join(['cross'] * 17) + '/45\n',
                    'phase a\ncheck w now_step=1 mash cross/9\n'):  # a check cannot press
            with self.subTest(bad=bad), self.assertRaises(pt.ScriptError):
                self.parsed(bad)

    def test_retail_mash_placement(self):
        """the reference gets a press every K host frames from the wait's
        start; the presses at or after the hit are dropped"""
        s = self.parsed('hold 2\nphase a\nwait w now_step=0x34 mash cross/40\n+10 start\nend 5\n')
        calls = []

        def fake_regtest(frames, trace, inputs, hold, extra=None, timeout=None, limits=None):
            calls.append(list(inputs))
            return [dict(frame=f, frame_counter=f, now_step=0x34 if f >= 130 else 0)
                    for f in range(frames)]
        orig_regtest, orig_run = pt.tc.regtest, pt.run
        pt.tc.regtest = fake_regtest
        pt.run = lambda *a, **k: type('R', (), {'require_ok': lambda self: None})()
        try:
            with tempfile.TemporaryDirectory() as d:
                m, failures = pt.run_retail(s, d, timeout=30)
        finally:
            pt.tc.regtest, pt.run = orig_regtest, orig_run
        self.assertEqual(failures, [])
        self.assertEqual(m['w']['vblank'], 130)
        # mash at 40, 80, 120 (< 130), then START 10 frames after the hit
        self.assertEqual(calls[-1], [(40, 0x4000), (80, 0x4000), (120, 0x4000), (140, 0x0008)])

    BRANCH = '''hold 2
phase a
wait start now_step=0x46
+5 circle
branch answer within 900 mash cross/40
case yes reserved_date!=0
  +10 cross
  wait day_yes now_step=0x50 rand
case no now_step=0x34
  +10 start
  branch again within 600
  case yes2 reserved_date!=0
    +3 circle
  case no2 now_step=0x33
  end-branch
end-branch
wait join now_step=0x50 within 600
end 5
'''

    def test_branch_parse_and_program(self):
        s = self.parsed(self.BRANCH)
        items = s['phases'][0]['items']
        self.assertEqual([it['kind'] for it in items], ['wait', 'press', 'branch', 'wait', 'end'])
        b = items[2]
        self.assertEqual([c['label'] for c in b['cases']], ['yes', 'no'])
        self.assertTrue(b['cases'][0]['items'][1]['rand'])
        self.assertEqual(pt.rand_labels(s), {'yes', 'no', 'yes2', 'no2', 'day_yes'})
        text = pt.numeric_phase(s['phases'][0], 2, False).splitlines()
        self.assertEqual(text, [
            'hold 2',
            'clock vblank',                                       # header
            'wait start now_step:1=0x46',                         # 0
            '+5:2000',                                            # 1
            'branch answer 2 within 900 mash 4000/40',            # 2
            'case yes 5 reserved_date:4!=0x0',                    # 3
            'case no 8 now_step:1=0x34',                          # 4
            '+10:4000',                                           # 5
            'wait day_yes now_step:1=0x50',                       # 6
            'goto 16',                                            # 7
            '+10:0008',                                           # 8
            'branch again 2 within 600',                          # 9
            'case yes2 12 reserved_date:4!=0x0',                  # 10
            'case no2 14 now_step:1=0x33',                        # 11
            '+3:2000',                                            # 12
            'goto 15',                                            # 13
            'goto 15',                                            # 14
            'goto 16',                                            # 15
            'wait join now_step:1=0x50 within 600',               # 16
            'end 5'])
        saved = pt.INPUT_CLOCK
        try:
            pt.INPUT_CLOCK = 'game'
            self.assertEqual(pt.numeric_phase(s['phases'][0], 2, False).splitlines()[1],
                             'clock game')
        finally:
            pt.INPUT_CLOCK = saved

    def test_branch_errors(self):
        for bad in ('phase a\nbranch b\ncase x now_step=1\nend-branch\n',      # one case
                    'phase a\nbranch b\n+1 start\n',                           # item before a case
                    'phase a\nbranch b\ncase x now_step=1\ncase y now_step=2\n',  # unclosed
                    'phase a\ncase x now_step=1\n',                             # case outside
                    'phase a\nbranch b now_step=1\n',                           # conds on branch
                    'phase a\nwait x now_step=1\nbranch c\ncase x now_step=2\n'
                    'case y now_step=3\nend-branch\n'):                         # duplicate label
            with self.subTest(bad=bad), self.assertRaises(pt.ScriptError):
                self.parsed(bad)

    def test_retail_branch_placement(self):
        """the reference takes the first case that holds; only its items'
        presses are placed, and the script joins after the branch"""
        s = self.parsed('''hold 2
phase a
wait w now_step=0x10
branch b
case one now_step=0x20
  +5 start
case two now_step=0x30
  +7 circle
end-branch
wait j now_step=0x40
+3 cross
end 5
''')
        calls = []

        def fake_regtest(frames, trace, inputs, hold, extra=None, timeout=None, limits=None):
            calls.append(list(inputs))
            def step(f):
                return 0x10 if f < 50 else 0x30 if f < 100 else 0x40
            return [dict(frame=f, frame_counter=f, now_step=step(f)) for f in range(frames)]
        orig_regtest, orig_run = pt.tc.regtest, pt.run
        pt.tc.regtest = fake_regtest
        pt.run = lambda *a, **k: type('R', (), {'require_ok': lambda self: None})()
        pt.BRANCHES.clear()
        try:
            with tempfile.TemporaryDirectory() as d:
                m, failures = pt.run_retail(s, d, timeout=30)
        finally:
            pt.tc.regtest, pt.run = orig_regtest, orig_run
        self.assertEqual(failures, [])
        self.assertEqual(m['b']['vblank'], 50)
        self.assertEqual(m['two']['vblank'], 50)
        self.assertNotIn('one', m)
        self.assertEqual(pt.BRANCHES['retail'], {'b': 'two'})
        self.assertEqual(m['j']['vblank'], 100)
        self.assertEqual(calls[-1], [(57, 0x2000), (103, 0x4000)])

    def test_script_errors(self):
        for bad in ('wait x now_step=1\n',                      # before a phase
                    'phase a\n+3 bogus\n',                       # unknown button
                    'phase a\nwait x\n',                         # no condition
                    'phase a\nwait x p_sei:8=82a0\n',            # wrong byte count
                    'phase a\nwait x now_step=1\nwait x now_step=2\n'):  # duplicate
            with self.subTest(bad=bad), self.assertRaises(pt.ScriptError):
                self.parsed(bad)

    def test_read_card(self):
        card = bytearray(128 * 1024)
        card[0:2] = b'MC'
        for i in range(1, 16):
            card[i * 128] = 0xA0
        card[2 * 128] = 0x51
        card[2 * 128 + 0x0A:2 * 128 + 0x0A + 6] = b'BISLPS'
        card[2 * 8192:2 * 8192 + 4] = b'DATA'
        with tempfile.NamedTemporaryFile(delete=False) as fh:
            fh.write(card)
        try:
            files = pt.read_card(fh.name)
        finally:
            os.unlink(fh.name)
        self.assertEqual(list(files), ['BISLPS'])
        self.assertEqual(files['BISLPS'][:4], b'DATA')

    def test_unreached_milestone_fails_loudly(self):
        # a fake port that reports the first milestone only, then exits 3
        with tempfile.TemporaryDirectory() as d:
            fake = Path(d) / 'port'
            fake.write_text('#!/bin/sh\n'
                            'echo "[TRACE] PC_INPUT $PC_INPUT: 0 presses, 3 anchored items, hold 2"\n'
                            'echo "[TRACE] PC_INPUT wait menu reached: game frame 10 vblank 12"\n'
                            'echo "[TRACE] PC_INPUT wait named NOT reached within 2 vblanks"\n'
                            'exit 3\n')
            fake.chmod(fake.stat().st_mode | stat.S_IEXEC)
            s = self.parsed()
            m, failures = pt.run_pc(s, d, str(fake), shots=False, timeout=30)
            self.assertEqual(m['menu']['game'], 10)
            self.assertTrue(any('named NOT reached' in f for f in failures), failures)
            self.assertTrue(any('process ended 3' in f for f in failures), failures)
            # the failing phase stops the run: phase two never starts
            self.assertNotIn('again', m)

    def test_audio_mix_env(self):
        """--audio-mix game (default) gives the port the game-thread mix
        (PC_PARITY_DIR, no anchor); host leaves it unset"""
        for mix, want in (('game', True), ('host', False)):
            with tempfile.TemporaryDirectory() as d:
                fake = Path(d) / 'port'
                seen = Path(d) / 'env.txt'
                fake.write_text('#!/bin/sh\n'
                                f'echo "parity=${{PC_PARITY_DIR}}" > {seen}\n'
                                'echo "[TRACE] PC_INPUT $PC_INPUT: 0 presses, 3 anchored items, hold 2"\n'
                                'exit 3\n')
                fake.chmod(fake.stat().st_mode | stat.S_IEXEC)
                old = pt.AUDIO_MIX
                pt.AUDIO_MIX = mix
                try:
                    pt.run_pc(self.parsed(), d, str(fake), shots=False, timeout=30)
                finally:
                    pt.AUDIO_MIX = old
                text = seen.read_text().strip()
                self.assertEqual(text != 'parity=', want, text)


class ShotTests(unittest.TestCase):
    def test_vram_ppm_and_diff(self):
        """the reference shot is the (0, DISPLAY_Y) 320x240 BGR555 rectangle;
        the diff counts pixels that differ at 5-bit precision"""
        vram = bytearray(1024 * 512 * 2)
        o = (pt.DISPLAY_Y * 1024 + 5) * 2
        vram[o:o + 2] = (31 | (16 << 5) | (1 << 10)).to_bytes(2, 'little')
        with tempfile.TemporaryDirectory() as d:
            a, b = os.path.join(d, 'a.ppm'), os.path.join(d, 'b.ppm')
            pt.vram_ppm(bytes(vram), pt.DISPLAY_Y, a)
            w, h, px = pt.read_ppm(a)
            self.assertEqual((w, h), (320, 240))
            self.assertEqual(px[15:18], bytes((248, 128, 8)))
            # the port's shot carries 8-bit channels: low 3 bits are ignored
            pc = bytearray(px)
            pc[15:18] = bytes((255, 135, 15))
            with open(b, 'wb') as fh:
                fh.write(b'P6\n320 240\n255\n' + bytes(pc))
            self.assertEqual(pt.shot_diff(b, a), dict(differing=0.0, mad=0.0))
            pc[0:3] = bytes((8, 0, 0))
            with open(b, 'wb') as fh:
                fh.write(b'P6\n320 240\n255\n' + bytes(pc))
            self.assertEqual(pt.shot_diff(b, a)['differing'], round(1 / (320 * 240), 6))
            self.assertGreater(pt.shot_diff(b, a)['differing'], 0)


if __name__ == '__main__':
    unittest.main()
