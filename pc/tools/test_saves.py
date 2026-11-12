# SPDX-License-Identifier: GPL-3.0-or-later
"""The save overhaul on the real port: weekly autosave, the ending save, the
resume state on quit, the save browser (pc/hal/save_auto.c, save_browser.c,
quicksave.c; player docs PLAYER-GUIDE.md "Saves").

* Autosave is read-only for the game: spring_events.txt's input (11 weekly
  planners, 4 Apr .. 11 Jun 1995) run twice with the saves folder set, once
  plain and once with the autosave forced on (PC_AUTOSAVE_FORCE=1: automated
  runs never autosave otherwise).  Every presented frame (PC_FRAME_HASH: VRAM
  and the displayed picture), the trace and the mixed audio are identical;
  the forced run wrote one autosave per planner date, at the planner's
  input wait (scene 52/1), into auto1..auto3 in rotation; the plain run
  wrote none.
* The ending save: the date poked to 1998-03-01 at the first planner, the
  game's own schedule_day_main / schedule_exit go to step 99 (ENDING) and
  the `ending` entry is written there.
* Resume: `winquit` (the window's close button) during a game defers the
  quit to the next boundary, writes `resume`, and the process ends through
  its normal shutdown; a new process with TOKI_RESUME=1 (the launcher's
  --resume) loads it at its first boundary and, K frames later, has the
  same machine and parity artifacts as a run that never stopped.  Quitting
  outside a game writes nothing and deletes a stale resume; a resume state
  of another build is not offered, it is renamed resume-old.*.
* The save browser through real key events (TOKI_INPUT_SCRIPT `key`): Ctrl+S
  opens it, NEW SAVE writes save0001, LOAD loads it (the undo slot is
  written), DELETE asks and deletes.  (The launch prompt holds the game
  before its first pad poll, where TOKI_INPUT_SCRIPT cannot reach it; its
  page is unit-tested in pc/tests/test_save_browser.c.)

Needs the built port and pc/assets; skipped otherwise.  About 6 minutes.
"""
import configparser
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import playthrough as pt  # noqa: E402
import savestate_roundtrip as rt  # noqa: E402
import savestate_tool as sst  # noqa: E402

ROOT = Path(rt.ROOT)
BINARY = ROOT / 'pc/build/tokimeki-pc'
WIN_BINARY = ROOT / 'pc/build-win/tokimeki-pc.exe'   # the Windows build, under Wine
WINE = shutil.which('wine')
ASSETS = ROOT / 'pc/assets'
NEW_GAME = ROOT / 'pc/tools/playthrough/new_game.txt'
SPRING = ROOT / 'pc/tools/playthrough/spring_events.txt'
K = 180
AUTOSAVE = re.compile(r'\[AUTOSAVE\] (?:week|month) of (\S+) \(pass_days (\d+)\) -> (auto\d) at frame (\d+), '
                      r'scene (\d+)/(\d+)/(\d+), after (\d+) busy boundaries')
# the first planner of each month (the default, [saves] autosave_every = month)
MONTH_DATES = ['1995-04-04', '1995-05-07', '1995-06-04']
PLANNER_DATES = ['1995-04-04', '1995-04-09', '1995-04-16', '1995-04-23', '1995-04-30', '1995-05-07',
                 '1995-05-14', '1995-05-21', '1995-05-28', '1995-06-04', '1995-06-11']


def ini(path):
    c = configparser.ConfigParser()
    c.read(path)
    return c


def available():
    return BINARY.is_file() and (ASSETS / 'raw/PSX.EXE').is_file()


@unittest.skipUnless(available(), 'needs the built port and pc/assets')
class Autosave(unittest.TestCase):
    """spring_events with and without the forced autosave"""
    binary = BINARY

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix='saves-auto-')
        script = pt.parse(str(SPRING))
        phase = script['phases'][0]
        num = os.path.join(cls.tmp, 'spring.input')
        with open(num, 'w') as fh:
            fh.write(pt.numeric_phase(phase, script['hold'], False))
        syms = pt.trace_syms(script)
        cls.runs = {}
        for name, force in (('plain', False), ('auto', True), ('month', True)):
            d = os.path.join(cls.tmp, name)
            os.makedirs(d)
            env = dict(PC_INPUT=num, PC_TRACE=os.path.join(d, 'trace'), PC_TRACE_PHASE='pre_irq',
                       PC_TRACE_FRAMES='40000', PC_PARITY_DIR=os.path.join(d, 'parity'),
                       PC_PARITY_FRAMES='1', PC_FRAME_HASH=os.path.join(d, 'frames'),
                       TOKI_SAVE_DIR=os.path.join(d, 'saves'))
            if syms:
                env['PC_TRACE_SYMS'] = ','.join(f'{n}:{s}' for n, s in syms.items())
            if force:
                env['PC_AUTOSAVE_FORCE'] = '1'
                # 'auto' checks every week's planner; 'month' the default period
                env['TOKI_AUTOSAVE_EVERY'] = 'week' if name == 'auto' else 'month'
            rc, text = rt.run_port(str(cls.binary), os.path.join(d, 'work'), os.path.join(d, 'log'), env, 1200)
            cls.runs[name] = dict(dir=d, rc=rc, text=text)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_autosave_is_read_only(self):
        a, b = self.runs['plain'], self.runs['auto']
        reached = re.findall(r'PC_INPUT wait (\S+) reached', b['text'])
        self.assertIn('rensyu', reached, 'the run reached the end of the script')
        for name in ('frames', 'trace'):
            fa = Path(a['dir'], name).read_bytes()
            fb = Path(b['dir'], name).read_bytes()
            self.assertGreater(len(fa), 1000)
            self.assertEqual(fa, fb, f'{name} differs with the autosave on')
        self.assertEqual(rt.compare_dirs(os.path.join(a['dir'], 'parity'), os.path.join(b['dir'], 'parity'),
                                         rt.is_windows(self.binary)), [])

    def test_monthly_autosave_at_the_first_planner_of_each_month(self):
        """the default period: one autosave per calendar month, at the first
        planner whose date is in it; the weekly run (negative control) saves
        at all eleven planners"""
        text = self.runs['month']['text']
        saves = AUTOSAVE.findall(text)
        self.assertEqual([s[0] for s in saves], MONTH_DATES, text[-2000:])
        self.assertEqual([s[2] for s in saves], ['auto1', 'auto2', 'auto3'])
        self.assertIn('[AUTOSAVE] month of', text)
        self.assertEqual(len(AUTOSAVE.findall(self.runs['auto']['text'])), len(PLANNER_DATES))

    def test_one_autosave_per_planner_in_rotation(self):
        text = self.runs['auto']['text']
        saves = AUTOSAVE.findall(text)
        self.assertEqual([s[0] for s in saves], PLANNER_DATES, text[-2000:])
        self.assertEqual([s[2] for s in saves], ['auto1', 'auto2', 'auto3'] * 3 + ['auto1', 'auto2'])
        for date, pass_days, ident, frame, step, sub, subsub, waited in saves:
            self.assertEqual((step, sub), ('52', '1'), 'saved at the planner waiting for input')
        self.assertNotIn('planner left before', text)
        self.assertEqual(text.count('[QUICKSAVE] Saved autosave'), len(PLANNER_DATES))
        qs = Path(self.runs['auto']['dir'], 'saves/quicksave')
        self.assertEqual(sorted(p.name for p in qs.iterdir()),
                         sorted(f'auto{i}.{e}' for i in (1, 2, 3) for e in ('ini', 'png', 'tks')))
        got = {}
        for i in (1, 2, 3):
            g = ini(qs / f'auto{i}.ini')
            self.assertEqual(g['slot']['kind'], 'auto')
            self.assertEqual(g['slot']['id'], f'auto{i}')
            self.assertEqual(g['slot']['scene'].split('/')[:2], ['52', '1'])
            got[f'auto{i}'] = f"{g['game']['year']}-{int(g['game']['month']):02d}-{int(g['game']['day']):02d}"
        self.assertEqual(got, {'auto1': '1995-06-04', 'auto2': '1995-06-11', 'auto3': '1995-05-28'})

    def test_never_in_an_automated_run(self):
        text = self.runs['plain']['text']
        self.assertNotIn('[AUTOSAVE]', text)
        self.assertFalse(Path(self.runs['plain']['dir'], 'saves/quicksave').exists())


@unittest.skipUnless(available(), 'needs the built port and pc/assets')
class ResumeAndEnding(unittest.TestCase):
    binary = BINARY
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix='saves-resume-')
        cls.syms = sst.Symbols(str(cls.binary))
        cls.reached, cls.abs_input, _ = rt.resolve(str(NEW_GAME), cls.tmp, str(cls.binary), 900)
        with open(os.path.join(cls.tmp, 'resolve.log'), errors='replace') as fh:
            cls.vblank = {m.group(1): int(m.group(2)) for m in re.finditer(
                r'PC_INPUT wait (\S+) reached: game frame \d+ vblank (\d+)', fh.read())}

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def run_case(self, tag, cmds, extra=None, saves=None, keys=None, parity=True):
        d = os.path.join(self.tmp, tag)
        os.makedirs(d, exist_ok=True)
        inp = os.path.join(d, 'abs.input')
        with open(inp, 'w') as fh:
            fh.write(self.abs_input)
        env = dict(PC_INPUT=inp, TOKI_SAVE_DIR=saves or os.path.join(self.tmp, 'saves'))
        if cmds is not None:
            with open(os.path.join(d, 'cmds'), 'w') as fh:
                fh.write(cmds)
            env['PC_DBG_CMDS'] = os.path.join(d, 'cmds')
        if parity:
            env.update(PC_PARITY_DIR=os.path.join(d, 'parity'), PC_PARITY_FRAMES=f'1,30,90,{K - 10}',
                       PC_PARITY_RAM='1')
        if keys is not None:
            with open(os.path.join(d, 'keys'), 'w') as fh:
                fh.write(keys)
            env.update(TOKI_INPUT_SCRIPT=os.path.join(d, 'keys'),
                       PC_PRESENT_HASH=os.path.join(d, 'present'))
        env.update(extra or {})
        rc, text = rt.run_port(str(self.binary), os.path.join(d, 'work'), os.path.join(d, 'log'), env, 900)
        return d, rc, text

    def test_resume_round_trip(self):
        n = self.reached['lesson'] + 50
        saves = os.path.join(self.tmp, 'saves-rt')
        # the player closes the window at frame N of a game
        d, rc, text = self.run_case('quit', f'at {n} winquit\n', saves=saves,
                                    extra={'PC_RESUME_FORCE': '1'})
        self.assertEqual(rc, 0, text[-3000:])
        self.assertIn(f'[RESUME] resume state written at frame {n}', text)
        self.assertIn('[HAL] Shutdown', text)                 # the normal exit path
        self.assertNotRegex(text, r'\[DBG\] cmds line \d+ at frame \d+: (?!winquit)')
        qs = Path(saves) / 'quicksave'
        g = ini(qs / 'resume.ini')
        self.assertEqual((g['slot']['kind'], g['slot']['id']), ('resume', 'resume'))
        self.assertEqual(int(g['slot']['frame_counter']), n)
        self.assertEqual(g['game']['in_progress'], '1')
        self.assertEqual(int(sst.read_state(str(qs / 'resume.tks'))['info']['frame_counter']), n)
        # 1. the quit wrote the machine at N: an uninterrupted run's state
        #    at N is the same (outside the game stack's leftovers, which are
        #    host values and differ between any two processes)
        ref_n = os.path.join(self.tmp, 'resume-ref_n.tks')
        d_ref, rc, text = self.run_case('ref', f'at {n} save {ref_n}\nafter 1 quit\n',
                                        saves=os.path.join(self.tmp, 'saves-ref'))
        self.assertEqual(rc, 0, text[-3000:])
        self.same(ref_n, str(qs / 'resume.tks'))
        # 2. the next launch with --resume loads it at its first boundary and
        #    runs on exactly as the toolkit's own loader (PC_LOADSTATE, the
        #    bit-exact replay path of savestate_roundtrip.py) does from the
        #    same file -- the same machine and parity artifacts at N+K.  (A
        #    run that never stopped is not the reference past N: it has its
        #    own stack leftovers, which game code reads.)
        r_n = os.path.join(self.tmp, 'resume-r_n.tks')
        load_end = os.path.join(self.tmp, 'resume-load_end.tks')
        ls_end = os.path.join(self.tmp, 'resume-ls_end.tks')
        for tag in ('resume', 'loadstate'):
            shutil.copytree(saves, os.path.join(self.tmp, f'saves-{tag}'))
        d_load, rc, text = self.run_case('resume', f'at 0 save {r_n}\nafter 0 anchor\n'
                                         f'at {n + K} save {load_end}\nafter 1 quit\n',
                                         saves=os.path.join(self.tmp, 'saves-resume'),
                                         extra={'TOKI_RESUME': '1'})
        self.assertEqual(rc, 0, text[-3000:])
        self.assertIn('[QUICKSAVE] resume loaded', text)
        self.assertRegex(text, r'\[DBG\] cmds line 1 at frame %d: save' % n)
        self.same(ref_n, r_n)
        d_ls, rc, text = self.run_case('loadstate', f'at 0 anchor\nat {n + K} save {ls_end}\nafter 1 quit\n',
                                       saves=os.path.join(self.tmp, 'saves-loadstate'),
                                       extra={'PC_LOADSTATE': str(qs / 'resume.tks')})
        self.assertEqual(rc, 0, text[-3000:])
        self.same(ls_end, load_end)
        self.assertEqual(rt.compare_dirs(os.path.join(d_ls, 'parity'), os.path.join(d_load, 'parity'),
                                         rt.is_windows(self.binary)), [])

    def same(self, a, b):
        """the same machine (rt.machine_same: every region but the game
        stack's leftover bytes, which hold host values)"""
        sa = sst.read_state(a)
        rep = sst.diff_states(sa, sst.read_state(b), self.syms)
        state = {r: sum(h - l for l, h in rs) for r, rs in rep.items()}
        names = sst.diff_names(sa, rep, syms=self.syms)
        self.assertTrue(rt.machine_same(state), names)

    def test_paced_resume_keeps_real_time(self):
        """a state loaded into a paced (not turbo) session: the frame pacing
        starts over from the loaded clock instead of sleeping away the
        emulated time between the two moments (system.c hal_pacing_resync;
        before it, a resume at frame 0 of a new session slept ~77 s and the
        watchdog fired)"""
        n = self.reached['lesson'] + 50
        saves = os.path.join(self.tmp, 'saves-paced')
        d, rc, text = self.run_case('paced-quit', f'at {n} winquit\n', saves=saves, parity=False,
                                    extra={'PC_RESUME_FORCE': '1'})
        self.assertIn(f'[RESUME] resume state written at frame {n}', text)
        t0 = time.monotonic()
        d, rc, text = self.run_case('paced', f'at {n + 120} info\nafter 1 quit\n', saves=saves,
                                    parity=False, extra={'TOKI_RESUME': '1', 'PC_TURBO': '0'})
        wall = time.monotonic() - t0
        self.assertEqual(rc, 0, text[-3000:])
        self.assertIn('[QUICKSAVE] resume loaded', text)
        self.assertIn(f'ok frame={n + 120} ', text)
        self.assertNotIn('[CRASH]', text)
        self.assertLess(wall, 40, 'the paced run slept away the emulated time')

    def test_quit_outside_a_game_and_another_build(self):
        saves = os.path.join(self.tmp, 'saves-misc')
        qs = Path(saves) / 'quicksave'
        qs.mkdir(parents=True)
        (qs / 'resume.tks').write_bytes(b'TOKISTv1')
        (qs / 'resume.ini').write_text('[slot]\nbuild_id = 0123\nsaved = 2026-01-02 03:04:05\n'
                                       '[game]\nin_progress = 1\n')
        # another build's resume state: not offered, set aside
        d, rc, text = self.run_case('other', 'at 300 info\nafter 1 quit\n', saves=saves, parity=False,
                                    extra={'PC_RESUME_FORCE': '1'})
        self.assertEqual(rc, 0, text[-3000:])
        self.assertIn('[RESUME] the resume state was saved by another build', text)
        self.assertNotIn('[RESUME] offering', text)
        self.assertEqual(sorted(p.name for p in qs.iterdir()), ['resume-old.ini', 'resume-old.tks'])
        # a quit outside a game: immediate, no resume state.  A session that
        # never got into a game (closed at the title, or at the "continue?"
        # prompt) leaves an existing resume state alone ...
        n = self.reached['lesson'] + 50
        d, rc, text = self.run_case('title', 'at 300 qsave resume\nafter 5 winquit\n', saves=saves,
                                    parity=False, extra={'PC_RESUME_FORCE': '1'})
        self.assertEqual(rc, 0, text[-3000:])
        self.assertIn('[QUICKSAVE] resume saved', text)
        self.assertNotIn('resume deleted', text)
        self.assertIn('[HAL] Shutdown', text)
        self.assertTrue((qs / 'resume.tks').exists())
        # ... one that played and then left the game deletes it: the last
        # quit was not in a game.  (The quit is asked in the lesson, but the
        # load of a boot-time quicksave asked just before acts first, so at
        # the boundary the game is gone: no resume state is written either.)
        d, rc, text = self.run_case('left', f'at 300 qsave 4\nat {n} qload 4\nafter 0 winquit\n',
                                    saves=saves, parity=False, extra={'PC_RESUME_FORCE': '1'})
        self.assertEqual(rc, 0, text[-3000:])
        self.assertIn('[QUICKSAVE] slot 4 loaded', text)
        self.assertIn('[RESUME] no game in progress at the boundary', text)
        self.assertIn('[QUICKSAVE] resume deleted', text)
        self.assertNotIn('[RESUME] resume state', text)
        self.assertFalse((qs / 'resume.tks').exists())
        # without PC_RESUME_FORCE an automated run never defers its quit
        (qs / 'resume.tks').write_bytes(b'TOKISTv1')
        d, rc, text = self.run_case('harness', f'at {n} winquit\n', saves=saves, parity=False)
        self.assertEqual(rc, 0, text[-3000:])
        self.assertNotIn('[RESUME]', text)
        self.assertEqual((qs / 'resume.tks').read_bytes(), b'TOKISTv1')

    def test_ending_save(self):
        n = self.reached['schedule_ready'] + 20
        saves = os.path.join(self.tmp, 'saves-ending')
        # 1998-03-01 (a Sunday): schedule_day_main's first run on it starts the ending
        d, rc, text = self.run_case(
            'ending', f'at {n} poke now_year:1 98\nat {n} poke now_month:1 3\nat {n} poke now_day:1 1\n'
            f'at {n} poke now_youbi:1 6\nat {n + 3000} info\nafter 1 quit\n',
            saves=saves, parity=False, extra={'PC_AUTOSAVE_FORCE': '1'})
        self.assertEqual(rc, 0, text[-3000:])
        # (the day has already moved on to 2 March when step 99 starts)
        m = re.search(r'\[AUTOSAVE\] ending \(1998-03-0\d\) -> ending at frame (\d+), scene 99/', text)
        self.assertTrue(m, re.findall(r'\[AUTOSAVE\][^\n]*', text))
        g = ini(Path(saves) / 'quicksave/ending.ini')
        self.assertEqual((g['slot']['kind'], g['game']['year'], g['game']['month']), ('ending', '1998', '3'))
        self.assertEqual(text.count('-> ending at frame'), 1)

    def test_browser_through_keys(self):
        saves = os.path.join(self.tmp, 'saves-browser')
        v = self.vblank['lesson']
        # ticks are the pad's VBlank polls (the boot movie is one long game
        # frame, so they are counted from the lesson's VBlank); the pause
        # loop takes one queued key per shown frame.  A load rolls the pad's
        # tick back with the machine; the script's position stays.
        keys = (f'{v + 50} key Ctrl+S Return\n'                    # NEW SAVE
                f'{v + 300} key Ctrl+S Down Return Return\n'       # save0001: LOAD
                # newest first: UNDO (slot 0, the load's undo), SAVE 1; on
                # SAVE 1: LOAD, SAVE HERE, DELETE -> NO, YES; Escape closes
                f'{v + 600} key Ctrl+S Down Down Return Down Down Return Down Return Escape\n'
                f'{v + 700} quit\n')
        d, rc, text = self.run_case('browser', None, saves=saves, keys=keys, parity=False)
        self.assertEqual(rc, 0, text[-3000:])
        self.assertIn('[PRESENT] settings menu open (saves)', text)
        self.assertIn('[QUICKSAVE] save0001 saved', text)
        self.assertIn('[QUICKSAVE] save0001 loaded', text)
        self.assertIn('[QUICKSAVE] slot 0 saved', text)       # the undo of that load
        self.assertIn('[QUICKSAVE] save0001 deleted', text)
        qs = Path(saves) / 'quicksave'
        self.assertFalse((qs / 'save0001.tks').exists())
        self.assertEqual(text.count('[QUICKSAVE] save0001 saved'), 1)
        self.assertNotIn('save0002', text)
        frames = Path(d, 'present').read_text().splitlines()
        self.assertGreaterEqual(sum(line.endswith(' menu') for line in frames), 3)



def windows_available():
    return WIN_BINARY.is_file() and WINE and (ASSETS / 'raw/PSX.EXE').is_file()


@unittest.skipUnless(windows_available(), 'needs the Windows build, Wine and pc/assets')
class AutosaveWindows(Autosave):
    """the autosave tests on the Windows build under Wine: before the
    portable state format every weekly autosave failed there"""
    binary = WIN_BINARY


@unittest.skipUnless(windows_available(), 'needs the Windows build, Wine and pc/assets')
class ResumeAndEndingWindows(ResumeAndEnding):
    """resume, the ending save and the save browser on the Windows build"""
    binary = WIN_BINARY


if __name__ == '__main__':
    unittest.main()
