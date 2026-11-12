# SPDX-License-Identifier: GPL-3.0-or-later
"""pc/tools/explorer.py: the input is a pure function of (start, seed), the
scripts parse as playthrough scripts, and the detectors read the port's
log and trace lines (no game run)."""
import os
import tempfile
import types
import unittest

import explorer as ex
import playthrough as pt


class InputTests(unittest.TestCase):
    def test_same_seed_same_input(self):
        a = ex.generate('newgame', 5, 20000)
        self.assertEqual(a, ex.generate('newgame', 5, 20000))
        self.assertNotEqual(a, ex.generate('newgame', 6, 20000))
        self.assertNotEqual(a, ex.generate('title', 5, 20000))

    def test_covers_the_budget_and_never_chords(self):
        lines = ex.generate('title', 1, 50000)
        t = 0
        for line in lines:
            delta, buttons = line.split()
            btn, _, hold = buttons.partition('*')
            t += int(delta[1:]) + (int(hold) if hold else 2)
            mask = pt.parse_buttons(btn)
            self.assertFalse(mask & 0x0008 and mask & 0x0001, line)   # START+SELECT
            self.assertLessEqual(bin(mask & 0x0F00).count('1'), 1, line)   # shoulder chords
        self.assertGreaterEqual(t, 40000)
        kinds = {l.split()[1].split('*')[0] for l in lines}
        self.assertTrue({'circle', 'cross', 'start', 'up', 'down', 'left', 'right'} <= kinds)

    def test_scripts_parse(self):
        for start in ('title', 'newgame', 'album:95/7/22'):
            with tempfile.TemporaryDirectory() as d:
                txt, inp = os.path.join(d, 'e.txt'), os.path.join(d, 'e.input')
                ex.numeric_input(ex.script_text(start, 2, 3000), txt, inp)
                with open(inp) as fh:
                    body = fh.read()
                self.assertTrue(body.startswith('hold 2\nclock vblank\n'))
                if start == 'newgame':
                    self.assertIn('wait schedule_ready', body)
                if start.startswith('album'):
                    self.assertIn('wait album_loaded', body)


def hand_walk(lines):
    """replay the generated presses on a model of the hand
    (step_directional_scroll_once: csr_speed 3 px per held VBlank, clamped
    to -150..140 / -100..100, frozen while L1/L2/R1/R2 is held) -> one
    (vblank, x, y, previous circle's vblank, moved: the hand moved since
    the previous circle) per circle press, and the presses themselves"""
    x, y, t, start, prev_circle, moved = 0, 0, 0, 0, None, False
    circles, presses = [], []
    for line in lines:
        delta, buttons = line.split()
        btn, _, hold = buttons.partition('*')
        hold = int(hold) if hold else 2
        start += int(delta[1:])
        mask = pt.parse_buttons(btn)
        presses.append((btn, hold))
        if not mask & 0x0F00:     # pt masks: shoulders are 0x0100..0x0800
            dx = (3 if 'right' in btn else 0) - (3 if 'left' in btn else 0)
            dy = (3 if 'down' in btn else 0) - (3 if 'up' in btn else 0)
            moved |= bool(dx or dy)
            x = max(-150, min(140, x + dx * hold))
            y = max(-100, min(100, y + dy * hold))
        if btn == 'circle':
            circles.append((start, x, y, prev_circle, moved))
            prev_circle, moved = start, False
    return circles, presses


def cue_seat(x, y):
    """taiiku_ouen_cue's seat for the hand at (x, y), or None"""
    if -48 <= y < 0:
        t, off = 0, 0
    elif -64 <= y < -48:
        t, off = 10, 16
    elif -80 <= y < -64:
        t, off = 20, 0
    else:
        return None
    for i in range(10):
        if -160 + off + 32 * i < x + 11 < -128 + off + 32 * i:
            return t + i
    return None


class ActionTests(unittest.TestCase):
    """the action mix reaches what the stuck screens' C needs"""

    def setUp(self):
        # four 50,000-frame runs' worth of input (generate covers 4 VBlanks
        # a frame)
        self.circles, self.presses = [], []
        for seed in (1, 2, 3, 4):
            c, p = hand_walk(ex.generate('card:x', seed, 200000))
            self.circles += c
            self.presses += p

    def test_plain_hand_moves(self):
        """a held direction that is not the park or an aim's leg: the
        branch `elif r < 0.62` stood after `elif r < 0.64` and never ran"""
        plain = [b for b, h in self.presses
                 if b in ex.DIRS + ex.DIAG and h in (1, 3, 5, 8, 12, 20, 30, 45, 70)
                 and b not in ('right', 'down')]
        self.assertGreater(len(plain), 20)

    def test_icon_14_is_aimed(self):
        """icon 14 (x -78..-54, y 26..32), the forced icon of an exam week,
        is under the hand at a circle many times per run (aims, not the
        repeats of a tap burst)"""
        hits = [c for c in self.circles if c[4] and -78 <= c[1] <= -54 and 26 <= c[2] <= 32]
        self.assertGreaterEqual(len(hits), 12)

    def test_girl_menu_columns(self):
        """girls 8 (x 0..42, y 80..96) and 11 (x 64..106) are aimed"""
        for lo in (0, 64):
            hits = [c for c in self.circles if c[4] and lo <= c[1] <= lo + 42 and 80 <= c[2] <= 96]
            self.assertGreaterEqual(len(hits), 5, lo)

    def test_cue_sweeps_reach_every_seat(self):
        """every seat 0..29 gets a circle at least 41 VBlanks after the
        previous one (the cue's D_801493FF >= 41)"""
        seats = {cue_seat(x, y) for t, x, y, p, m in self.circles
                 if p is not None and t - p >= 45}
        self.assertTrue(set(range(30)) <= seats, sorted(set(range(30)) - seats))


SEGMENT = """# checkpoint x
hold 2
trace girl_tbl:112
phase m96_07
wait title_menu now_step=0x12 now_sub_step=2 now_sub_sub_step=1 within 3000
+30 start
+40 circle                  # album 1 (card_file_read)
wait album_loaded now_step=0x34 now_sub_step=1 within 1800
check loaded_date calendar:7=0060061e06c501
shot album_loaded 30
+30 left+up*80
wait week_end now_step=0x34 within 16000
"""


class CheckpointStartTests(unittest.TestCase):
    def test_prefix_is_the_segment_load(self):
        """a card start plays the segment's Continue -> album 1 lines and
        stops at album_loaded: no checks, shots, trace or scripted week"""
        self.assertEqual(ex.checkpoint_prefix(SEGMENT), [
            'wait title_menu now_step=0x12 now_sub_step=2 now_sub_sub_step=1 within 3000',
            '+30 start', '+40 circle',
            'wait album_loaded now_step=0x34 now_sub_step=1 within 1800'])

    def test_expand_starts(self):
        index = [dict(id='m96_07', group='months'), dict(id='ev_x', group='events')]
        old = dict(ex._INDEX)
        ex._INDEX[os.path.join(ex.CHECKPOINTS, 'index.json')] = index
        try:
            self.assertEqual(ex.expand_starts('cards'), ['card:m96_07', 'card:ev_x'])
            self.assertEqual(ex.expand_starts('title,cards:events'), ['title', 'card:ev_x'])
            self.assertIn('card:m96_07', ex.start_names())
        finally:
            ex._INDEX.clear()
            ex._INDEX.update(old)

    def test_card_script_parses(self):
        with tempfile.TemporaryDirectory() as d:
            seg = os.path.join(d, 'segment.txt')
            with open(seg, 'w') as fh:
                fh.write(SEGMENT)
            index = [dict(id='m96_07', group='months', segment=seg, card='x.mcr')]
            old = dict(ex._INDEX)
            ex._INDEX[os.path.join(ex.CHECKPOINTS, 'index.json')] = index
            try:
                txt, inp = os.path.join(d, 'e.txt'), os.path.join(d, 'e.input')
                ex.numeric_input(ex.script_text('card:m96_07', 1, 3000), txt, inp)
            finally:
                ex._INDEX.clear()
                ex._INDEX.update(old)
            with open(inp) as fh:
                body = fh.read()
            self.assertIn('wait album_loaded', body)
            self.assertNotIn('week_end', body)

    def test_entry_outside_the_index(self):
        """a built checkpoint missing from a partial index.json (a
        `checkpoints.py build ID` rewrote it) is found by its directory"""
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, 'm96_07'))
            for n in ('segment.txt', 'card.mcr'):
                with open(os.path.join(d, 'm96_07', n), 'w') as fh:
                    fh.write(SEGMENT)
            old_dir, old = ex.CHECKPOINTS, dict(ex._INDEX)
            ex.CHECKPOINTS = d
            ex._INDEX[os.path.join(d, 'index.json')] = [dict(id='other', group='g', segment='s',
                                                             card='c')]
            try:
                e = ex.checkpoint_entry('m96_07')
                self.assertEqual(e['card'], os.path.join(d, 'm96_07', 'card.mcr'))
                with self.assertRaises(SystemExit):
                    ex.checkpoint_entry('nowhere')
            finally:
                ex.CHECKPOINTS = old_dir
                ex._INDEX.clear()
                ex._INDEX.update(old)


class ResumeTests(unittest.TestCase):
    def test_press_masks_place_presses_as_trace_c(self):
        """+N counts from the previous press's start (pc/hal/trace.c
        seq_step); a press covers `hold` VBlanks; prefix items are skipped"""
        text = ('hold 2\nphase explore\nwait a now_step=1\n+30 start\n'
                'wait b now_step=2\n+5 circle\n+3 left*4\n+10 cross\n')
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'e.txt')
            with open(p, 'w') as fh:
                fh.write(text)
            m = ex.press_masks(p, 100)
        circle, left, cross = (pt.parse_buttons(b) for b in ('circle', 'left', 'cross'))
        self.assertEqual(m, {105: circle, 106: circle, 108: left, 109: left, 110: left,
                             111: left, 118: cross, 119: cross})


class DetectorTests(unittest.TestCase):
    def test_scan_log(self):
        log = '\n'.join([
            '[MARK] game frame 0 vblank 0 step 10/00/00',
            '[MARK] game frame 120 vblank 130 step 34/01/02',
            '[CRASH] sig=11 pc=0x401000 sym=foo_disp+0x1c fault_addr=0x8 scene=52 sub=1',
            '[FOLD] site bar+0x10 value=ffffffff80123456 scene=52 sub=1',
            '[MARK] game frame 180 vblank 190 step 40/00/00',
            'src/main/matched/x.c:12:7: runtime error: index 7755 out of bounds for type \'u8 [68]\'',
            '[CRASH] sig=14 pc=0x402000 sym=baz+0x4 fault_addr=(nil) scene=64 sub=0',
        ])
        f = ex.scan_log(log)
        kinds = [(x['kind'], x['site'], x['frame']) for x in f]
        self.assertIn(('crash', 'foo_disp', 120), kinds)
        self.assertIn(('fold', 'bar+0x10', 120), kinds)
        self.assertIn(('ubsan', 'src/main/matched/x.c:12:7', 180), kinds)
        self.assertIn(('watchdog', 'scene 40/00/00', 180), kinds)

    def row(self, vb, fc, key='34/01/02', pad=0, crashes=0):
        s, a, b = key.split('/')
        return ('frame=%d pad=%04x tick=1 now_step=%s now_sub_step=%s now_sub_sub_step=%s '
                'now_girl=ff frame_counter=%s step_frame_cnt=00000000 crashes=%d'
                % (vb, pad, s, a, b, fc.to_bytes(4, 'little').hex(), crashes))

    def args(self):
        return types.SimpleNamespace(hang_vblanks=100, stuck_vblanks=500, max_crashes=5)

    def test_hang_and_progress(self):
        w = ex.Watch(self.args())
        for vb in range(300):
            w.row(self.row(vb, vb))
        self.assertIsNone(w.problem)
        for vb in range(300, 450):
            w.row(self.row(vb, 299))
        self.assertEqual(w.problem['kind'], 'hang')
        self.assertEqual(w.problem['vblank'], 299)

    def test_stuck_needs_input(self):
        w = ex.Watch(self.args())
        for vb in range(800):
            w.row(self.row(vb, vb))                  # no presses: a held screen
        self.assertIsNone(w.problem)
        w = ex.Watch(self.args())
        for vb in range(800):
            w.row(self.row(vb, vb, pad=0x2000 if vb % 10 < 2 else 0))
        self.assertEqual(w.problem['kind'], 'stuck')

    def test_crash_rows(self):
        w = ex.Watch(self.args())
        w.row(self.row(1, 1))
        w.row(self.row(2, 2, crashes=1))
        self.assertEqual(w.crash_rows, [(2, 2, '34/01/02')])


if __name__ == '__main__':
    unittest.main()
