#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests of the enhanced status page (pc/hal/status_view.c) and its decode
of the game's state (pc/hal/game_state_view.c).

DecodeTests (pc/tests/game_state_view_harness.c, needs pc/assets/raw/PSX.EXE
for the game's tables): the decode against the GAME'S OWN functions --
get_grp, get_grp_point, grp_table_calc, get_h_yuukou, get_h_tokimeki,
compiled from src/main/matched and linked at the console addresses -- over
random states (every girl's friendship% / tokimeki%, icon and ending
score).  Negative controls: eight source mutations of the decode (a rule
one step off) each produce mismatches.

InGame (the built port, date_save.txt up to Shiori's phone call):
  * the page opened on the first day, on Sunday 9 Apr and on the phone
    call, with every tab shown, changes nothing the game sees: per-frame
    VRAM / scanout / 2 MB RAM hashes (PC_FRAME_HASH, PC_FRAME_HASH_RAM) and
    the game's mixed audio (PC_AUDIO_HASH) equal the run where it stays
    closed, and that run equals one with the feature off (TOKI_STATUS=0);
    the presenter's own windows (tagged "menu") are the only additions;
  * the HUD line (TOKI_STATUS_HUD=1) changes no frame hash, only the
    presented windows it is drawn on (tagged "hud");
  * values: the first day's parameters are parameter_init's, Shiori's
    record is one of name_ent_girl_para_set's four starts, the other girls
    girl_rec_init's; Sunday 9 Apr is the date the playthrough itself checks;
    each RAM image written at an opening decodes offline to the same text
    and the game's functions agree on it;
  * a seeded run (poked girl records) shows the poked values and the
    game's thresholds: bomb 59 not lit, 60 lit, the rumour latch, neglect
    under 10 % friendship, the highest bomb in the HUD;
  * the page (status_view.c) drawn by the harness from the seeded RAM into
    a recording canvas shows those values, lit in the warning colour
    exactly at 60+; with TOKI_STATUS unset (the parity default) there is
    no page (negative control);
  * parity: the key does nothing (no page, no reads), the hashes equal the
    parity run without it.
  * negative control: a PC-side assist that does press for the player
    (TOKI_TEXT_AUTO) changes the frame hashes.
TEST_STATUS_SHOTS=DIR copies the window shots there.
"""
import os
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
BINARY = game_platform.GAME   # PC_TEST_PLATFORM=windows: the .exe under Wine
ASSETS = ROOT / 'pc/assets'
EXE = ASSETS / 'raw/PSX.EXE'
VIEW = 'pc/hal/game_state_view.c'
GAME_FUNCS = ['get_grp', 'get_grp_point', 'grp_table_calc', 'get_h_yuukou', 'get_h_yuukou_table',
              'get_h_tokimeki', 'get_h_tokimeki_table']

MUTATIONS = [
    ('bomb weight sign', 'S16(REC(k) + 10) * grp_table_calc_tbl0[k]',
     'S16(REC(k) + 10) * -grp_table_calc_tbl0[k]'),
    ('dates threshold 3', 'if (sum >= 4) buf[i]', 'if (sum >= 3) buf[i]'),
    ("Shiori's bomb limit 52", 'S16(REC(0) + 10) < 51', 'S16(REC(0) + 10) < 52'),
    ('ladder <=', '(u32)(thr[k] + add[mode][k]) < stat', '(u32)(thr[k] + add[mode][k]) <= stat'),
    ('friendship clamp', 'if (r >= 101) return 100;', 'if (r > 101) return 100;'),
    ('mode 5 limit', '{ 20, 20, 20, 20, 0, -5, 0 }', '{ 20, 20, 20, 20, 0, 0, 0 }'),
    ("stress bonus on Ayako", 'buf[2] = (s16)(buf[2] + grp_table_calc_tbl4',
     'buf[3] = (s16)(buf[3] + grp_table_calc_tbl4'),
    ('liked-parameter order', '{ 7, 6, 5, 4, 3, 2, 1 }', '{ 1, 2, 3, 4, 5, 6, 7 }'),
]


def build_harness(folder, mutation=None):
    src = ROOT / VIEW
    if mutation:
        text = src.read_text()
        assert mutation[1] in text, mutation[0]
        src = Path(folder) / 'game_state_view.c'
        src.write_text(text.replace(mutation[1], mutation[2]))
    out = Path(folder) / 'gsv_harness'
    # the page's UI font (pc/include/pc_ui.h), generated as the build does
    font = Path(folder) / 'ui_font_data.c'
    subprocess.run([sys.executable, 'pc/tools/gen_ui_font.py', '--out', str(font)], cwd=ROOT,
                   check=True, capture_output=True, text=True)
    cmd = ['gcc', '-std=gnu17', '-w', '-funsigned-char', '-fno-strict-aliasing', '-O2',
           '-D_GNU_SOURCE', '-DPC_PORT', '-include', 'pc/include/game/types.h',
           '-I', 'pc/include/game', '-I', 'pc/include', '-I', decomp('include'), '-I', 'pc/hal',
           '-no-pie', 'pc/tests/game_state_view_harness.c', str(src), 'pc/hal/status_view.c',
           'pc/hal/menu.c', 'pc/hal/settings.c', 'pc/hal/ui_font.c', str(font)]
    cmd += [decomp(f'src/main/matched/{f}.c') for f in GAME_FUNCS]
    cmd += ['-Wl,--just-symbols=pc/ld/psx_globals.syms', '-o', str(out)]
    subprocess.run(cmd, cwd=ROOT, check=True, capture_output=True, text=True)
    return out


def harness(binary, *args, env=None):
    e = {k: v for k, v in os.environ.items() if not k.startswith(('TOKI_', 'PC_'))}
    e['TOKI_SETTINGS'] = 'off'          # defaults only, nothing written
    e.update(env or {})
    r = subprocess.run([str(binary), *args, str(EXE)], cwd=ROOT, capture_output=True, text=True,
                       timeout=600, env=e)
    return r.returncode, r.stdout


@unittest.skipUnless(EXE.is_file(), 'needs pc/assets/raw/PSX.EXE (the game tables)')
class DecodeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.bin = build_harness(cls.temp.name)

    def test_equals_the_games_functions(self):
        for seed in ('1', '2', '3'):
            rc, out = harness(self.bin, 'random', '20000', seed)
            self.assertEqual(rc, 0, out)
            self.assertIn('mismatches 0 of 20000', out)
            # the sample reaches both outcomes of the special rules
            shiori = int(out.split('Shiori 785: ')[1].split()[0])
            self.assertGreater(shiori, 20, out)

    def test_names_are_the_games_tables(self):
        rc, out = harness(self.bin, 'names')
        self.assertEqual(rc, 0)
        rows = dict(l.split(' ', 1) if ' ' in l else (l, '') for l in out.splitlines())
        self.assertEqual(bytes.fromhex(rows['girl0']).decode('cp932'), '藤崎詩織')
        self.assertEqual(bytes.fromhex(rows['girl12']).decode('cp932'), '伊集院レイ')
        self.assertEqual(bytes.fromhex(rows['place0']).decode('cp932'), '近所の公園')

    def test_negative_controls(self):
        for m in MUTATIONS:
            with self.subTest(m[0]), tempfile.TemporaryDirectory() as d:
                b = build_harness(d, m)
                rc, out = harness(b, 'random', '20000', '1')
                self.assertNotEqual(rc, 0, f'{m[0]}: {out}')
                self.assertNotIn('mismatches 0 of', out)


# ---------------------------------------------------------------- the game

def call_script(path, pokes=()):
    """date_save.txt up to Shiori's phone call (the calendar); POKES
    (seeded: address, hex) right after the Sunday check"""
    lines = []
    for line in (ROOT / 'pc/tools/playthrough/date_save.txt').read_text().splitlines():
        lines.append(line)
        if pokes and line.startswith('check week_done '):
            lines += [f'poke 1 0x{a:08X} {h}' for a, h in pokes]
            lines.append('end 300')
            break
        if line.startswith('wait calendar '):
            lines.append('end 60')
            break
    Path(path).write_text('\n'.join(lines) + '\n')


def girl_pokes(girl, tok, yuu, bomb, flags, dates=0):
    rec = 0x800EE91C + 56 * girl
    data = (struct.pack('<hh', 0, tok) + struct.pack('<hh', 0, yuu) + struct.pack('<hh', 0, bomb) +
            struct.pack('<I', flags))
    out = [(rec, data.hex())]
    if dates:
        out.append((rec + 21, '%02x' % dates))
    return out


SEED = [  # girl, tokimeki, yuukou, bomb, flag word, dates
    (1, 60, 70, 59, 0x2, 5),
    (3, 30, 40, 60, 0x2, 0),
    (4, 10, 5, 20, 0x2, 0),
    (5, 40, 60, 76, 0x20002, 0),
    (7, 50, 50, 95, 0x2, 0),
]


def lines(path):
    return Path(path).read_text().splitlines() if Path(path).exists() else []


def present(path):
    return [(p[1], p[3] if len(p) > 3 else '') for p in (l.split() for l in lines(path))]


def blocks(path):
    """the status log: [text of each opening]"""
    out, cur = [], None
    for l in lines(path):
        if l.startswith('--- open '):
            cur = []
            out.append(cur)
        elif cur is not None:
            cur.append(l)
    return ['\n'.join(b) for b in out]


def field(block, key):
    for l in block.splitlines():
        if l.startswith(key + ' = '):
            return l.split(' = ', 1)[1]
    return None


def kv(text):
    return dict(p.split('=', 1) for p in text.split() if '=' in p)


@unittest.skipUnless(BINARY.is_file() and EXE.is_file(), 'needs the built port and pc/assets')
class InGame(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.dir = Path(cls.temp.name)
        cls.script = cls.dir / 'call.txt'
        call_script(cls.script)
        cls.seeded_script = cls.dir / 'seeded.txt'
        pokes = []
        for s in SEED:
            pokes += girl_pokes(*s)
        call_script(cls.seeded_script, pokes)
        cls.hbin = build_harness(cls.dir)
        keys = ('3590 key Ctrl+I Escape\n'
                '4100 key Ctrl+I Down Right Right Right Left Return Up Escape\n'
                '5300 key Ctrl+I Left Ctrl+P Escape\n')
        cls.closed = cls.play('closed', {})
        cls.off = cls.play('off', {'TOKI_STATUS': '0'})
        cls.used = cls.play('used', {}, keys)
        cls.hud = cls.play('hud', {'TOKI_STATUS_HUD': '1'}, '4100 key Ctrl+P\n')
        cls.seeded = cls.play('seeded', {}, '4100 key Ctrl+I Ctrl+P Escape\n', script=cls.seeded_script)
        cls.seeded_girl = cls.play('seeded_girl', {}, '4100 key Ctrl+I Down Down Down Down Return Ctrl+P '
                                   'Escape\n', script=cls.seeded_script)
        cls.seeded_hud = cls.play('seeded_hud', {'TOKI_STATUS_HUD': '1'}, '4100 key Ctrl+P\n',
                                  script=cls.seeded_script)
        # parity: the key alone (arrows would reach the game: no page holds them)
        cls.parity = cls.play('parity', {}, '3590 key Ctrl+I\n4100 key Ctrl+I\n5300 key Ctrl+I\n',
                              profile='parity')
        cls.parity_closed = cls.play('parity_closed', {}, profile='parity')
        # negative control: an assist that does press for the player
        cls.pressed = cls.play('pressed', {'TOKI_TEXT_AUTO': '1', 'TOKI_TEXT_AUTO_DELAY': '1'})

    @classmethod
    def play(cls, name, settings, keys=None, profile='enhanced', script=None):
        out = cls.dir / name
        out.mkdir()
        files = dict(frames=out / 'frames.txt', audio=out / 'audio.txt', present=out / 'present.txt',
                     log=out / 'status.txt', cfg=out / 'cfg')
        files['cfg'].mkdir()
        (out / 'saves').mkdir()
        sets = dict(PC_FRAME_HASH=files['frames'], PC_FRAME_HASH_RAM='game',
                    PC_AUDIO_HASH=files['audio'], PC_PRESENT_HASH=files['present'],
                    TOKI_STATUS_LOG=files['log'], TOKI_CONFIG_DIR=files['cfg'],
                    TOKI_SAVE_DIR=out / 'saves', **settings)
        if keys:
            (out / 'keys.txt').write_text(keys)
            sets['TOKI_INPUT_SCRIPT'] = out / 'keys.txt'
            sets['PC_PRESENT_WINDOW_SHOT'] = out / f'{name}.png'
        cmd = ['.venv/bin/python3' if (ROOT / '.venv/bin/python3').exists() else 'python3',
               'pc/tools/playthrough.py', str(script or cls.script), '--out', str(out / 'run'),
               '--bin', str(BINARY), '--profile', profile]
        for k, v in sets.items():
            cmd += ['--set', f'{k}={v}']
        r = run(cmd, cwd=ROOT, timeout=600, max_output=16 * 1024**2)
        log = out / 'run/date.pc.log'
        files['out'] = log.read_text(errors='replace') if log.exists() else ''
        files['result'] = r
        shot = os.environ.get('TEST_STATUS_SHOTS')
        if shot and (out / f'{name}.png').exists():
            Path(shot).mkdir(parents=True, exist_ok=True)
            shutil.copy(out / f'{name}.png', Path(shot) / f'{name}.png')
        return files

    def test_runs_pass(self):
        for name in ('closed', 'off', 'used', 'hud', 'seeded', 'parity', 'parity_closed'):
            r = getattr(self, name)['result']
            self.assertTrue(r.ok, f'{name}: {r.stdout[-1500:]}')

    def test_closed_is_the_same_as_off(self):
        self.assertGreater(len(lines(self.off['frames'])), 1000)
        self.assertIn(' ram=', lines(self.off['frames'])[0])
        self.assertEqual(lines(self.closed['frames']), lines(self.off['frames']))
        self.assertEqual(lines(self.closed['audio']), lines(self.off['audio']))
        self.assertEqual(present(self.closed['present']), present(self.off['present']))
        self.assertEqual(lines(self.closed['log']), [])
        self.assertNotIn('[STATUS]', self.off['out'])

    def test_using_the_page_changes_nothing_the_game_sees(self):
        out = self.used['out']
        self.assertEqual(out.count('[STATUS] open (in game)'), 3, out[-3000:])
        self.assertEqual(lines(self.used['frames']), lines(self.closed['frames']))
        self.assertEqual(lines(self.used['audio']), lines(self.closed['audio']))
        frames = present(self.used['present'])
        inserted = [h for h, tag in frames if tag == 'menu']
        self.assertGreaterEqual(len(inserted), 10)
        game = [(h, tag) for h, tag in frames if tag != 'menu']
        ref = present(self.closed['present'])
        self.assertEqual(len(game), len(ref))
        differ = [i for i, ((h, tag), (r, _)) in enumerate(zip(game, ref)) if h != r]
        self.assertTrue(all(game[i][1] == 'toast' for i in differ), differ[:20])

    def test_hud_draws_over_the_game_only(self):
        self.assertEqual(lines(self.hud['frames']), lines(self.closed['frames']))
        self.assertEqual(lines(self.hud['audio']), lines(self.closed['audio']))
        frames, ref = present(self.hud['present']), present(self.closed['present'])
        self.assertEqual(len(frames), len(ref))
        tagged = [i for i, (_, tag) in enumerate(frames) if tag == 'hud']
        self.assertGreater(len(tagged), 500)
        differ = [i for i, ((h, tag), (r, _)) in enumerate(zip(frames, ref)) if h != r]
        self.assertTrue(set(differ) <= set(tagged) | {i for i, (_, t) in enumerate(frames)
                                                        if t == 'toast'}, differ[:20])
        self.assertTrue(differ)
        # before the game starts (title, name entry) there is no line
        self.assertEqual(frames[0][1], '')

    def test_values_are_the_games(self):
        b = blocks(self.used['log'])
        self.assertEqual(len(b), 3)
        first, sunday, call = b
        # the first day: parameter_init's values, girl_rec_init's records,
        # Shiori from name_ent_girl_para_set's table
        self.assertEqual(kv(field(first, 'date'))['youbi'], '1')
        self.assertTrue(field(first, 'date').startswith('95/4/4 '))
        self.assertEqual(field(first, 'stats'), '100 40 40 40 40 32 60 5 0')
        g0 = kv(field(first, 'girl0'))
        self.assertIn((int(g0['yuu']), int(g0['tok']), int(g0['bomb'])),
                      [(45, 10, 10), (50, 20, 0), (50, 25, 0), (55, 25, 0)])
        self.assertEqual(g0['met'], '1')
        for i in range(1, 11):
            g = kv(field(first, f'girl{i}'))
            self.assertEqual((g['met'], g['tok'], g['yuu'], g['bomb'], g['dates']),
                             ('0', '0', '50', '0', '0'), i)
        self.assertIn('nmet=1', field(first, 'ending'))
        # the playthrough's own check: calendar:7=005f0409060500
        self.assertTrue(field(sunday, 'date').startswith('95/4/9 youbi=6 season=0 pass_days=5'))
        # every opening's RAM decodes offline to the same text, and the
        # game's own functions agree on it
        for k, text in enumerate(b):
            rc, out = harness(self.hbin, 'image', f"{self.used['log']}.{k}.ram")
            self.assertEqual(rc, 0, out[-2000:])
            self.assertEqual(out.rsplit('mismatches', 1)[0].strip(), text.strip())

    def test_seeded_values_and_thresholds(self):
        b = blocks(self.seeded['log'])
        self.assertEqual(len(b), 1)
        text = b[0]
        for girl, tok, yuu, bomb, flags, dates in SEED:
            g = kv(field(text, f'girl{girl}'))
            self.assertEqual((int(g['tok']), int(g['yuu']), int(g['bomb']), int(g['flags'], 16),
                              int(g['dates'])), (tok, yuu, bomb, flags, dates), girl)
            self.assertEqual(g['met'], '1')
            self.assertEqual(g['lit'], '1' if bomb >= 60 else '0', girl)     # 59 no, 60 yes
        self.assertEqual(kv(field(text, 'girl5'))['latch61'], '1')
        self.assertEqual(kv(field(text, 'girl3'))['latch61'], '0')
        self.assertEqual(kv(field(text, 'girl4'))['neglect'], '1')         # 5 x 131 / 100 = 6 %
        self.assertEqual(kv(field(text, 'girl1'))['neglect'], '0')
        e = kv(field(text, 'ending'))
        self.assertEqual((e['most_bomb'], e['nmet']), ('7', '6'))
        rc, out = harness(self.hbin, 'image', f"{self.seeded['log']}.0.ram")
        self.assertEqual(rc, 0, out[-2000:])
        self.assertIn('[STATUS] open (in game)', self.seeded_girl['out'])

    def test_page_draws_the_seeded_state(self):
        ram = f"{self.seeded['log']}.0.ram"
        rc, out = harness(self.hbin, 'page', ram, env={'TOKI_STATUS': '1', 'TOKI_STATUS_HUD': '1'})
        self.assertEqual(rc, 0, out[-2000:])
        tabs = out.split('TAB ')[1:]
        self.assertEqual(len(tabs), 4)
        by_y = {}
        for r in (l.split(None, 3) for l in tabs[0].splitlines()[1:]):
            if len(r) == 4:
                by_y.setdefault(int(r[1]), []).append((int(r[0]), r[2], r[3]))
        names = {min(v)[2]: v for v in by_y.values() if min(v)[0] == 2}
        # the bomb number: left-aligned after the meter (status_view.c COL_BOMB)
        bomb = {n: [(c, t) for x, c, t in v if x == 184] for n, v in names.items()}
        # 59 is not lit, 60 / 76 / 95 are (the game's bomb sprite rule)
        self.assertEqual([bomb[n][0][1] for n in ('Mio', 'Ayako', 'Yukari', 'Mira')],
                         ['59', '60!', '76!', '95!'])
        self.assertEqual(bomb['Ayako'][0][0], 'ff5050ff')      # UI_C_LIT
        self.assertEqual(bomb['Mio'][0][0], 'f8f8f8ff')        # UI_C_TEXT, the game's white
        # every text of every tab within the 284 x 168 canvas (latin12 is
        # proportional: the page measures, cuts and wraps)
        self.assertNotIn('OUTSIDE', out)
        self.assertIn('Nobody: the Yoshio ending', tabs[2])
        self.assertIn('Today 1995/4/9 Sun', tabs[3])
        self.assertIn('HUD Stress', out)
        self.assertIn('Bomb: Mira 95!', out)
        # negative control: the switch off (the parity default) -> no page
        rc, out = harness(self.hbin, 'page', ram)
        self.assertEqual((rc, out.strip().splitlines()[-1]), (3, 'no page'))

    def test_parity_has_no_page(self):
        self.assertNotIn('[STATUS]', self.parity['out'])
        self.assertNotIn('settings menu open (status)', self.parity['out'])
        self.assertEqual(lines(self.parity['log']), [])
        self.assertEqual(lines(self.parity['frames']), lines(self.parity_closed['frames']))
        self.assertEqual(lines(self.parity['audio']), lines(self.parity_closed['audio']))
        self.assertNotIn('menu', {t for _, t in present(self.parity['present'])})

    def test_negative_control_a_press_is_seen(self):
        self.assertNotEqual(lines(self.pressed['frames']), lines(self.closed['frames']))


if __name__ == '__main__':
    unittest.main()
