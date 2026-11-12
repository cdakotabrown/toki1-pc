# SPDX-License-Identifier: GPL-3.0-or-later
"""The PC UI and input machines of docs/pc-port/STATE-MACHINES.md section 5
(present.c, menu, pad, setup screen) and the save / rewind risks of
section 6, each against the real binary (headless) or a C harness.

Fixed 2026-11-03, one test each (the negative controls are recorded in
STATE-MACHINES.md section 4):

* A2  a hold (pause, focus, menu) ended by Ctrl+Space with Ctrl let go
      first handed Space's circle to the game -- the hold-off is armed at
      every hold end and counts every bound key, also one Ctrl hid;
* A3  a mouse button let go during a hold stayed down for the game;
* A5  Esc or the close button on "the game data is ready" started the game;
* A6  the pause and the menu kept the relative-mode grab / hidden cursor;
* A8  the one on-screen message slot lost messages (now a queue, timed
      while shown; C harness present_notes_harness.c);
* A9  hotkey changes are session-only: decided as documented (pinned here);
* A11 settings changed in the same event drain as a quit were lost;
* A14 a quicksave / quickload hotkey while held was deferred silently;
* R3  a pending save dropped by a load or a second save said nothing;
* R7  a quicksave (and a rewind snapshot) could capture a memory-card
      transfer in flight (the autosave already waited);
* R13 the rewind stayed off after a failed set-up, and a spent test hold
      survived into the player's first rewind (C harness rewind_harness.c);
* R1  a new PC-layer module's data was machine state by default, unseen:
      every pc/hal file with data in a state must be KEEP or listed in
      dbg_state.c MACHINE_FILES.

STATE_MACHINES_BINARY=PATH runs the port tests against another build (the
negative controls); STATE_MACHINES_KEEP=1 keeps the folders (build/tmp/);
PC_TEST_PLATFORM=windows runs them on the Windows build under Wine (a
private prefix per run).  Needs the built port and pc/assets (and, for A5,
the disc in disks/); skipped otherwise.
"""
import contextlib
import glob
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import game_platform  # noqa: E402
import savestate_roundtrip as rt  # noqa: E402
import savestate_tool as sst  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
from pc_paths import decomp, disc_input  # noqa: E402
BINARY = os.environ.get('STATE_MACHINES_BINARY') or str(game_platform.GAME)
ASSETS = ROOT / 'pc/assets'
TMP = ROOT / 'build/tmp'
TMP.mkdir(parents=True, exist_ok=True)
KEEP = bool(os.environ.get('STATE_MACHINES_KEEP'))
RELEASE = 'Tokimeki Memorial - Forever with You (Japan) (Rev 1)'
DISC = [disc_input('disks') / n for n in (f'{RELEASE}.cue', f'{RELEASE} (Track 1).bin',
                                     f'{RELEASE} (Track 2).bin')]


def available():
    return os.path.isfile(BINARY) and (ASSETS / 'raw/PSX.EXE').is_file() and \
        (not BINARY.endswith('.exe') or game_platform.wine_available())


def run_game(work, log, env_extra, timeout=600, args=None, binary=None):
    """The game in WORK (pc/assets and disks linked in, as
    savestate_roundtrip.run_port), headless; a Windows .exe in a private
    Wine prefix.  -> (exit status, output)"""
    binary = binary or BINARY
    os.makedirs(os.path.join(work, 'pc'), exist_ok=True)
    for name, target in (('pc/assets', ROOT / 'pc/assets'), ('disks', disc_input('disks'))):
        p = os.path.join(work, name)
        if not os.path.lexists(p):
            os.symlink(target, p)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('PC_', 'SCENE', 'TOKI_', 'AUTOPLAY', 'GP0_', 'MDFN_', 'SDL_'))}
    env.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', PC_TURBO='1',
               TOKI_INPUT_DEVICE='pad', WINEDEBUG='-all')
    env.update(env_extra)
    cmd, env, cleanup = game_platform.wrap(
        [binary] + ([str(ROOT / 'pc/assets')] if args is None else list(args)), env)
    try:
        with open(log, 'w') as fh:
            r = subprocess.run(cmd, cwd=work, env=env, stdout=fh, stderr=subprocess.STDOUT,
                               timeout=timeout)
    finally:
        cleanup()
    with open(log, errors='replace') as fh:
        return r.returncode, fh.read()


class Case(unittest.TestCase):
    prefix = 'ui-sm-'

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(dir=TMP, prefix=cls.prefix)

    @classmethod
    def tearDownClass(cls):
        if not KEEP:
            shutil.rmtree(cls.tmp, ignore_errors=True)

    def port(self, tag, keys=None, cmds=None, extra=None, timeout=600):
        """one run: TOKI_INPUT_SCRIPT KEYS, PC_DBG_CMDS CMDS (texts)"""
        d = os.path.join(self.tmp, tag)
        os.makedirs(d, exist_ok=True)
        env = dict(TOKI_SAVE_DIR=os.path.join(d, 'saves'))
        if keys is not None:
            with open(os.path.join(d, 'keys'), 'w') as fh:
                fh.write(keys)
            env['TOKI_INPUT_SCRIPT'] = os.path.join(d, 'keys')
        if cmds is not None:
            with open(os.path.join(d, 'cmds'), 'w') as fh:
                fh.write(cmds)
            env['PC_DBG_CMDS'] = os.path.join(d, 'cmds')
        env.update(extra or {})
        rc, text = run_game(os.path.join(d, 'work'), os.path.join(d, 'log'), env, timeout)
        self.assertEqual(rc, 0, text[-3000:])
        return text, d


def dumps(text):
    """{tick: (pad_data, mouse)} of the input script's `dump` lines"""
    out = {}
    for m in re.finditer(r'\[PADSCRIPT\] tick=(\d+) .*?pad_data=([0-9a-f]+) .*?mouse=(\d\d)', text):
        out[int(m.group(1))] = (int(m.group(2), 16), m.group(3))
    return out


CIRCLE = 0x20       # the game's pad_data bit of circle (Space, Enter)


@unittest.skipUnless(available(), 'needs the built port and pc/assets')
class HoldEndsCleanly(Case):
    """The presenter's hold (pc/hal/present.c pc_present_pause_loop) and
    the input that ends it.  Scripted with TOKI_INPUT_SCRIPT: everything
    due at one tick is queued at once, and the pause loop takes it event
    by event, as a fast player's keys."""
    prefix = 'ui-sm-hold-'

    def test_pause_key_released_out_of_order(self):
        """A2: Ctrl+Space pauses; Ctrl+Space again resumes, Ctrl let go
        before Space.  Space alone is circle: it must not reach the game
        until released.  Positive control: Space pressed again later is
        circle (the hold-off ends)."""
        # (at the title, where the game polls the pad every frame)
        text, _ = self.port('a2', keys='2400 keydown Ctrl Space\n2400 keyup Space\n'
                                       '2400 keydown Space\n2400 keyup Ctrl\n'
                                       '2410 dump\n2420 dump\n2440 keyup Space\n2445 dump\n'
                                       '2460 keydown Space\n2465 dump\n2470 keyup Space\n',
                            cmds='at 2500 quit\n')
        self.assertIn('[PRESENT] paused', text)
        self.assertIn('[PRESENT] resumed', text)
        d = dumps(text)
        for t in (2410, 2420, 2445):
            self.assertFalse(d[t][0] & CIRCLE, f'tick {t}: the resume key reached the game ({d})')
        self.assertTrue(d[2465][0] & CIRCLE, f'a later press did not reach the game ({d})')

    def test_mouse_release_during_pause(self):
        """A3: a click held into a pause and let go during it is let go for
        the game.  Positive control: a click after the pause is seen."""
        text, _ = self.port('a3', keys='400 mbutton left 1\n400 key Ctrl+Space\n'
                                       '400 mbutton left 0\n400 key Ctrl+Space\n'
                                       '410 dump\n450 dump\n460 mbutton left 1\n465 dump\n'
                                       '470 mbutton left 0\n',
                            cmds='at 500 quit\n', extra=dict(TOKI_INPUT_DEVICE='mouse'))
        d = dumps(text)
        self.assertEqual(d[410][1], '00', f'the left button stayed down after the pause ({d})')
        self.assertEqual(d[450][1], '00', d)
        self.assertEqual(d[465][1], '10', f'a click after the pause was not seen ({d})')

    def test_pause_releases_the_grab(self):
        """A6: relative mouse mode, the game in mouse mode (grabbed): the
        pause releases the grab and shows the cursor, resuming grabs again."""
        text, _ = self.port('a6', keys='400 key Ctrl+Space Ctrl+Space\n',
                            cmds='at 500 quit\n',
                            extra=dict(TOKI_INPUT_DEVICE='mouse', TOKI_MOUSE_MODE='relative',
                                       PC_DEBUG='pad'))
        i = text.find('[PRESENT] paused')
        j = text.find('[PRESENT] resumed')
        self.assertGreater(i, 0, text[-2000:])
        self.assertIn('[PAD] cursor grabbed', text[:i], 'the game never grabbed: no test')
        self.assertIn('[PAD] cursor released (pad_type=0, game held)', text[i:j],
                      'the pause kept the grab')
        self.assertIn('[PAD] cursor grabbed', text[i:j + 400])

    def test_quicksave_while_paused_is_said(self):
        """A14: F5 while paused: the save is made when the game resumes (the
        main-loop boundary), and the pause label says so meanwhile -- the
        paused frames are not all the same picture.  TOKI_QS_HOTKEYS=1: the
        quicksave hotkeys in an automated run."""
        d = os.path.join(self.tmp, 'a14.hash')
        text, _ = self.port('a14', keys='400 key Ctrl+Space F5 Ctrl+Space\n', cmds='at 500 quit\n',
                            extra=dict(TOKI_QS_HOTKEYS='1', PC_PRESENT_HASH=d))
        self.assertIn('[PRESENT] Quicksave when you resume', text)
        self.assertRegex(text, r'\[QUICKSAVE\] slot 1 saved')
        paused = {l.split()[1] for l in Path(d).read_text().splitlines() if l.endswith(' paused')}
        self.assertGreaterEqual(len(paused), 2, 'the pause label never changed: the deferred save was silent')


@unittest.skipUnless(available(), 'needs the built port and pc/assets')
class SettingsAndQuit(Case):
    """The settings menu's changes against a quit, and the hotkeys'."""
    prefix = 'ui-sm-settings-'

    def test_menu_change_and_close_in_one_drain(self):
        """A11: the menu key, a change and the window's close button in one
        event drain outside the pause loop (the toolkit's pause drains with
        hal_poll_quit, as the main loop does after a frame): the change is
        still saved (here: reported as not saved, an automated run).
        Before, the run ended with the menu open and nothing saved."""
        text, _ = self.port('a11', cmds='at 400 keys Ctrl+O Down Right\nwinclose\npause\n')
        self.assertIn('[PRESENT] settings menu open', text)
        self.assertIn('[PRESENT] settings not saved: automated run: display.scale', text)
        self.assertIn('[PRESENT] settings menu closed', text)

    def test_hotkey_changes_are_session_only(self):
        """A9 (decided: intended, PLAYER-GUIDE.md "Settings"): a hotkey's
        change (Ctrl+- master volume) applies at once and is not written;
        a menu change made after it is (here: listed as not saved, an
        automated run) -- and only it.  Control: the menu's own change is
        listed, so a written hotkey key would be too."""
        text, _ = self.port('a9', cmds='at 400 keys Ctrl+- Ctrl+O Down Right Escape\nafter 30 quit\n')
        self.assertIn('[PRESENT] Volume 90%', text)
        line = re.search(r'\[PRESENT\] settings not saved: automated run:(.*)', text)
        self.assertIsNotNone(line, text[-2000:])
        self.assertEqual(line.group(1).split(), ['display.scale'])


@unittest.skipUnless(available(), 'needs the built port and pc/assets')
class SaveRequestsAreReported(Case):
    """R3: requests that will not be carried out are said (a note, queued
    behind the next toast).  The toolkit's `qsave` / `qload` are the
    hotkeys' and the save browser's requests."""
    prefix = 'ui-sm-requests-'

    def test_second_save_and_load_over_a_save(self):
        text, _ = self.port('r3', cmds='at 300 qsave 2\nqsave 3\nqload 1\nafter 5 quit\n')
        self.assertIn('[QUICKSAVE] Saving slot 3 instead of slot 2', text)
        self.assertIn('[QUICKSAVE] Save to slot 3 cancelled: a load came first', text)
        self.assertIn('[PRESENT] note: Save to slot 3 cancelled: a load came first', text)
        self.assertNotIn('slot 2 saved', text)


@contextlib.contextmanager
def private_wine():
    """savestate_roundtrip.resolve runs the game with run_port: a Windows
    .exe there gets a private prefix too (run_game)"""
    old = rt.run_port

    def run_port(binary, work, log, env_extra, timeout):
        return run_game(work, log, env_extra, timeout, binary=binary)
    rt.run_port = run_port
    try:
        yield
    finally:
        rt.run_port = old


def card_quiet(state):
    """the memory card driver in a state: no request, directory pass or
    file transfer in flight (pc/hal/memcard.c CardRequest / FileTransfer)"""
    recs = sst.read_state(state)['records']
    _, req = recs['memcard.c:g_requests']
    _, xfer = recs['memcard.c:g_file_xfer']
    # sizeof(CardRequest): int, int, u8 *, u8[128], 3 x int, u8[36][128],
    # u8[128], 3 x int = 4904 on both platforms (the record can be longer:
    # a PE symbol without a size runs to the next symbol)
    one = 4904
    assert len(req) >= 2 * one, len(req)
    busy = []
    for slot in (0, 1):
        op, = struct.unpack_from('<i', req, slot * one)
        stage, = struct.unpack_from('<i', req, slot * one + 152)    # directory_stage
        if op or stage >= 0:
            busy.append(f'slot {slot}: operation {op}, directory stage {stage}')
    if struct.unpack_from('<i', xfer, 0)[0]:
        busy.append('a file transfer')
    return busy


@unittest.skipUnless(available(), 'needs the built port and pc/assets')
class QuicksaveWaitsForTheCard(Case):
    """R7: new_game.txt's absolute input creates the system file on the
    card (frame ~2440).  A quicksave requested at a boundary where a card
    request is in flight (the toolkit's `when`) waits for the card, as the
    autosave does: the state written holds no transfer in flight.  Before,
    it was taken at once with the driver mid-request -- its load would
    finish that old request onto the newer live card image."""
    prefix = 'ui-sm-card-'

    def test_quicksave_waits_for_the_memory_card(self):
        with private_wine():
            _, abs_input, _ = rt.resolve(str(ROOT / 'pc/tools/playthrough/new_game.txt'),
                                         self.tmp, BINARY, 900)
        inp = os.path.join(self.tmp, 'abs.input')
        with open(inp, 'w') as fh:
            fh.write(abs_input)
        text, d = self.port('r7', cmds='when memcard.c:g_requests:4!=0 do qsave 3\nafter 120 quit\n',
                            extra=dict(PC_INPUT=inp), timeout=900)
        self.assertRegex(text, r'cmds line 1 at frame \d+: qsave 3')
        self.assertIn('[QUICKSAVE] the save waits for the memory card', text)
        state = os.path.join(d, 'saves/quicksave/slot3.tks')
        self.assertTrue(os.path.isfile(state), text[-2000:])
        self.assertEqual(card_quiet(state), [], 'the quicksave holds a card transfer in flight')


@unittest.skipUnless(available() and all(p.exists() for p in DISC),
                     'needs the built port, pc/assets and the disc in disks/')
class SetupReadyScreen(Case):
    """A5: the first-run setup (pc/hal/setup_screen.c) on "the game data is
    ready": Esc and the window's close button leave -- the game does not
    start -- and the data is kept, so the next launch starts the game
    without the setup.  (Decided: Esc closes here as on every other setup
    screen; the screen moves on by itself after 1.2 s, so "stay" would mean
    nothing.)  Before, both started the game."""
    prefix = 'ui-sm-setup-'

    def install(self, tag):
        base = Path(self.tmp) / tag
        game = base / 'game'
        (game / 'disk').mkdir(parents=True)
        (base / 'home').mkdir()
        (base / 'cwd').mkdir()
        shutil.copy(BINARY, game / os.path.basename(BINARY))
        for p in DISC:
            os.symlink(p.resolve(), game / 'disk' / p.name)
        return base, game

    def launch(self, base, game, script=None):
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(('PC_', 'TOKI_', 'SDL_', 'XDG_', 'SCENE', 'AUTOPLAY'))}
        env.update(HOME=str(base / 'home'), SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy',
                   TOKI_PORTABLE='1', PC_TURBO='1', PC_TRACE=str(base / 'trace'),
                   PC_TRACE_FRAMES='30', TOKI_INPUT_DEVICE='pad')
        if script:
            env['TOKI_SETUP_SCRIPT'] = script
        cmd, env, cleanup = game_platform.wrap([str(game / os.path.basename(BINARY))], env)
        try:
            r = subprocess.run(cmd, cwd=base / 'cwd', env=env, capture_output=True, text=True,
                               errors='replace', timeout=900)
        finally:
            cleanup()
        log = game / 'data/tokimeki-pc.log'
        return r, log.read_text(errors='replace') if log.exists() else ''

    def check_left(self, tag, script):
        base, game = self.install(tag)
        r, log = self.launch(base, game, script)
        self.assertEqual(r.returncode, 0, r.stdout[-2000:])
        self.assertIn('[SETUP] game data ready', log)
        self.assertIn('[SETUP] closed on "the game data is ready"', log)
        self.assertNotIn('PC_TRACE_FRAMES=30 reached', r.stdout, 'the game started')
        self.assertFalse((base / 'trace').exists() and (base / 'trace').stat().st_size,
                         'the game ran frames')
        return base, game

    def test_escape_on_ready_leaves(self):
        base, game = self.check_left('esc', 'key:escape')
        # the data is kept: the next launch starts the game, no setup
        r, log = self.launch(base, game)
        self.assertEqual(r.returncode, 0, r.stdout[-2000:])
        self.assertIn('PC_TRACE_FRAMES=30 reached', r.stdout, r.stdout[-2000:])
        self.assertNotIn('[SETUP] preparing', log.split('[SETUP] closed on')[-1])

    def test_close_on_ready_leaves(self):
        # the script's `quit` is the close button (SDL_QUIT sets the same flag)
        self.check_left('close', 'quit')


# ---------------------------------------------------------------- harnesses

CC = shutil.which('cc') or shutil.which('gcc')
CFLAGS = ['-std=gnu17', '-w', '-funsigned-char', '-D_GNU_SOURCE', '-DPC_PORT', '-I', 'pc/include/game',
          '-I', 'pc/include', '-I', decomp('include'), '-I', 'pc/hal', '-include', 'pc/include/game/types.h']


def build_harness(out, sources, libs=(), mutation=None):
    """compile SOURCES (repo paths) into OUT; MUTATION (desc, path, old, new,
    mode) is applied to a copy of its file first"""
    srcs = []
    for src in sources:
        path = str(ROOT / src)
        if mutation and mutation[1] == src:
            text = Path(path).read_text()
            if mutation[2] not in text:
                raise AssertionError(f'mutation {mutation[0]!r}: its text is not in {src}')
            path = os.path.join(os.path.dirname(out), os.path.basename(src))
            Path(path).write_text(text.replace(mutation[2], mutation[3], 1))
        srcs.append(path)
    r = subprocess.run([CC] + CFLAGS + ['-o', out] + srcs + list(libs), cwd=ROOT,
                       capture_output=True, text=True)
    if r.returncode:
        raise AssertionError(r.stderr[-3000:])
    return out


def run_harness(binary, mode=None):
    r = subprocess.run([binary] + ([mode] if mode else []), capture_output=True, text=True,
                       timeout=120)
    return r.returncode, r.stdout


class HarnessCase:
    """a C harness: every mode passes; each mutation makes its case FAIL"""
    sources, libs, mutations, modes = (), (), (), (None,)

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(dir=TMP, prefix='ui-sm-harness-')
        cls.bin = build_harness(os.path.join(cls.tmp, 'h'), cls.sources, cls.libs)

    @classmethod
    def tearDownClass(cls):
        if not KEEP:
            shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_cases(self):
        for mode in self.modes:
            with self.subTest(mode):
                rc, out = run_harness(self.bin, mode)
                self.assertEqual(rc, 0, out)
                self.assertNotIn('FAIL', out)

    def test_negative_controls(self):
        for i, m in enumerate(self.mutations):
            with self.subTest(m[0]):
                d = os.path.join(self.tmp, f'mut{i}')
                os.makedirs(d, exist_ok=True)
                b = build_harness(os.path.join(d, 'h'), self.sources, self.libs, m)
                rc, out = run_harness(b, m[4])
                self.assertNotEqual(rc, 0, f'{m[0]}: not caught\n{out}')
                self.assertIn(f'FAIL {m[5]}', out)


@unittest.skipUnless(CC, 'needs a C compiler')
class PresentNotes(HarnessCase, unittest.TestCase):
    """A8: the presenter's notes (pc/hal/present_layout.c): two notes at
    once are both shown, in order; a note covered by the menu / pause /
    another message waits, its time running only while it is shown.
    Before, one slot: the launch's "cannot be resumed" (sa_launch) was
    overwritten by "English unavailable" (hal_lang_boot_fixup), and a
    message shown under the menu expired unseen."""
    sources = ('pc/tests/present_notes_harness.c', 'pc/hal/present_layout.c')
    libs = ('-lm',)
    mutations = (
        ('a new note replaces the one shown (the old single slot)', 'pc/hal/present_layout.c',
         '    q->n++;\n    return 0;',
         '    q->head = (q->head + q->n) % PRESENT_NOTES;\n    q->n = 1;\n    q->shown = 0;\n'
         '    return 0;', None, 'two_in_order'),
        ('the time runs while covered', 'pc/hal/present_layout.c',
         '        q->shown = 0;               /* covered: its time stands still */\n', '',
         None, 'covered_waits'),
    )


@unittest.skipUnless(CC, 'needs a C compiler')
class RewindMachine(HarnessCase, unittest.TestCase):
    """pc/hal/rewind.c's switch and boundary machine: R13 (a failed set-up
    is retried after the menu turns the rewind off and on; a test hold
    spent with nothing kept does not come back in the player's first
    rewind) and R7 (no snapshot and no rewind while the memory card has a
    transfer in flight)."""
    sources = ('pc/tests/rewind_harness.c', 'pc/hal/rewind.c', 'pc/hal/rewind_codec.c')
    libs = ('-lpthread',)
    modes = ('retry', 'card', 'sim')
    mutations = (
        ('a failed set-up stays failed', 'pc/hal/rewind.c',
         '        if (was_on && g_init != 0) free_all();', '        if (was_on && g_init > 0) free_all();',
         'retry', 'setup_retried_after_toggle'),
        ('snapshots ignore the memory card', 'pc/hal/rewind.c',
         '    if (memcard_busy && memcard_busy()) {\n        g_busy++;\n        return;\n    }\n', '',
         'card', 'no_snapshot_while_card_busy'),
        ('a spent test hold stays active', 'pc/hal/rewind.c',
         '        g_sim_active = 0;       /* TOKI_REWIND_HOLD fired with nothing kept: spent */\n', '',
         'sim', 'player_rewind_is_not_the_test_hold'),
    )


# ------------------------------------------------- save-state ownership (R1)

PC_LAYER = {os.path.basename(p) for p in glob.glob(str(ROOT / 'pc/hal/**/*.c'), recursive=True) +
            glob.glob(str(ROOT / 'pc/hal/**/*.cpp'), recursive=True)}


def read_inventory(path):
    """-> (rows [(class, key, file)], machine files) of a PC_STATE_INVENTORY dump"""
    rows, machine = [], set()
    for line in Path(path).read_text().splitlines():
        if line.startswith('# machine-file '):
            machine.add(line.split(' ', 2)[2])
        elif not line.startswith('#'):
            f = line.split('\t')
            rows.append((f[0], f[1], f[4] if len(f) > 4 else ''))
    return rows, machine


def unlisted(rows, machine, pc_layer=PC_LAYER):
    """{file: [keys]}: PC-layer files with data in a state (saved or partly
    saved records) that MACHINE_FILES does not list"""
    out = {}
    for cls_, key, f in rows:
        if cls_ in ('saved', 'partial') and f in pc_layer and f not in machine:
            out.setdefault(f, []).append(key)
    return out


@unittest.skipUnless(available(), 'needs the built port and pc/assets')
class StateOwnershipIsListed(Case):
    """R1: every pc/hal source file whose data a save state holds is listed
    on purpose in pc/hal/dbg_state.c MACHINE_FILES; the others are KEEP.  A
    new PC module that adds data (a feature's flags, a heap pointer) fails
    here until someone decides which it is -- before, it was machine state
    by default and a load wrote it back.  The inventory is the running
    build's (PC_STATE_INVENTORY: every data symbol, its class and file).
    Negative controls: a listed file taken off the list, and a new module's
    object, are both reported."""
    prefix = 'ui-sm-ownership-'

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.inv = os.path.join(cls.tmp, 'inv.tsv')
        rc, cls.text = run_game(os.path.join(cls.tmp, 'work'), os.path.join(cls.tmp, 'log'),
                                dict(PC_SAVESTATE=f'300:{os.path.join(cls.tmp, "s.tks")}',
                                     PC_STATE_INVENTORY=cls.inv, PC_TRACE_FRAMES='305',
                                     PC_TRACE=os.path.join(cls.tmp, 'trace')))

    def test_every_pc_module_in_a_state_is_listed(self):
        self.assertTrue(os.path.isfile(self.inv), self.text[-2000:])
        rows, machine = read_inventory(self.inv)
        self.assertTrue(machine, 'no MACHINE_FILES in the inventory (an older build?)')
        self.assertEqual(unlisted(rows, machine), {},
                         'pc/hal files with data in states that dbg_state.c MACHINE_FILES does not list '
                         '(list them, or keep them out with KEEP_FILES / KEEP_SYMS)')
        # the list names real PC-layer files that a state does hold
        held = {f for c, _, f in rows if c in ('saved', 'partial')}
        self.assertEqual(sorted(machine - PC_LAYER), [], 'MACHINE_FILES names a file outside pc/hal')
        self.assertEqual(sorted(machine - held), [], 'MACHINE_FILES names a file with nothing in a state')

    def test_negative_controls(self):
        rows, machine = read_inventory(self.inv)
        self.assertIn('gpu.c', unlisted(rows, machine - {'gpu.c'}))
        new = rows + [('saved', 'newfeature.c:g_armed', 'newfeature.c')]
        self.assertEqual(unlisted(new, machine, PC_LAYER | {'newfeature.c'}),
                         {'newfeature.c': ['newfeature.c:g_armed']})


if __name__ == '__main__':
    unittest.main()
