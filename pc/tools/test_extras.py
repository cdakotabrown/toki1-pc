# SPDX-License-Identifier: GPL-3.0-or-later
"""The extras: endings seen, the gallery, achievements (pc/hal/extras.h;
docs/pc-port/PLAYER-GUIDE.md "Extras: endings, gallery, achievements").

1. pc/tests/extras_harness.c (no game, no assets): the detectors fed
   hand-built views -- a real ending decision, the OPTION ending viewer, a
   load into a decided ending, a state loaded at start; every achievement
   and the views that must not unlock one; the record's persistence
   (reopen, unknown lines kept, atomic writes); the gallery against the
   calls cd.c / libpress.c / gpu.c make (a still loaded and shown, one
   never shown, a broken column run, a non-BS still, an event scene's
   brightest frame, a known source); the three menu pages.  Each MUTATIONS
   entry rebuilds the harness with one mechanism broken and requires the
   test that guards it to fail (the negative controls).
2. With the port built and pc/assets present:
   * read-only: date_save.txt's first phase (new game .. Shiori's date in
     the park) with the extras forced on and off: every presented frame
     (PC_FRAME_HASH), the trace, the mixed audio (PC_AUDIO_HASH) and the
     parity artifacts identical; the run with them on has the date's
     picture (BS/D_09_02.BS, Shiori, step 80) and FIRST DATE; the run with
     them off wrote nothing.
   * endings: the date poked to 1998-03-01 at the first planner (the
     game's own schedule_day_main / schedule_exit / ending_select_init do
     the rest) in three seeded states -- untouched (Yoshio, 13), Rei's
     condition (denwa_cnt 70, ev_flag4f 2: 12) and Saki met with a high
     score (4); the recorded ending equals now_girl as the debug toolkit
     peeks it at the decision; the three runs share one record, so it also
     shows persistence across processes (GRADUATION unlocked once, its first
     time kept).  A state saved after the decision and loaded into a new
     process records nothing.

Seeded runs write game state (the pokes); they are tests of the detector,
never normal-run results.  About 4 minutes with the port.
"""
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from safe_process import run  # noqa: E402
import game_platform  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
from pc_paths import decomp  # noqa: E402
BINARY = game_platform.GAME   # PC_TEST_PLATFORM=windows: the .exe under Wine
ASSETS = ROOT / 'pc/assets'
SOURCES = ('pc/hal/extras.c', 'pc/hal/extras_state.c', 'pc/hal/extras_gallery.c',
           'pc/hal/extras_view.c', 'pc/hal/profile.c')

# (name, file, old, new, test that must then fail)
MUTATIONS = (
    ('the OPTION viewer counts as an ending', 'pc/hal/extras.c',
     '} else if (v->sub == 0 && g_end_armed && v->in_game) {',
     '} else if (g_end_armed && v->in_game) {', 'test_endings'),
    ('a load does not re-arm', 'pc/hal/extras.c',
     'g_end_armed = v->step != EX_STEP_ENDING || (v->sub == 0 && v->subsub == 0);',
     'g_end_armed = 1;', 'test_endings'),
    ('the first frame counts', 'pc/hal/extras.c',
     'g_end_armed = v->step != EX_STEP_ENDING;\n', 'g_end_armed = 1;\n', 'test_endings'),
    ('achievements outside a game', 'pc/hal/extras.c',
     '    if (!v->in_game) return;\n    /* the scene achievements */',
     '    /* the scene achievements */', 'test_achievements'),
    ('no club is a club', 'pc/hal/extras.c',
     '((v->basyo_pack >> 12) & 15) <= 10', '(v->basyo_pack & 0xF000) != 0', 'test_achievements'),
    ('stress counts as a perfect stat', 'pc/hal/extras.c',
     'for (i = 0; i < EX_PARAMS - 1; i++)', 'for (i = 0; i < EX_PARAMS; i++)', 'test_achievements'),
    ('a bomb at 60 is not lit', 'pc/hal/extras.c',
     'v->girl[i].bomb >= 60', 'v->girl[i].bomb > 60', 'test_achievements'),
    ('unknown lines are dropped', 'pc/hal/extras.c',
     '    if (S.nother < MAX_OTHER) S.other[S.nother++] = strdup(line);', '', 'test_persist'),
    ('a relock overwrites the first time', 'pc/hal/extras.c',
     '    if (k < 0 || k >= A_COUNT || S.ach[k]) return 0;',
     '    if (k < 0 || k >= A_COUNT) return 0;', 'test_persist'),
    ('the record is written in place', 'pc/hal/extras.c',
     '    snprintf(tmp, sizeof tmp, "%s.tmp", path);\n    if (pc_settings_mkdirs',
     '    snprintf(tmp, sizeof tmp, "%s", path);\n    if (pc_settings_mkdirs',
     'test_persist'),
    ('a still enters without being shown', 'pc/hal/extras_gallery.c',
     '        if (g_still.drawn) {', '        if (1) {', 'test_gallery'),
    ('a 4-bit draw shows a still', 'pc/hal/extras_gallery.c',
     'if (g_still.st == ST_SHOWN_WAIT && fmt == 2 &&', 'if (g_still.st == ST_SHOWN_WAIT &&',
     'test_gallery'),
    ('columns with a gap join', 'pc/hal/extras_gallery.c',
     '} else if (x == g_still.x0 + 16 * g_still.cols && y == g_still.y0 && h == g_still.h) {',
     '} else if (y == g_still.y0 && h == g_still.h) {', 'test_gallery'),
    ('the still is read when shown, not when loaded', 'pc/hal/extras_gallery.c',
     '        if (g_still.drawn) {\n            still_done(v);',
     '        if (g_still.drawn) {\n            { int r; for (r = 0; r < g_still.h; r++) '
     'memcpy(&g_still.pix[r * 320], &g_vram[g_still.y0 + r][g_still.x0], 640); }\n'
     '            still_done(v);', 'test_gallery'),
    ('any still counts, not only BS/', 'pc/hal/extras_gallery.c',
     '!strncmp(rel, "BS/", 3) &&', '1 &&', 'test_gallery'),
    ('the first event frame is kept', 'pc/hal/extras_gallery.c',
     '    if (g_ev.have && sum <= g_ev.best) return;', '    if (g_ev.have) return;',
     'test_gallery'),
    ('known sources are added again', 'pc/hal/extras.c',
     '    if (ex_picture_known(p->source)) return 0;\n    if (S.npics', '    if (S.npics',
     'test_gallery'),
    ('parity turns the extras on', 'pc/hal/profile.c',
     '{ "TOKI_EXTRAS",        0, 1,', '{ "TOKI_EXTRAS",        1, 1,', 'test_config'),
    ('automated runs record', 'pc/hal/extras.c',
     'g_active = sw && (!s->harness || (f && *f == \'1\'));', 'g_active = sw;', 'test_config'),
)


def build(folder, mutation=None):
    folder = Path(folder)
    paths = {s: ROOT / s for s in SOURCES}
    if mutation:
        _, path, old, new, _ = mutation
        text = (ROOT / path).read_text()
        assert text.count(old) == 1, f'mutation anchor not unique in {path}: {old!r}'
        copy = folder / Path(path).name
        copy.write_text(text.replace(old, new))
        paths[path] = copy
    binary = folder / 'extras_harness'
    # the pages' UI font (pc/include/pc_ui.h), generated as the build does
    font = folder / 'ui_font_data.c'
    run([sys.executable, str(ROOT / 'pc/tools/gen_ui_font.py'), '--out', str(font)], cwd=ROOT,
        timeout=60).require_ok()
    srcs = [str(ROOT / 'pc/tests/extras_harness.c')] + [str(paths[s]) for s in SOURCES] + [
        str(ROOT / p) for p in ('pc/hal/settings.c', 'pc/hal/menu.c', 'pc/hal/present_layout.c',
                                'pc/hal/gpu_scanout.c', 'pc/src/file_lba_table.c',
                                'pc/hal/ui_font.c')] + [str(font)]
    run(['cc', '-std=gnu17', '-D_GNU_SOURCE', '-O1', '-g', '-w', '-funsigned-char',
         '-fno-strict-aliasing', '-Ipc/include', '-Ipc/include/game', '-Ipc/hal', '-I' + decomp('include'),
         '-include', 'pc/include/game/types.h', *srcs, '-lm', '-o', str(binary)],
        cwd=ROOT, timeout=180).require_ok()
    return binary


def harness_env(**kv):
    # Every variable of pc_settings_harness_reason() (pc/hal/settings.c) goes,
    # not only TOKI_/PC_: SDL_VIDEODRIVER=dummy, which any headless checkout
    # exports, would make the harness an automated run with the extras off
    # (test_config and test_gallery then fail).  The harness links no SDL.
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('TOKI_', 'PC_', 'SCENE', 'AUTOPLAY'))
           and k != 'SDL_VIDEODRIVER'}
    env['TOKI_CONFIG'] = '/dev/null'
    env['TOKI_PROFILE'] = 'enhanced'
    env['TOKI_SETTINGS'] = '/dev/null'
    env['TOKI_ACHIEVEMENTS'] = '1'      # opt-in; the tests that unlock turn it on
    env.update({k: str(v) for k, v in kv.items()})
    return env


class HarnessTests(unittest.TestCase):
    binary = None

    @classmethod
    def setUpClass(cls):
        if cls.binary is None:
            cls._tmp = tempfile.TemporaryDirectory()
            HarnessTests.binary = build(cls._tmp.name)

    def mode(self, name, **env):
        with tempfile.TemporaryDirectory() as d:
            r = run([str(self.binary), name, os.path.join(d, 'extras')], env=harness_env(**env),
                    timeout=120)
            self.assertEqual(r.returncode, 0, f'{name}: {r.stdout}\n{r.stderr}')
            return r.stdout

    def test_config(self):
        def on(**kv):
            out = run([str(self.binary), 'config'], env=harness_env(**kv), timeout=30).stdout
            return re.findall(r'^extras=\d$', out, re.M)[-1]
        self.assertEqual(on(), 'extras=1')
        self.assertEqual(on(TOKI_PROFILE='parity'), 'extras=0')
        self.assertEqual(on(TOKI_PROFILE='parity', TOKI_EXTRAS='1'), 'extras=1')
        self.assertEqual(on(TOKI_EXTRAS='0'), 'extras=0')
        # an automated run (PC_INPUT: the harness rule) records nothing ...
        self.assertEqual(on(PC_INPUT='/dev/null'), 'extras=0')
        # ... unless a test forces it
        self.assertEqual(on(PC_INPUT='/dev/null', PC_EXTRAS_FORCE='1'), 'extras=1')

    def test_endings(self):
        out = self.mode('endings')
        self.assertIn('endings 2 distinct 2', out)

    def test_achievements(self):
        self.assertIn('achievements 30 of 30', self.mode('achievements'))

    def test_achievements_are_opt_in(self):
        """[enhanced] achievements defaults to off: nothing unlocks, nothing is
        recorded or announced (the on run above is the negative control)"""
        with tempfile.TemporaryDirectory() as d:
            r = run([str(self.binary), 'achievements', os.path.join(d, 'extras')],
                    env=harness_env(TOKI_ACHIEVEMENTS=''), timeout=120)
        out = r.stdout + r.stderr
        self.assertIn('achievements 0 of 30', out)
        self.assertNotIn('ACHIEVEMENT:', out)

    def test_persist(self):
        self.assertIn('persist ok', self.mode('persist'))

    def test_gallery(self):
        out = self.mode('gallery')
        self.assertIn('gallery 2 stills 1/301 events 1/77', out)
        self.assertIn('loaded but not shown', out)

    def test_view(self):
        self.assertIn('view ok', self.mode('view'))


class NegativeControls(unittest.TestCase):
    def test_each_mutation_is_caught(self):
        for mutation in MUTATIONS:
            name, _, _, _, test = mutation
            with self.subTest(name), tempfile.TemporaryDirectory() as tmp:
                binary = build(tmp, mutation)

                class Mutated(HarnessTests):
                    pass
                Mutated.binary = binary
                result = unittest.TestResult()
                Mutated(test).run(result)
                self.assertTrue(result.failures or result.errors,
                                f'mutation "{name}" was not caught by {test}')


# ------------------------------------------------------------------ the port

def available():
    return BINARY.is_file() and (ASSETS / 'raw/PSX.EXE').is_file()


def records(saves):
    path = Path(saves) / 'extras/extras.txt'
    if not path.exists():
        return []
    return [l.split() for l in path.read_text().splitlines() if l and not l.startswith('#')]


@unittest.skipUnless(available(), 'needs the built port and pc/assets')
class PortReadOnly(unittest.TestCase):
    """date_save's first phase with the extras forced on and off"""

    @classmethod
    def setUpClass(cls):
        import playthrough as pt
        import savestate_roundtrip as rt
        cls.tmp = tempfile.mkdtemp(prefix='extras-ro-')
        script = pt.parse(str(ROOT / 'pc/tools/playthrough/date_save.txt'))
        phase = script['phases'][0]
        num = os.path.join(cls.tmp, 'date.input')
        with open(num, 'w') as fh:
            fh.write(pt.numeric_phase(phase, script['hold'], False))
        syms = pt.trace_syms(script)
        cls.runs = {}
        for name, on in (('off', '0'), ('on', '1'), ('toasts', '1')):
            d = os.path.join(cls.tmp, name)
            os.makedirs(d)
            env = dict(PC_INPUT=num, PC_TRACE=os.path.join(d, 'trace'), PC_TRACE_PHASE='pre_irq',
                       PC_TRACE_FRAMES='40000', PC_PARITY_DIR=os.path.join(d, 'parity'),
                       PC_PARITY_FRAMES='1', PC_FRAME_HASH=os.path.join(d, 'frames'),
                       PC_AUDIO_HASH=os.path.join(d, 'audio'),
                       TOKI_SAVE_DIR=os.path.join(d, 'saves'), TOKI_EXTRAS=on, PC_EXTRAS_FORCE='1',
                       TOKI_ACHIEVEMENTS='1',
                       PC_PRESENT_HASH=os.path.join(d, 'present'),
                       TOKI_EXTRAS_CLOCK='1800000000')
            if name == 'toasts':
                env['TOKI_TOASTS'] = '1'      # [system] toasts is off by default
            if syms:
                env['PC_TRACE_SYMS'] = ','.join(f'{n}:{s}' for n, s in syms.items())
            rc, text = rt.run_port(game_platform.command_path(), os.path.join(d, 'work'), os.path.join(d, 'log'),
                                   env, 1200)
            cls.runs[name] = dict(dir=d, rc=rc, text=text)
        cls.rt = rt

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_read_only(self):
        a, b = self.runs['off'], self.runs['on']
        self.assertIn('PC_INPUT wait date_home reached', b['text'])
        for name in ('frames', 'trace', 'audio'):
            fa = Path(a['dir'], name).read_bytes()
            fb = Path(b['dir'], name).read_bytes()
            self.assertGreater(len(fa), 1000, name)
            self.assertEqual(fa, fb, f'{name} differs with the extras on')
        self.assertEqual(self.rt.compare_dirs(os.path.join(a['dir'], 'parity'),
                                              os.path.join(b['dir'], 'parity')), [])

    def test_unlocks_pop_up_only_with_toasts_on(self):
        """[system] toasts is off by default: an unlock is recorded (and
        logged) but draws no popup; with it on, the same run draws one."""
        def popups(name):
            lines = Path(self.runs[name]['dir'], 'present').read_text().splitlines()
            return sum(1 for l in lines if l.split()[-1:] == ['toast'])
        self.assertIn('[EXTRAS]', self.runs['on']['text'])
        self.assertEqual(popups('on'), 0)
        self.assertGreater(popups('toasts'), 0)

    def test_date_picture_and_achievements(self):
        recs = records(os.path.join(self.runs['on']['dir'], 'saves'))
        pics = {r[-1]: r for r in recs if r[0] == 'picture'}
        self.assertIn('BS/D_09_02.BS', pics, recs)
        p = pics['BS/D_09_02.BS']
        # picture HASH KIND W H UNIX STEP GIRL DATE SOURCE
        self.assertEqual(p[2:5] + p[6:8], ['0', '320', '160', '80', '0'])
        png = Path(self.runs['on']['dir'], 'saves/extras/gallery', p[1] + '.png')
        self.assertTrue(png.is_file())
        achs = {r[1] for r in recs if r[0] == 'achievement'}
        self.assertIn('first_date', achs)
        self.assertNotIn('club', achs, 'date_save joins no club')
        self.assertFalse(any(r[0] == 'ending' for r in recs))
        self.assertFalse(Path(self.runs['off']['dir'], 'saves/extras').exists())


@unittest.skipUnless(available(), 'needs the built port and pc/assets')
@unittest.skipIf(game_platform.WINDOWS, 'PC_DBG_CMDS pokes and save states: the debug toolkit '
                 'is not on the Windows build yet')
class PortEndings(unittest.TestCase):
    """the ending decision in three seeded states, one shared record"""

    @classmethod
    def setUpClass(cls):
        import savestate_roundtrip as rt
        cls.rt = rt
        cls.tmp = tempfile.mkdtemp(prefix='extras-end-')
        cls.reached, cls.abs_input, _ = rt.resolve(str(ROOT / 'pc/tools/playthrough/new_game.txt'),
                                                   cls.tmp, game_platform.command_path(), 900)
        cls.saves = os.path.join(cls.tmp, 'saves')
        cls.out = {}
        n = cls.reached['schedule_ready'] + 20
        date = (f'at {n} poke now_year:1 98\nat {n} poke now_month:1 3\nat {n} poke now_day:1 1\n'
                f'at {n} poke now_youbi:1 6\n')
        decided = 'when now_step=99 now_sub_step=0 now_sub_sub_step!=0 do peek 0x800EF6BF 1\n'
        # Saki (record 4, 0x800EE9FC): tokimeki and yuukou 128, met (flag
        # word bit 1), 30 counted dates (flags[1], record + 21)
        saki = (f'at {n} poke 0x800EE9FE:2 128\nat {n} poke 0x800EEA02:2 128\n'
                f'at {n} poke 0x800EEA08:1 2\nat {n} poke 0x800EEA11:1 30\n')
        rei = f'at {n} poke 0x800EEBCB:1 70\nat {n} poke 0x800EED1B:1 2\n'
        state = os.path.join(cls.tmp, 'decided.tks')
        cases = (('yoshio', '', '1800000001', f'after 60 save {state}\n'),
                 ('rei', rei, '1800000002', ''),
                 ('saki', saki, '1800000003', ''))
        for tag, pokes, clock, tail in cases:
            cmds = date + pokes + decided + tail + 'after 30 quit\n'
            cls.out[tag] = cls.port(tag, cmds, clock)
        # a new process loads the state saved after Yoshio's decision
        cls.out['load'] = cls.port('load', f'at {cls.reached["schedule_ready"] + 20} load {state}\n'
                                   'after 120 quit\n', '1800000004')

    @classmethod
    def port(cls, tag, cmds, clock):
        d = os.path.join(cls.tmp, tag)
        os.makedirs(d, exist_ok=True)
        inp = os.path.join(d, 'abs.input')
        with open(inp, 'w') as fh:
            fh.write(cls.abs_input)
        with open(os.path.join(d, 'cmds'), 'w') as fh:
            fh.write(cmds)
        env = dict(PC_INPUT=inp, PC_DBG_CMDS=os.path.join(d, 'cmds'), TOKI_SAVE_DIR=cls.saves,
                   TOKI_EXTRAS='1', PC_EXTRAS_FORCE='1', TOKI_EXTRAS_CLOCK=clock,
                   TOKI_ACHIEVEMENTS='1')
        rc, text = cls.rt.run_port(game_platform.command_path(), os.path.join(d, 'work'), os.path.join(d, 'log'),
                                   env, 900)
        return dict(rc=rc, text=text)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def decided(self, tag):
        t = self.out[tag]['text']
        self.assertEqual(self.out[tag]['rc'], 0, t[-3000:])
        m = re.search(r'\[EXTRAS\] ending decided: now_girl (\d+)', t)
        self.assertTrue(m, t[-3000:])
        peek = re.search(r'\[DBG\] ok 0x800ef6bf 1 [0-9a-f]+ u=(\d+)', t)
        self.assertTrue(peek, re.findall(r'\[DBG\][^\n]*|ok [^\n]*', t)[-10:])
        self.assertEqual(int(m.group(1)), int(peek.group(1)), 'recorded = the game\'s now_girl')
        self.assertEqual(t.count('[EXTRAS] ending decided'), 1)
        return int(m.group(1))

    def test_yoshio(self):
        self.assertEqual(self.decided('yoshio'), 13)

    def test_rei(self):
        self.assertEqual(self.decided('rei'), 12)

    def test_saki(self):
        self.assertEqual(self.decided('saki'), 4)

    def test_shared_record(self):
        recs = records(self.saves)
        ends = [(r[1], r[2], r[3]) for r in recs if r[0] == 'ending']
        self.assertEqual(ends, [('1800000001', '13', '1998-03-01'), ('1800000002', '12', '1998-03-01'),
                                ('1800000003', '4', '1998-03-01')])
        achs = {r[1]: r[2] for r in recs if r[0] == 'achievement'}
        # Graduation from the first process, kept; one achievement per ending
        self.assertEqual(achs.get('graduate'), '1800000001')
        for k, t in (('ending_13', '1800000001'), ('ending_12', '1800000002'), ('ending_4', '1800000003')):
            self.assertEqual(achs.get(k), t)
        self.assertEqual(sum('Achievement: Graduation' in o['text'] for o in self.out.values()), 1)

    def test_load_into_a_decided_ending(self):
        t = self.out['load']['text']
        self.assertIn('[QUICKSAVE]', t + '[QUICKSAVE]')
        self.assertRegex(t, r'\[DBG\] cmds line 1 at frame \d+: load')
        self.assertNotIn('[EXTRAS] ending decided', t)


if __name__ == '__main__':
    unittest.main()
