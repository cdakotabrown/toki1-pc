# SPDX-License-Identifier: GPL-3.0-or-later
"""The PC layer's state machines across a restore of the machine
(docs/pc-port/STATE-MACHINES.md).

A state load (quickload, the save browser, the resume, the toolkit's
`load`, PC_LOADSTATE) or a rewind replaces the machine; the PC layer's own
machines either stay the session's (dbg_state.c KEEP rules) or are told
(dbg_state_after_restore).  Before 2026-11-03 both lists had holes:

* Host resources in states.  The file writer's mutex / condition variables
  / thread (async_io.c), the texture dumper's heap tables and FILE*
  (gpu_texrep.c), the hi-res shadow's buffers and textures (gpu.c g_hires,
  hires_tex; gpu_hires.c pixels) were saved and written back by a load.  A
  state saved in one process and loaded in another (the resume at launch,
  a browser load after a restart) under the enhanced profile with internal
  resolution 2x and texture dumping hung in pthread_mutex_lock on the dead
  mutex, and the watchdog crash-looped.  Checked here: savestate_tool.py
  audit finds no host pointer in such a state (only the known value
  coincidences below), and a new process loads it and runs 60 frames.
* The observers.  Every restore path tells every observer: PC_RESTORE_LOG=1
  prints which ones heard of it ("[RESTORE] file: trace extras hires
  assist pacing autosave rewind"; a rewind tells all but the rewind
  buffer).  Before, the toolkit's load told only the rewind buffer (and
  the extras/trace via dbg_state_load), a rewind no extras.
* The consequence that shows: a paced (not turbo) session that loads a
  state of a later moment through the toolkit (PC_LOADSTATE) slept the
  whole emulated gap between the two moments, as the resume did before
  hal_pacing_resync (test_saves.py test_paced_resume_keeps_real_time).

Negative control: STATE_MACHINES_BINARY=<a build before the fix> makes
every test here fail (recorded in STATE-MACHINES.md).  Needs the built port
and pc/assets; skipped otherwise.  PC_TEST_PLATFORM=windows runs the
Windows build under Wine.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import game_platform  # noqa: E402
import savestate_roundtrip as rt  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
from pc_paths import decomp  # noqa: E402
BINARY = os.environ.get('STATE_MACHINES_BINARY') or str(game_platform.GAME)
ASSETS = ROOT / 'pc/assets'
PY = sys.executable
TMP = ROOT / 'build/tmp'          # not /tmp: states are tens of MB
TMP.mkdir(parents=True, exist_ok=True)
# words of saved data that merely look like host addresses: VRAM pixels and
# the PGXP cache's coordinate fractions (pc/hal/gpu_pgxp.c PgxpSlot: values)
VALUE_COINCIDENCES = {'g_vram', 'gpu_pgxp.c:g_table', 'gpu_pgxp.c:g_tri'}
FILE_OBSERVERS = 'trace extras hires assist pacing autosave audio music rewind'
REWIND_OBSERVERS = 'trace extras hires assist pacing autosave audio music'
ENHANCED = dict(TOKI_PROFILE='enhanced', TOKI_RES_SCALE='2', TOKI_TEXTURE_DUMP='1', AUTOPLAY='1')


def available():
    return os.path.isfile(BINARY) and ASSETS.is_dir()


class Port:
    tmp = None

    @classmethod
    def port(cls, tag, cmds, extra=None, timeout=900):
        d = os.path.join(cls.tmp, tag)
        os.makedirs(d, exist_ok=True)
        env = dict(TOKI_SAVE_DIR=os.path.join(d, 'saves'), TOKI_TEXTURE_DIR=os.path.join(d, 'tex'))
        if cmds is not None:
            with open(os.path.join(d, 'cmds'), 'w') as fh:
                fh.write(cmds)
            env['PC_DBG_CMDS'] = os.path.join(d, 'cmds')
        env.update(extra or {})
        t0 = time.monotonic()
        rc, text = rt.run_port(BINARY, os.path.join(d, 'work'), os.path.join(d, 'log'), env, timeout)
        return rc, text, time.monotonic() - t0


# ---------------------------------------------------------------- harnesses

CC = shutil.which('cc') or shutil.which('gcc')
CFLAGS = ['-std=gnu17', '-w', '-funsigned-char', '-I', 'pc/include/game', '-I', 'pc/include',
          '-I', decomp('include'), '-I', 'pc/hal', '-include', 'pc/include/game/types.h']
# save_auto.c (the autosave / ending / resume / quit machine): each mutation
# undoes one fix or one rule and must make its case fail
SA_MUTATIONS = [
    ('the busy wait counted per call, not per frame', 'pc/hal/save_auto.c',
     '    if (g_waited_frame == frame_counter) return;\n', '', 'machine'),
    ('the quit timeout keeps the older resume', 'pc/hal/save_auto.c',
     '            qs_delete_id("resume");\n            printf("[RESUME] the older resume state is deleted\\n");\n',
     '', 'timeout'),
    ('a load does not drop an armed save', 'pc/hal/save_auto.c',
     '    if (g_state == SA_WEEK_ARMED || g_state == SA_ENDING_ARMED) sa_go(SA_IDLE, "a state was loaded");\n',
     '', 'machine'),
    ('the planner arms in any state', 'pc/hal/save_auto.c',
     '    if (g_state == SA_IDLE && now_step == STEP_PLANNER', '    if (now_step == STEP_PLANNER', 'machine'),
]


def build_harness(out, sources, defines=(), mutation=None):
    """compile SOURCES (repo paths) into OUT; MUTATION (desc, path, old, new, mode)
    is applied to a copy of its file first"""
    srcs = []
    for src in sources:
        path = str(ROOT / src)
        if mutation and mutation[1] == src:
            with open(path) as fh:
                text = fh.read()
            if mutation[2] not in text:
                raise AssertionError(f'mutation {mutation[0]!r}: its text is not in {src}')
            path = os.path.join(os.path.dirname(out), os.path.basename(src))
            with open(path, 'w') as fh:
                fh.write(text.replace(mutation[2], mutation[3]))
        srcs.append(path)
    r = subprocess.run([CC] + CFLAGS + list(defines) + ['-o', out] + srcs, cwd=ROOT,
                       capture_output=True, text=True)
    if r.returncode:
        raise AssertionError(r.stderr[-3000:])
    return out


def run_harness(binary, mode, env=None):
    e = dict(os.environ)
    e.update(env or {})
    r = subprocess.run([binary, mode], capture_output=True, text=True, env=e)
    return r.returncode, r.stdout


SA_SOURCES = ['pc/tests/save_auto_harness.c', 'pc/hal/save_auto.c']
SA_DEFINES = ['-DSA_HARNESS_RAM=g_ram']


@unittest.skipUnless(CC, 'needs a C compiler')
class SaveAutoMachine(unittest.TestCase):
    """pc/hal/save_auto.c's explicit machine (one enum, one transition
    function) against stubs: the weekly autosave (once per planner date, at
    0x34/1; not after leaving; not after a load), the ending save (waits
    ARM_LIMIT game frames for a quiet boundary, counted per frame although
    the toolkit runs sa_boundary twice per boundary), the quit (resume
    written at the boundary, one hal_quit_now) and the quit timeout (the
    older resume deleted: fixed 2026-11-03); no refused transition."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(dir=TMP, prefix='state-machines-sa-')
        cls.bin = build_harness(os.path.join(cls.tmp, 'sa'), SA_SOURCES, SA_DEFINES)

    @classmethod
    def tearDownClass(cls):
        if not os.environ.get('STATE_MACHINES_KEEP'):
            shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_machine(self):
        rc, out = run_harness(self.bin, 'machine')
        self.assertEqual(rc, 0, out)
        for case in ('week', 'week_left', 'month', 'load_disarms', 'ending_wait_counts_frames',
                     'quit_writes_resume', 'no_illegal'):
            self.assertIn(f'ok {case}\n', out)

    def test_quit_timeout(self):
        rc, out = run_harness(self.bin, 'timeout', dict(PC_QUIT_WAIT_MS='0'))
        self.assertEqual(rc, 0, out)
        self.assertIn('ok quit_timeout_deletes_older_resume', out)

    def test_negative_controls(self):
        for i, m in enumerate(SA_MUTATIONS):
            with self.subTest(m[0]):
                d = os.path.join(self.tmp, f'mut{i}')
                os.makedirs(d, exist_ok=True)
                b = build_harness(os.path.join(d, 'sa'), SA_SOURCES, SA_DEFINES, m)
                rc, out = run_harness(b, m[4], dict(PC_QUIT_WAIT_MS='0'))
                self.assertNotEqual(rc, 0, f'{m[0]}: not caught\n{out}')
                self.assertIn('FAIL ', out)


@unittest.skipUnless(available(), 'needs the built port and pc/assets')
class HostResourcesNotInStates(unittest.TestCase, Port):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(dir=TMP, prefix='state-machines-host-')
        cls.state = os.path.join(cls.tmp, 'enhanced.tks')
        cls.saved = cls.port('save', f'at 400 save {cls.state}\nafter 1 quit\n', ENHANCED)

    @classmethod
    def tearDownClass(cls):
        if not os.environ.get('STATE_MACHINES_KEEP'):
            shutil.rmtree(cls.tmp, ignore_errors=True)

    @unittest.skipIf(BINARY.endswith('.exe'),
                     'Windows: the heap sits at low addresses, where clock counts and data '
                     'words look like pointers (the cross-process load below still runs)')
    def test_audit_finds_no_host_pointer(self):
        rc, text, _ = self.saved
        self.assertEqual(rc, 0, text[-3000:])
        self.assertIn('[HIRES] internal resolution 2x', text)
        a = subprocess.run([PY, str(ROOT / 'pc/tools/savestate_tool.py'), 'audit', '--bin', BINARY,
                            self.state], capture_output=True, text=True)
        found = {line.split(': ')[0] for line in a.stdout.splitlines() if ': ' in line}
        self.assertEqual(found - VALUE_COINCIDENCES, set(), a.stdout)

    def test_another_process_loads_it(self):
        rc, text, _ = self.port('load', f'at 0 load {self.state}\nafter 60 quit\n', ENHANCED)
        self.assertEqual(rc, 0, text[-3000:])
        self.assertRegex(text, r'\[DBG\] ok loaded .*frame 400 ')
        self.assertFalse('[CRASH]' in text, 'the watchdog fired: ' + text[text.find('[CRASH]'):][:400])
        self.assertIn('[DBG] quit (code 0) at frame 460', text)


@unittest.skipUnless(available(), 'needs the built port and pc/assets')
class EveryRestoreTellsEveryObserver(unittest.TestCase, Port):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(dir=TMP, prefix='state-machines-restore-')
        cls.state = os.path.join(cls.tmp, 'at300.tks')
        env = dict(ENHANCED, PC_RESTORE_LOG='1', TOKI_REWIND='1')
        cls.out = {
            'toolkit': cls.port('toolkit', f'at 300 save {cls.state}\nafter 30 load {cls.state}\n'
                                          'after 30 quit\n', env),
            'quickload': cls.port('quickload', 'at 300 qsave 3\nafter 30 qload 3\nafter 30 quit\n', env),
            'rewind': cls.port('rewind', 'at 300 rewind 2\nafter 30 quit\n', env),
        }
        cls.out['loadstate'] = cls.port('loadstate', 'after 30 quit\n', dict(env, PC_LOADSTATE=cls.state))

    @classmethod
    def tearDownClass(cls):
        if not os.environ.get('STATE_MACHINES_KEEP'):
            shutil.rmtree(cls.tmp, ignore_errors=True)

    def told(self, tag):
        rc, text, _ = self.out[tag]
        self.assertEqual(rc, 0, text[-3000:])
        return re.findall(r'\[RESTORE\] (\w+): ([a-z ]*)', text)

    def test_toolkit_load(self):
        self.assertEqual(self.told('toolkit'), [('file', FILE_OBSERVERS)])

    def test_loadstate(self):
        self.assertEqual(self.told('loadstate'), [('file', FILE_OBSERVERS)])

    def test_quickload(self):
        self.assertEqual(self.told('quickload'), [('file', FILE_OBSERVERS)])

    def test_rewind(self):
        self.assertEqual(self.told('rewind'), [('rewind', REWIND_OBSERVERS)])


@unittest.skipUnless(available(), 'needs the built port and pc/assets')
class QuitDuringAGame(unittest.TestCase, Port):
    """The quit / resume machine against the frame loop and the rewind, in
    a game (new_game.txt's first school day, absolute input):

    * the window's close button during an interactive rewind (the held key,
      TOKI_REWIND_HOLD=N:3:quit) while the save writer is busy (a quicksave
      started the boundary before, on a slow disk: PC_QS_WRITE_DELAY_MS): the quit waits for the writer and the
      resume state is written.  Before 2026-11-03 the rewind only peeked at
      the SDL_QUIT event; the next frame's event poll delivered it again as
      a second request, which quits at once -- no resume state.  Negative
      control: rewind.c with SDL_PEEKEVENT again (STATE-MACHINES.md);
    * after the resume state is written at the boundary, not one more game
      frame runs (PC_MARK_EVERY=1 prints one [MARK] per VBlank).  Before,
      the loop ran a whole frame after the state was written."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(dir=TMP, prefix='state-machines-quit-')
        reached, cls.abs_input, _ = rt.resolve(str(ROOT / 'pc/tools/playthrough/new_game.txt'),
                                               cls.tmp, BINARY, 900)
        cls.n = reached['lesson'] + 50
        cls.inp = os.path.join(cls.tmp, 'abs.input')
        with open(cls.inp, 'w') as fh:
            fh.write(cls.abs_input)

    @classmethod
    def tearDownClass(cls):
        if not os.environ.get('STATE_MACHINES_KEEP'):
            shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_close_during_rewind_writes_resume(self):
        n = self.n
        # the quicksave one boundary before keeps the writer busy 1.5 s
        # (PC_QS_WRITE_DELAY_MS, a slow disk)
        rc, text, _ = self.port('rewind-quit', f'at {n - 1} qsave 3\nafter 3000 quit\n',
                                dict(PC_INPUT=self.inp, PC_RESUME_FORCE='1', TOKI_REWIND='1',
                                     TOKI_REWIND_HOLD=f'{n}:3:quit', PC_QS_WRITE_DELAY_MS='1500'))
        self.assertEqual(rc, 0, text[-3000:])
        self.assertIn(f'[REWIND] rewinding from frame {n}', text)
        self.assertTrue(re.search(r'\[RESUME\] resume state written at frame \d+', text), text[-1500:])
        self.assertIn('[HAL] Shutdown', text)

    def test_no_frame_after_the_resume_state(self):
        n = self.n
        rc, text, _ = self.port('winquit', f'at {n} winquit\n',
                                dict(PC_INPUT=self.inp, PC_RESUME_FORCE='1', PC_MARK_EVERY='1'))
        self.assertEqual(rc, 0, text[-3000:])
        i = text.find(f'[RESUME] resume state written at frame {n}')
        self.assertGreaterEqual(i, 0, text[-3000:])
        self.assertIn('[MARK] game frame ', text[:i])
        self.assertNotIn('[MARK]', text[i:], 'a game frame ran after the resume state was written')


@unittest.skipUnless(available(), 'needs the built port and pc/assets')
class OneOverlayAtATime(unittest.TestCase, Port):
    """The presenter's hold (pc/hal/present.c): the backlog and the settings
    menu are never open together.  Ctrl+S (the save browser) over the open
    backlog did open the menu before 2026-11-03 (MENU and STATUS had the
    guard, SAVES not): drawn over the backlog, while the keys went to the
    invisible backlog."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(dir=TMP, prefix='state-machines-overlay-')

    @classmethod
    def tearDownClass(cls):
        if not os.environ.get('STATE_MACHINES_KEEP'):
            shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_saves_key_over_the_backlog(self):
        keys = os.path.join(self.tmp, 'keys')
        with open(keys, 'w') as fh:
            fh.write('300 key Ctrl+B Ctrl+S Escape\n')
        # before the fix the menu stayed open over the closed backlog and held
        # the game for ever (the `quit` at frame 400 never came): bounded here
        try:
            rc, text, _ = self.port('backlog-saves', 'at 400 quit\n',
                                    dict(AUTOPLAY='1', TOKI_PROFILE='enhanced',
                                         TOKI_INPUT_SCRIPT=keys), timeout=180)
        except subprocess.TimeoutExpired:
            self.fail('the game stayed held (a menu left open over the backlog)')
        self.assertEqual(rc, 0, text[-3000:])
        self.assertIn('[BACKLOG] open', text)
        self.assertIn('[BACKLOG] closed', text)
        self.assertFalse('settings menu open (saves)' in text,
                         'the save browser opened over the backlog')


@unittest.skipUnless(available(), 'needs the built port and pc/assets')
class PacedToolkitLoad(unittest.TestCase, Port):
    """The toolkit's `load` (also the F9 debug key) of a state 38 s of
    emulated time later, at frame 120 of a paced session: the pacing starts
    from the loaded clock.  Before: the next frame slept the 38 s gap, the
    watchdog fired ([CRASH]) and the run took ~50 s (measured with the
    pre-fix build)."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(dir=TMP, prefix='state-machines-paced-')

    @classmethod
    def tearDownClass(cls):
        if not os.environ.get('STATE_MACHINES_KEEP'):
            shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_paced_load_keeps_real_time(self):
        state = os.path.join(self.tmp, 'at2400.tks')
        rc, text, _ = self.port('save', f'at 2400 save {state}\nafter 1 quit\n', dict(AUTOPLAY='1'))
        self.assertEqual(rc, 0, text[-3000:])
        rc, text, wall = self.port('paced', f'at 120 load {state}\nafter 60 quit\n',
                                  dict(AUTOPLAY='1', PC_TURBO='0'))
        self.assertEqual(rc, 0, text[-3000:])
        self.assertRegex(text, r'\[DBG\] ok loaded .*frame 2400 ')
        self.assertFalse('[CRASH]' in text, 'the watchdog fired: ' + text[text.find('[CRASH]'):][:400])
        # 180 paced frames are three seconds (plus the boot); the gap is ~38 s
        self.assertLess(wall, 30, 'the paced session slept away the emulated time')


if __name__ == '__main__':
    unittest.main()
