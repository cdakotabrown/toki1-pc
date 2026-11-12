#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests of the game-mechanics guide (pc/hal/guide_view.c, guide_model.c,
guide_rules.c, guide_text.c).

RulesTests (pc/tests/guide_harness.c, needs pc/assets/raw/PSX.EXE for the
game's tables): the guide's rules against the GAME'S OWN functions, compiled
from src/main/matched and linked at the console addresses, on the same RAM:
  * parameter_up_down: every activity and club, holiday, poor day and
    illness state, all nine parameters;
  * biorhythm_sf_set: the poor-day chance, counted over all 32768 values of
    rand(), on random states and seven day offsets;
  * touzyou_check / _check2 / touzyou_basyo_check: which girl's first
    meeting is scheduled (and its variant), and the dice;
  * byouki_hassei: the next illness state over the day's draws, the visit;
  * girl_counter_periodic_decrement, syoushin_up, get_g_zyotai_h/_s,
    get_h_tokimeki: decay, neglect, moods, the tokimeki ladder;
  * date_sasoi_judge / geko_judge: who asks you out or walks home, the roll;
  * tel_dialog20 (TEL, the overlay file mapped at 0x80132000 as the CD
    delivers it): her yes or no over all 256 draws for every girl and
    place, and the bomb and stress the call applies;
  * date_score_apply / date_rank_grade_get (DATE): every girl, place and
    answer grade.
Negative controls: source mutations of the guide's rules (a number or a
condition one step off) each produce mismatches.

PageTests: every page as text from a game state and from no game; the
numbers printed are the rules' (the activity table equals the game's
parameter_up_down for a school day); the page drawn into a recording
canvas stays inside it; TOKI_GUIDE=0 removes it.

InGame (the built port, date_save.txt up to Shiori's phone call): the
guide opened from the settings menu on the first day and on Sunday 9 Apr,
every tab, a girl, scrolled, changes nothing the game sees (per-frame VRAM /
scanout / RAM hashes and the game's audio equal the run where it stays
closed, in the enhanced and the parity profile); its window shots are kept
with TEST_GUIDE_SHOTS=DIR.
"""
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from safe_process import run  # noqa: E402
import game_platform  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
from pc_paths import decomp  # noqa: E402
BINARY = game_platform.GAME
ASSETS = ROOT / 'pc/assets'
EXE = ASSETS / 'raw/PSX.EXE'
TEL = ASSETS / 'raw/CDROM/EXEDIR/TEL.EXN'
DATE = ASSETS / 'raw/CDROM/EXEDIR/DATE.EXN'
RULES = 'pc/hal/guide_rules.c'
MODEL = 'pc/hal/guide_model.c'
DATES = 'pc/hal/guide_rules_dates.c'
COMMON = ['gcc', '-std=gnu17', '-fpermissive', '-w', '-funsigned-char', '-fno-strict-aliasing',
          '-O2', '-D_GNU_SOURCE', '-DPC_PORT', '-include', 'pc/include/game/types.h',
          '-I', 'pc/include/game', '-I', 'pc/include', '-I', decomp('include'), '-I', 'pc/hal']
GAME_FUNCS = ['parameter_up_down', 'parameter_change', 'biorhythm_sf_set', 'biorhythm_sf2',
              'get_pass_days', 'rsin', 'sin_1', 'touzyou_check', 'touzyou_check2',
              'touzyou_basyo_check', 'byouki_hassei', 'girl_counter_periodic_decrement',
              'syoushin_up', 'get_g_zyotai_h', 'get_g_zyotai_s', 'get_h_yuukou',
              'get_h_yuukou_table', 'get_h_tokimeki', 'get_h_tokimeki_table',
              'date_sasoi_judge', 'geko_judge', 'get_grp', 'get_grp_point', 'grp_table_calc',
              'param_clamp']
OVERLAY_FUNCS = [decomp(f) for f in (
                 'src/overlay/TEL/tel_dialog20.c', 'src/overlay/DATE/date_score_apply.c',
                 'src/overlay/DATE/date_rank_grade_get.c', 'src/overlay/ETC/score_grade_calc.c',
                 'src/overlay/ETC/get_etc_hosei.c')]

# (name, file, old, new, the check that must fail)
MUTATIONS = [
    ('holiday x3', RULES, '((holiday ? 4 : 1) << 12)', '((holiday ? 3 : 1) << 12)', 'act'),
    ('losses scaled', RULES, 'return v > 0 ? v * mul : v * 4096;', 'return v * mul;', 'act'),
    ('no poor-day stress', RULES, 'return v * 4096 + (poor ? 2 : 1) * 65536 - 65536;',
     'return v * 4096;', 'act'),
    ('looks halved when ill', RULES, 'act != 5 && act != 7 && act != 6', 'act != 5 && act != 7',
     'act'),
    ('stress cut at 96', RULES, 'para_str < 97 ? para_str / 32 : 4', 'para_str < 96 ? para_str / 32 : 4',
     'poor'),
    ('no quarter', RULES, 'if (probes == 3) x /= 4;', '', 'poor'),
    ('mood curve for art', RULES, 'case 2: t0 = sf2(c, 0, 1, 0, 16)', 'case 2: t0 = sf2(c, 0, 0, 1, 16)',
     'poor'),
    ('Mio 56', RULES, '{ 1, 0, 1, 55, 75,', '{ 1, 0, 1, 56, 75,', 'debut'),
    ('Mira 125', RULES, '{ 7, 6, 6, 101, 126,', '{ 7, 6, 6, 101, 125,', 'debut'),
    ('Saki any club', RULES, 'if (r->not_home < 0 && home(r->girl) == (basyo_pack >> 12)) return 0;',
     '', 'debut'),
    ('club route 20 sessions', RULES, 'sessions >= 21) {', 'sessions >= 20) {', 'debut'),
    ('die 1 in 4', RULES, 'return mode == 1 ? 3 : 8;', 'return mode == 1 ? 3 : 6;', 'debut'),
    ('ill limit 205', RULES, 'para_kon / 8 + 204', 'para_kon / 8 + 205', 'ill'),
    ('cold recovers at 30', RULES, 'return para_tai > 30 ? 0 : n;', 'return para_tai >= 30 ? 0 : n;',
     'ill'),
    ('Shiori decays once', RULES, 'if (day / 7 == 2 && girl == 0) return 1;', '', 'girls'),
    ('neglect at 11', RULES, 'gsv_yuukou_pct(girl) < 10 ? 5 : 0', 'gsv_yuukou_pct(girl) < 11 ? 5 : 0',
     'girls'),
    ('mood cool at 40', RULES, 'if ((unsigned)y < 50) return 3;', 'if ((unsigned)y < 40) return 3;',
     'girls'),
    ('ladder mode 4', MODEL, '{ 10, 10, 10, 10, 0, 0, 0 },', '{ 10, 10, 10, 0, 0, 0, 0 },', 'girls'),
    ('looks penalty from 41', DATES, 'if (para_you < 40) a += para_you - 40;',
     'if (para_you < 41) a += para_you - 41;', 'tel'),
    ('liking 0 accepted', DATES, 'a = like ? a + like : 0;', 'a = a + like;', 'tel'),
    ('icon / 2', DATES, 'int a = icon >> 2,', 'int a = icon >> 1,', 'tel'),
    ('loved place x4', DATES, '*B = cell == 0 ? 5 :', '*B = cell == 0 ? 4 :', 'date'),
    ('bad answers not inverted', DATES, '*A = mode > 0 ? 5 - i : i + 3;', '*A = 5 - i;', 'date'),
    # (icons come in steps of 5 with a few 4s and 9s: 15 is the value just
    # under 17 that exists)
    ('walk home at icon 15', DATES, 'return icon >= (kind ? 17 : 33);',
     'return icon >= (kind ? 15 : 33);', 'invite'),
    ('invitation 10 %', DATES, 'return kind ? 50 : 8;', 'return kind ? 50 : 10;', 'invite'),
    ('no 17 Dec gate', DATES, '(now_day == 17 || now_day == 24)', '(now_day == 24)', 'invite'),
    ('exam halved by injury', RULES, 'if (byouki_kind & 2) v /= 2;', 'if (byouki_kind & 3) v /= 2;',
     'exam'),
    ('exam second subject cap 39', RULES, 'if (sub >= 39) sub = 38;\n        bio',
     'if (sub >= 40) sub = 39;\n        bio', 'exam'),
]

CHECKS = {'act': ['act'], 'poor': ['poor', '300', '11'], 'debut': ['debut', '4000', '12'],
          'ill': ['ill', '120', '13'], 'girls': ['girls', '6000', '14'],
          'invite': ['invite', '6000', '21'], 'tel': ['tel', '6', '22'],
          'date': ['date', '40', '23'], 'exam': ['exam', '30', '24'], 'cal': ['cal']}


def build_harness(folder, mutation=None):
    srcs = {RULES: ROOT / RULES, MODEL: ROOT / MODEL, DATES: ROOT / DATES}
    if mutation:
        f = mutation[1]
        text = (ROOT / f).read_text()
        assert mutation[2] in text, mutation[0]
        srcs[f] = Path(folder) / Path(f).name
        srcs[f].write_text(text.replace(mutation[2], mutation[3]))
    out = Path(folder) / 'guide_harness'
    font = Path(folder) / 'ui_font_data.c'
    subprocess.run([sys.executable, 'pc/tools/gen_ui_font.py', '--out', str(font)], cwd=ROOT,
                   check=True, capture_output=True, text=True)
    cmd = COMMON + ['-no-pie', 'pc/tests/guide_harness.c', str(srcs[RULES]), str(srcs[MODEL]),
           str(srcs[DATES]), 'pc/hal/guide_text.c', 'pc/hal/guide_view.c', 'pc/hal/game_state_view.c',
           'pc/hal/menu.c', 'pc/hal/settings.c', 'pc/hal/ui_font.c', str(font)]
    cmd += [decomp(f'src/main/matched/{f}.c') for f in GAME_FUNCS]
    # the overlays' functions: their own objects (printf quiet: they print
    # debug lines from format strings in the overlay)
    for f in OVERLAY_FUNCS:
        obj = Path(folder) / (Path(f).stem + '.o')
        r = subprocess.run(COMMON + ['-Dprintf=guide_quiet_printf', '-c', f, '-o', str(obj)],
                           cwd=ROOT, capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError(r.stderr[-4000:])
        cmd.append(str(obj))
    # tel_dialog20 names the month byte through a volatile alias
    cmd += ['-Wl,--defsym=now_month_v=0x800EE79F']
    cmd += ['-Wl,--just-symbols=pc/ld/psx_globals.syms', '-o', str(out)]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        # the rest of what those functions call (other steps of the same
        # state machines, never reached by the checks) as empty stubs
        names = sorted(set(re.findall(r"undefined reference to `([A-Za-z_][A-Za-z0-9_]*)'",
                                      r.stderr)))
        if not names:
            raise RuntimeError(r.stderr[-4000:])
        stubs = Path(folder) / 'stubs.c'
        stubs.write_text(''.join(f'int {n}() {{ return 0; }}\n' for n in names))
        r = subprocess.run(cmd[:-4] + [str(stubs)] + cmd[-4:], cwd=ROOT, capture_output=True,
                           text=True)
        if r.returncode:
            raise RuntimeError(r.stderr[-4000:])
    return out


def harness(binary, *args, env=None, exe=True):
    e = {k: v for k, v in os.environ.items() if not k.startswith(('TOKI_', 'PC_'))}
    e['TOKI_SETTINGS'] = 'off'
    e.update(env or {})
    r = subprocess.run([str(binary), *args] + ([str(EXE)] if exe else []), cwd=ROOT,
                       capture_output=True, text=True, timeout=900, env=e)
    return r.returncode, r.stdout


@unittest.skipUnless(EXE.is_file(), 'needs pc/assets/raw/PSX.EXE (the game tables)')
class RulesTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.bin = build_harness(cls.temp.name)

    def check(self, name, binary=None):
        return harness(binary or self.bin, *CHECKS[name])

    def test_activities_equal_parameter_up_down(self):
        rc, out = self.check('act')
        self.assertEqual(rc, 0, out)
        self.assertIn('act mismatches 0 of 3420', out)

    def test_poor_days_equal_biorhythm_sf_set(self):
        rc, out = self.check('poor')
        self.assertEqual(rc, 0, out)
        self.assertIn('poor mismatches 0 of 2100', out)
        lo, hi = map(int, re.search(r'poor counts from (\d+) to (\d+)', out).groups())
        self.assertLess(lo, 8000)       # the sample has easy days ...
        self.assertGreater(hi, 20000)   # ... and hard ones

    def test_first_meetings_equal_touzyou_checks(self):
        rc, out = self.check('debut')
        self.assertEqual(rc, 0, out)
        self.assertIn('debut mismatches 0 of', out)
        picks = list(map(int, re.search(r'debut picks:((?: \d+)+)', out).group(1).split()))
        self.assertTrue(all(p > 0 for p in picks[:8]), picks)   # girls 1..8 each reached

    def test_illness_equals_byouki_hassei(self):
        rc, out = self.check('ill')
        self.assertEqual(rc, 0, out)
        self.assertIn('ill mismatches 0 of', out)
        seen = list(map(int, re.search(r'ill states seen:((?: \d+)+)', out).group(1).split()))
        self.assertTrue(all(s > 0 for s in seen), seen)

    def test_girls_rules_equal_the_games(self):
        rc, out = self.check('girls')
        self.assertEqual(rc, 0, out)
        self.assertIn('girls mismatches 0 of', out)

    def test_invitations_equal_date_sasoi_judge_and_geko_judge(self):
        rc, out = self.check('invite')
        self.assertEqual(rc, 0, out)
        self.assertIn('invite mismatches 0 of', out)
        inv, walk = map(int, re.search(r'invitations picked (\d+), walks (\d+)', out).groups())
        self.assertGreater(inv, 100)
        self.assertGreater(walk, inv)

    @unittest.skipUnless(TEL.is_file(), 'needs the TEL overlay')
    def test_phone_answers_equal_tel_dialog20(self):
        rc, out = self.check('tel')
        self.assertEqual(rc, 0, out)
        self.assertIn('tel mismatches 0 of', out)

    @unittest.skipUnless(DATE.is_file(), 'needs the DATE overlay')
    def test_date_answers_equal_date_score_apply(self):
        rc, out = self.check('date')
        self.assertEqual(rc, 0, out)
        self.assertIn('date mismatches 0 of 138600', out)

    @unittest.skipUnless((ASSETS / 'raw/CDROM/EXEDIR/ETC.EXN').is_file(), 'needs ETC')
    def test_exams_equal_score_grade_calc(self):
        rc, out = self.check('exam')
        self.assertEqual(rc, 0, out)
        self.assertIn('exam mismatches 0 of', out)
        self.assertGreater(int(re.search(r'exam scores of 95\+: (\d+)', out).group(1)), 100)

    def test_calendar_reading(self):
        # an independent parse of CALROM.NBN (the layout of cal_font_load:
        # 36 month pointers from 0x801A0000, each month's s32 trailer after
        # its 8192-byte picture) against the guide's reading, every month
        cal = ASSETS / 'raw/CDROM/FUKUTEST/CALROM.NBN'
        if not cal.is_file():
            self.skipTest('needs CALROM.NBN')
        d = cal.read_bytes()
        rc, out = harness(self.bin, 'cal')
        self.assertEqual(rc, 0, out)
        rows = [l for l in out.splitlines() if l.startswith('month ')]
        self.assertEqual(len(rows), 36)
        ptrs = struct.unpack_from('<36I', d, 0)
        for k, row in enumerate(rows):
            o = ptrs[k] - 0x801A0000 + 8192
            n, first = struct.unpack_from('<2i', d, o)
            kind = struct.unpack_from(f'<{n}i', d, o + 8)
            ev = struct.unpack_from(f'<{n}i', d, o + 8 + 4 * n)
            want = (f'month {k} n {n} first {first} kind {"".join(map(str, kind))} ev '
                    + ' '.join(map(str, ev)))
            self.assertEqual(row, want)
        # the game's own April 1995 (game_work_init1 hard-codes it): the
        # 4th is the opening event, the 29th a national holiday
        self.assertTrue(rows[0].startswith('month 0 n 30 first 5 kind 0100000010000001000000100000'))
        self.assertIn(' ev 0 0 0 1 0', rows[0])

    def test_negative_controls(self):
        for m in MUTATIONS:
            with self.subTest(m[0]), tempfile.TemporaryDirectory() as d:
                b = build_harness(d, m)
                rc, out = self.check(m[4], b)
                self.assertNotEqual(rc, 0, f'{m[0]}: {out[-800:]}')
                self.assertNotIn(' mismatches 0 of', out)


def pages(text):
    """the harness's text dump: {title: body}"""
    out, cur = {}, None
    for line in text.splitlines():
        if line.startswith('=== '):
            cur = line[4:]
            out[cur] = []
        elif cur is not None:
            out[cur].append(line)
    return {k: '\n'.join(v) for k, v in out.items()}


@unittest.skipUnless(EXE.is_file(), 'needs pc/assets/raw/PSX.EXE (the game tables)')
class PageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.bin = build_harness(cls.temp.name)
        rc, cls.out = harness(cls.bin, 'text', '-')
        assert rc == 0, cls.out[-2000:]
        cls.pages = pages(cls.out)

    def test_every_page_is_there(self):
        titles = list(self.pages)
        self.assertEqual(titles[0], 'Activities')
        self.assertEqual(len([t for t in titles if t.startswith('Girls ')]), 13)
        self.assertIn('Events', titles)
        self.assertIn('Rules', titles)
        for t, body in self.pages.items():
            self.assertGreater(len(body.splitlines()), 5, t)
            self.assertIn('Source:', body, t)

    def test_activity_table_is_the_games(self):
        # the table row for Humanities: parameter_up_down_tbl0 row 0 / 16, the
        # same numbers the act check proved equal to the game's function
        table = [l.split(' | ') for l in self.pages['Activities'].splitlines() if ' | ' in l]
        rows = {r[0]: r[1:] for r in table[:8]}           # the header and the activities
        self.assertEqual(rows['Humanities'], ['-0.8', '0.9', '0.1', '0.1', '-0.4', '0.3',
                                              '-0.3', '-0.1', '0.8'])
        self.assertEqual(rows['Sports'][4], '3.3')
        self.assertEqual(rows['Rest'][0], '3.1')
        self.assertEqual(rows['Rest'][8], '-2.9')
        self.assertEqual(len(table), 1 + 7 + 11)    # header, activities, clubs
        self.assertEqual(table[8][0], 'Literature')
        self.assertEqual(table[8][1:], ['-0.8', '0.8', '0.3', '0.1', '0', '0.1', '-0.3',
                                        '-0.1', '1.1'])

    @unittest.skipUnless(TEL.is_file(), 'needs the TEL overlay')
    def test_overlay_tables_are_read(self):
        # tel_ring_timeout's penalties for Yuina (27 / 26) and the stood-up
        # cost (56), read from the player's TEL.EXN at run time
        g = ' '.join(self.pages[[t for t in self.pages if t.startswith('Girls 2 ')][0]].split())
        self.assertIn("Yuina's is tokimeki -27, friendship -26", g)
        self.assertIn('bomb +56 and friendship -56', g)

    def test_no_game_has_no_live_notes(self):
        self.assertNotIn('Now:', self.out)
        self.assertNotIn('You qualify now', self.out)

    def test_page_draws_inside_the_canvas(self):
        rc, out = harness(self.bin, 'page', '-')
        self.assertEqual(rc, 0, out[-2000:])
        self.assertNotIn('OUTSIDE', out)
        self.assertEqual(out.count('TAB '), 4)
        self.assertIn('BACK list', out)
        # TOKI_GUIDE=0: no page (negative control)
        rc, out = harness(self.bin, 'page', '-', env={'TOKI_GUIDE': '0'})
        self.assertEqual((rc, out.strip().splitlines()[-1]), (3, 'no page'))


# ---------------------------------------------------------------- the game

def call_script(path):
    """date_save.txt up to Shiori's phone call (the calendar)"""
    lines = []
    for line in (ROOT / 'pc/tools/playthrough/date_save.txt').read_text().splitlines():
        lines.append(line)
        if line.startswith('wait calendar '):
            lines.append('end 60')
            break
    Path(path).write_text('\n'.join(lines) + '\n')


def lines(path):
    return Path(path).read_text().splitlines() if Path(path).exists() else []


def present(path):
    return [(p[1], p[3] if len(p) > 3 else '') for p in (l.split() for l in lines(path))]


# Ctrl+O, Up Up Up: the main page's last entry is the setup screen's GAME DATA
# AND ABOUT (pc_setup_menu_register runs after the presenter's), the one
# before it the controls legend's (pc/hal/controls_view.c), then the
# guide's; Return opens it; then every tab, a girl,
# scrolling, the list again
OPEN = 'Ctrl+O Up Up Up Return'
TOUR = 'Down Down Return {P}Right Right Left Right Down Down Down {P}Escape {P}Escape Escape'


def keys(first, sunday, shots=False):
    """the first day: every tab, the girls list moved; Sunday: it reopens
    on its last tab (Rules) and remembers the list (girl 2 + 2 = 4).
    SHOTS: Ctrl+P window shots between the steps (a separate run: a shot
    can swallow the key after it, which would leave the menu open)"""
    P = 'Ctrl+P ' if shots else ''
    return (f'{first} key {OPEN} {P}Down Down {P}Right Down Down {P}Right {P}'
            f'Down Right {P}Down Escape Escape\n'
            f'{sunday} key {OPEN} Right Right {TOUR.format(P=P)}\n')


@unittest.skipUnless(BINARY.is_file() and EXE.is_file(), 'needs the built port and pc/assets')
class InGame(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.dir = Path(cls.temp.name)
        cls.script = cls.dir / 'call.txt'
        call_script(cls.script)
        cls.hbin = build_harness(cls.dir)
        cls.closed = cls.play('closed', {})
        cls.used = cls.play('used', {}, keys(3590, 4100))
        cls.parity_closed = cls.play('parity_closed', {}, profile='parity')
        # parity reaches the same screens later (the script's clock: the first
        # lesson at 5268, Sunday's planner done at 5955, the phone book 6155)
        cls.parity_used = cls.play('parity_used', {}, keys(5300, 6000), profile='parity')
        cls.off = cls.play('off', {'TOKI_GUIDE': '0'}, keys(3590, 4100))
        if os.environ.get('TEST_GUIDE_SHOTS'):
            cls.play('shots', {}, keys(3590, 4100, shots=True))

    @classmethod
    def play(cls, name, settings, keys=None, profile='enhanced'):
        out = cls.dir / name
        out.mkdir()
        files = dict(frames=out / 'frames.txt', audio=out / 'audio.txt',
                     present=out / 'present.txt', log=out / 'guide.txt', cfg=out / 'cfg')
        files['cfg'].mkdir()
        (out / 'saves').mkdir()
        sets = dict(PC_FRAME_HASH=files['frames'], PC_FRAME_HASH_RAM='game',
                    PC_AUDIO_HASH=files['audio'], PC_PRESENT_HASH=files['present'],
                    TOKI_GUIDE_LOG=files['log'], TOKI_CONFIG_DIR=files['cfg'],
                    TOKI_SAVE_DIR=out / 'saves', **settings)
        if keys:
            (out / 'keys.txt').write_text(keys)
            sets['TOKI_INPUT_SCRIPT'] = out / 'keys.txt'
            sets['PC_PRESENT_WINDOW_SHOT'] = out / f'{name}-%d.png'   # Ctrl+P, one file each
        cmd = ['.venv/bin/python3' if (ROOT / '.venv/bin/python3').exists() else 'python3',
               'pc/tools/playthrough.py', str(cls.script), '--out', str(out / 'run'),
               '--bin', str(BINARY), '--profile', profile]
        for k, v in sets.items():
            cmd += ['--set', f'{k}={v}']
        r = run(cmd, cwd=ROOT, timeout=1800, max_output=16 * 1024**2)
        log = out / 'run/date.pc.log'
        files['out'] = log.read_text(errors='replace') if log.exists() else ''
        files['result'] = r
        shot = os.environ.get('TEST_GUIDE_SHOTS')
        if shot:
            Path(shot).mkdir(parents=True, exist_ok=True)
            for p in out.glob(f'{name}*.png'):
                shutil.copy(p, Path(shot) / p.name)
        return files

    def test_runs_pass(self):
        for name in ('closed', 'used', 'parity_closed', 'parity_used', 'off'):
            r = getattr(self, name)['result']
            self.assertTrue(r.ok, f'{name}: {r.stdout[-1500:]}')

    def test_the_guide_opened_and_showed_every_tab(self):
        out = self.used['out']
        self.assertEqual(out.count('[GUIDE] open (in game)'), 2, out[-3000:])
        log = Path(self.used['log']).read_text()
        for tab in ('Activities', 'Girls', 'Events', 'Rules'):
            self.assertIn(f'--- page {tab} ', log)
        self.assertRegex(log, r'--- page Girls girl 4 in_game 1')
        # live notes: the plan of the first week and Shiori's numbers
        self.assertIn('Yellow: your plan this week', log)
        self.assertIn('Now: friendship', log)

    def test_using_the_guide_changes_nothing_the_game_sees(self):
        for used, closed in ((self.used, self.closed), (self.parity_used, self.parity_closed)):
            self.assertGreater(len(lines(closed['frames'])), 1000)
            self.assertEqual(lines(used['frames']), lines(closed['frames']))
            self.assertEqual(lines(used['audio']), lines(closed['audio']))
            frames = present(used['present'])
            self.assertGreaterEqual(len([h for h, tag in frames if tag == 'menu']), 10)
            game = [(h, tag) for h, tag in frames if tag != 'menu']
            ref = present(closed['present'])
            self.assertEqual(len(game), len(ref))
            differ = [i for i, ((h, tag), (r, _)) in enumerate(zip(game, ref)) if h != r]
            self.assertTrue(all(game[i][1] == 'toast' for i in differ), differ[:20])

    def test_pages_are_the_rams(self):
        # each opening's RAM, offline: the calendar the game loaded equals the
        # guide's reading of CALROM.NBN, and the page text built from that
        # RAM by the harness equals what the game session showed
        rams = sorted(Path(str(self.used['log']) + '.0.ram').parent.glob('guide.txt.*.ram'))
        self.assertEqual(len(rams), 2)
        log = Path(self.used['log']).read_text()
        blocks = {}
        for b in log.split('--- page ')[1:]:
            head, _, body = b.partition('\n')
            blocks[head.split(' girl ')[0] + ' ' + head.split(' girl ')[1].split()[0]] = body
        for ram in rams:
            rc, out = harness(self.hbin, 'calram', str(ram))
            self.assertEqual(rc, 0, out)
            self.assertIn('calram mismatches 0 of', out)
        rc, out = harness(self.hbin, 'playertext', str(rams[-1]))
        self.assertEqual(rc, 0, out[-2000:])
        offline = pages(out)
        self.assertNotIn('Source:', out)      # players see no function names
        self.assertEqual(blocks['Activities -1'].strip(), offline['Activities'].strip())
        self.assertEqual(blocks['Rules -1'].strip(), offline['Rules'].strip())
        girl4 = [k for k in offline if k.startswith('Girls 4 ')][0]
        self.assertEqual(blocks['Girls 4'].strip(), offline[girl4].strip())

    def test_parity_profile_has_the_guide_too(self):
        self.assertEqual(self.parity_used['out'].count('[GUIDE] open (in game)'), 2)

    def test_negative_control_switched_off(self):
        # TOKI_GUIDE=0: no page; the same keys now reach other menu entries
        self.assertNotIn('[GUIDE] open', self.off['out'])
        self.assertEqual(lines(self.off['log']), [])


if __name__ == '__main__':
    unittest.main()
