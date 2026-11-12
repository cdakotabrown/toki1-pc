# SPDX-License-Identifier: GPL-3.0-or-later
"""Quick text ([enhanced] quick_text, TOKI_QUICK_TEXT; pc/hal/quick_text.c).

A confirm press while a dialogue page prints completes the page (held back
from the game; check_end_k_tbl1 set to the page's end at the frame
boundary); one while its voice line plays (voice_level 2) reaches the game
as cross, the game's own cut.

Harness (pc/tests/quick_text_harness.c, no game data): a synthetic text
engine and the controller-port model -- completion at the frame end and
not at the poll, the press held back while held, the cut for as long as it
is held, nothing after the voice / without a voice / at voice_level 0 or 1
/ outside a dialogue / for a press held from before the page, the TEL
scroll window, a page that changed before the frame end, the mouse (and
swapped buttons), and the feature off.  Negative controls: five source
mutations of quick_text.c, each caught.

On the port (needs the built port and pc/assets; skipped otherwise),
date_save.txt's opening up to Shiori's phone call, then scripted presses
on her greeting (one line, voiced):
  * enhanced, TOKI_QUICK_TEXT=1: a press in mid-print never reaches the
    game and the page is complete two VBlanks later; a press during the
    voice reaches it as cross: the drive leaves the playing state and the
    call goes to its next page;
  * negative control, TOKI_QUICK_TEXT=0: the same presses do what the
    console does -- the page prints on, circle during the voice is ignored;
  * an automated enhanced run without the variable (the feature is off in
    automated runs) has exactly the trace of the explicit-off run;
  * parity: no [QUICK] line, the presses reach the game as circle;
  * the whole call tapped through (circle every 25 frames) with and without
    the feature reaches the calendar with the same game state (screen, step,
    calendar, player record, girls' table), the backlog captures the same
    pages (text and voice request) and the read-page record the same pages;
  * the date (serifu_main, DATE): the same two presses complete a page and
    cut its voice there too.
QUICK_TEXT_WINDOWS=1 runs the port part on the Windows build under Wine.
"""
import json
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile
import threading
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from safe_process import run  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
BINARY = ROOT / 'pc/build/tokimeki-pc'
# QUICK_TEXT_WINDOWS=1: the port runs are the Windows build under Wine, with
# no display (DISPLAY / WAYLAND_DISPLAY left out: Wine has nowhere to show a
# window or a dialog; SDL draws to its dummy driver either way)
WINDOWS = os.environ.get('QUICK_TEXT_WINDOWS', '') not in ('', '0')
RUNNER = []
if WINDOWS:
    BINARY = ROOT / 'pc/build-win/tokimeki-pc.exe'
    RUNNER = ['--runner', shutil.which('wine') or 'wine']
ASSETS = ROOT / 'pc/assets'
PY = str(ROOT / '.venv/bin/python3') if (ROOT / '.venv/bin/python3').exists() else 'python3'

TRACE = ('trace check_end_k_tbl0:2 check_end_k_tbl1:2 serifu_state:4 serifu_voice:4 '
         'voice_level:1 xa_m_mode:4 cd_state:1 girl_tbl:56 reserved_date:4')
GREETING = ('now_step=0x45 now_sub_step=1 now_sub_sub_step=1 step_mode=1 xa_m_mode:4=1 '
            'check_end_k_tbl0:2=33')
COMPLETED = re.compile(r'\[QUICK\] page completed: (\d+) -> (\d+) of (\d+) glyphs '
                       r'\(step (\w+)/(\w+)/(\w+)/(\w+)\)')
CUT = re.compile(r'\[QUICK\] voice cut: confirm sent as cross \(step (\w+)/')

# ---------------------------------------------------------------- harness

HARNESS_SOURCES = ['pc/hal/quick_text.c', 'pc/hal/pad_device.c']
MUTATIONS = [
    ('press not held back', 'pc/hal/quick_text.c',
     '    if (g_mode == QT_HOLD) return player & ~CONFIRM;\n', '', 'print_completes'),
    ('written at the poll', 'pc/hal/quick_text.c',
     '        g_pending = t;\n', '        g_pending = t;\n        check_end_k_tbl1 = (s16)t;\n',
     'print_completes'),
    ('every screen is a dialogue', 'pc/hal/quick_text.c',
     '    if (!serifu && !tel) return QT_PASS;\n', '', 'not_dialogue'),
    ('no scroll window', 'pc/hal/quick_text.c',
     '    if (!tel || !(k_mode & 0x80)) return end;\n', '    return end;\n', 'tel_scroll'),
    ('cut at voice_level 1', 'pc/hal/quick_text.c',
     'voice_level == 2 && voice_playing(tel)', 'voice_level != 0 && voice_playing(tel)',
     'voice_level'),
]


def build_harness(folder, mutation=None):
    folder = Path(folder)
    paths = {s: ROOT / s for s in HARNESS_SOURCES}
    if mutation:
        _, path, old, new, _ = mutation
        text = (ROOT / path).read_text()
        assert text.count(old) == 1, f'mutation anchor not unique in {path}: {old!r}'
        copy = folder / Path(path).name
        copy.write_text(text.replace(old, new))
        paths[path] = copy
    binary = folder / 'quick_text_harness'
    run(['cc', '-std=gnu17', '-D_GNU_SOURCE', '-w', '-O1', '-fno-pie', '-no-pie',
         '-Ipc/include', '-Ipc/include/game', '-Ipc/hal', '-include', 'pc/include/game/types.h',
         str(ROOT / 'pc/tests/quick_text_harness.c'), *[str(paths[s]) for s in HARNESS_SOURCES],
         '-o', str(binary)], cwd=ROOT, timeout=120).require_ok()
    return binary


def harness(binary, *args, on=True):
    env = {k: v for k, v in os.environ.items() if not k.startswith(('TOKI_', 'PC_'))}
    env['TOKI_QUICK_TEXT'] = '1' if on else '0'
    r = run([str(binary), *args], cwd=ROOT, env=env, timeout=60)
    lines = [l for l in r.stdout.splitlines() if not l.startswith('[')]
    return r.returncode, lines


class Harness(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix='quick-text-harness-'))
        cls.binary = build_harness(cls.tmp)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_cases(self):
        rc, out = harness(self.binary)
        self.assertEqual(rc, 0, out)
        self.assertEqual(out, ['ok print_completes', 'ok after_voice_passes', 'ok voice_level',
                               'ok not_dialogue', 'ok held_from_before', 'ok tel_scroll',
                               'ok page_changed', 'ok mouse'])

    def test_off(self):
        rc, out = harness(self.binary, 'off', on=False)
        self.assertEqual((rc, out), (0, ['ok off']))
        # negative control: the "off" case fails with the feature on
        rc, out = harness(self.binary, 'off', on=True)
        self.assertNotEqual(rc, 0)

    def test_mutations_are_caught(self):
        for m in MUTATIONS:
            with self.subTest(m[0]):
                folder = self.tmp / re.sub(r'\W+', '_', m[0])
                folder.mkdir()
                rc, out = harness(build_harness(folder, m))
                self.assertNotEqual(rc, 0, f'{m[0]}: not caught: {out}')
                self.assertTrue(any(l.startswith(f'FAIL {m[4]}') for l in out), out)


# ---------------------------------------------------------------- the port

def script_until_call():
    """date_save.txt up to choosing Shiori in the phone book."""
    lines = []
    in_call = False
    for line in (ROOT / 'pc/tools/playthrough/date_save.txt').read_text().splitlines():
        if line.startswith('trace '):
            line = TRACE
        if line.startswith('wait phonebook '):
            in_call = True
        if in_call and line.startswith('+120 circle'):
            break
        lines.append(line)
    return lines


def script_until_date():
    lines = []
    for line in (ROOT / 'pc/tools/playthrough/date_save.txt').read_text().splitlines():
        if line.startswith('trace '):
            line = TRACE
        lines.append(line)
        if line.startswith('wait date_start '):
            return lines
    raise AssertionError('date_save.txt has no date_start')


def decode(path):
    """trace -> {vblank: {field: int}} (little-endian hex fields)."""
    rows = {}
    with open(path) as fh:
        lines = fh.readlines()
    for line in lines:
        if line.startswith('#'):
            continue
        d = dict(kv.split('=', 1) for kv in line.split() if '=' in kv)
        row = {}
        for k, v in d.items():
            if k == 'frame':
                continue
            try:
                row[k] = int.from_bytes(bytes.fromhex(v), 'little') if k != 'pad' else int(v, 16)
            except ValueError:
                pass
        rows[int(d['frame'])] = row
    return rows


def milestone(text, label):
    m = re.search(r'\]\s+' + re.escape(label) + r'\s+game frame\s+\d+\s+vblank\s+(\d+)', text)
    return int(m.group(1)) if m else None


@unittest.skipUnless(BINARY.is_file() and (ASSETS / 'raw/PSX.EXE').is_file(),
                     'needs the built port and pc/assets')
class OnThePort(unittest.TestCase):
    RUNS = {}

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix='quick-text-test-'))
        call = script_until_call()
        presses = [f'wait p {GREETING} check_end_k_tbl1:2=24 within 900',
                   '+1 circle', '+15 circle', 'end 200']
        (cls.tmp / 'presses.txt').write_text('\n'.join(call + presses) + '\n')
        tap = ['wait calendar now_step=0x45 now_sub_sub_step=0x20 step_mode=1 within 2400 '
               'mash circle/25', 'end 30']
        (cls.tmp / 'tap.txt').write_text('\n'.join(call + tap) + '\n')
        date = ['wait p now_step=0x50 serifu_state:4=1 serifu_voice:4=1 check_end_k_tbl1:2=8 '
                'within 2400 mash circle/40', '+1 circle', '+10 circle', 'end 200']
        (cls.tmp / 'date.txt').write_text('\n'.join(script_until_date() + date) + '\n')
        jobs = {
            'on': ('presses.txt', 'enhanced', {'TOKI_QUICK_TEXT': '1'}),
            'off': ('presses.txt', 'enhanced', {'TOKI_QUICK_TEXT': '0'}),
            'default': ('presses.txt', 'enhanced', {}),
            'parity': ('presses.txt', 'parity', {}),
            'tap_on': ('tap.txt', 'enhanced', {'TOKI_QUICK_TEXT': '1'}),
            'tap_off': ('tap.txt', 'enhanced', {'TOKI_QUICK_TEXT': '0'}),
            'date_on': ('date.txt', 'enhanced', {'TOKI_QUICK_TEXT': '1'}),
            'date_off': ('date.txt', 'enhanced', {'TOKI_QUICK_TEXT': '0'}),
        }
        threads = [threading.Thread(target=cls.play, args=(n, *j)) for n, j in jobs.items()]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    @classmethod
    def play(cls, name, script, profile, sets):
        out = cls.tmp / name
        out.mkdir()
        sets = dict(sets, TOKI_BACKLOG_LOG=out / 'backlog.jsonl', TOKI_SEEN_FILE=out / 'seen.dat')
        if profile == 'parity':
            sets = {}
        cmd = [PY, 'pc/tools/playthrough.py', str(cls.tmp / script), '--out', str(out / 'run'),
               '--bin', str(BINARY), '--profile', profile, *RUNNER]
        for k, v in sets.items():
            cmd += ['--set', f'{k}={v}']
        env = {k: v for k, v in os.environ.items() if k not in ('DISPLAY', 'WAYLAND_DISPLAY')}
        r = run(cmd, cwd=ROOT, env=env, timeout=3600, max_output=16 * 1024**2)
        log = out / 'run/date.pc.log'
        text = log.read_text(errors='replace') if log.exists() else ''
        cls.RUNS[name] = dict(result=r, text=text, dir=out, out=r.stdout,
                              trace=out / 'run/date.pc.trace')

    def get(self, name):
        r = self.RUNS[name]
        self.assertTrue(r['result'].ok, f'{name}: {r["result"].stdout[-2000:]}')
        return r

    def press_rows(self, name):
        r = self.get(name)
        v = milestone(r['out'], 'p')
        self.assertIsNotNone(v, r['out'][-1500:])
        rows = decode(r['trace'])
        return v, rows

    def test_1_print_then_voice(self):
        v, rows = self.press_rows('on')
        text = self.RUNS['on']['text']
        done = COMPLETED.findall(text)
        self.assertIn(('24', '33', '33', '45', '01', '01', '01'), done)
        self.assertEqual(len(CUT.findall(text)), 1)
        first = [rows[f] for f in range(v, v + 6) if f in rows]
        self.assertFalse(any(r['pad'] & 0x20 for r in first), 'the first press reached the game')
        self.assertEqual(rows[v + 3]['check_end_k_tbl1'], 33)
        cut = [f for f in range(v + 6, v + 40) if f in rows and rows[f]['pad_trig'] & 0x40]
        self.assertTrue(cut, 'no cross from the second press')
        after = rows[cut[0] + 2]
        self.assertGreater(after['step_mode'], 1)          # the call's next page
        self.assertNotEqual(after['cd_state'], 4)          # the line stopped

    def test_2_negative_control_off(self):
        v, rows = self.press_rows('off')
        self.assertNotIn('[QUICK]', self.RUNS['off']['text'])
        first = [rows[f] for f in range(v, v + 6) if f in rows]
        self.assertTrue(any(r['pad_trig'] & 0x20 for r in first))
        self.assertLess(rows[v + 3]['check_end_k_tbl1'], 33)   # prints on
        second = [f for f in range(v + 6, v + 40) if f in rows and rows[f]['pad_trig'] & 0x20]
        self.assertTrue(second)
        after = rows[second[0] + 2]
        self.assertEqual(after['step_mode'], 1)            # circle during the voice: ignored
        self.assertEqual(after['cd_state'], 4)

    def test_3_automated_default_is_off(self):
        self.get('default')
        self.assertNotIn('[QUICK]', self.RUNS['default']['text'])
        self.assertEqual(Path(self.RUNS['default']['trace']).read_bytes(),
                         Path(self.get('off')['trace']).read_bytes())

    def test_4_parity(self):
        v, rows = self.press_rows('parity')
        text = self.RUNS['parity']['text']
        self.assertNotIn('[QUICK]', text)
        self.assertNotIn('TOKI_QUICK_TEXT      on', text)
        presses = [f for f in range(v, v + 40) if f in rows and rows[f]['pad_trig'] & 0x20]
        self.assertEqual(len(presses), 2)                  # both reach the game as circle

    def test_5_tapped_call_same_state(self):
        on, off = self.get('tap_on'), self.get('tap_off')
        self.assertGreaterEqual(len(COMPLETED.findall(on['text'])), 1)
        self.assertGreaterEqual(len(CUT.findall(on['text'])), 1)
        self.assertNotIn('[QUICK]', off['text'])
        a, b = milestone(on['out'], 'calendar'), milestone(off['out'], 'calendar')
        self.assertIsNotNone(a, on['out'][-1500:])
        self.assertIsNotNone(b, off['out'][-1500:])
        self.assertLess(a, b, 'the call took no less time')
        ra, rb = decode(on['trace'])[a], decode(off['trace'])[b]
        for k in ('now_step', 'now_sub_step', 'now_sub_sub_step', 'step_mode', 'calendar',
                  'player', 'girl_tbl', 'reserved_date', 'now_girl', 'voice_level'):
            self.assertEqual(ra[k], rb[k], k)

        def pages(run):
            return [json.loads(line) for line in
                    (run['dir'] / 'backlog.jsonl').read_text().splitlines()]

        def text(p):
            return (p['screen'], p['speaker'], [s['sjis'] for s in p['segs']], 'voice' in p)
        pa, pb = pages(on), pages(off)
        self.assertGreaterEqual(len(pb), 3)
        self.assertEqual([text(p) for p in pa], [text(p) for p in pb],
                         'the backlog took other pages')
        # a completed-early page keeps its whole line for the replay: the
        # request window of the same voice line is the same.  (The call's
        # last page speaks in two parts -- xa_wait_m chains xa_m_tbl2 -- and
        # the capture attaches the newest part: the second when it plays
        # out, the first when the player cuts it.)
        same = [(a['voice'], b['voice']) for a, b in zip(pa, pb)
                if 'voice' in a and a['voice']['file'] == b['voice']['file']]
        self.assertTrue(same)
        for a, b in same:
            self.assertEqual(a['req'], b['req'])
            self.assertLessEqual(a['sectors'], b['sectors'])   # cut short, never longer
        self.assertEqual(sorted((on['dir'] / 'seen.dat').read_bytes()[8:]),
                         sorted((off['dir'] / 'seen.dat').read_bytes()[8:]))

    def test_6_date_serifu(self):
        on, off = self.get('date_on'), self.get('date_off')
        v, rows = self.press_rows('date_on')
        done = [d for d in COMPLETED.findall(on['text']) if d[3] == '50']
        self.assertTrue(done, on['text'][-2000:])
        self.assertTrue([c for c in CUT.findall(on['text']) if c == '50'])
        first = [rows[f] for f in range(v, v + 6) if f in rows]
        self.assertFalse(any(r['pad'] & 0x20 for r in first))
        self.assertEqual(rows[v + 3]['check_end_k_tbl1'], rows[v + 3]['check_end_k_tbl0'])
        cut = [f for f in range(v + 6, v + 40) if f in rows and rows[f]['pad_trig'] & 0x40]
        self.assertTrue(cut)
        self.assertEqual(rows[cut[0]]['serifu_state'], 2)   # serifu_main's own cut
        self.assertNotIn('[QUICK]', off['text'])
        v, rows = self.press_rows('date_off')
        self.assertLess(rows[v + 3]['check_end_k_tbl1'], rows[v + 3]['check_end_k_tbl0'])


if __name__ == '__main__':
    unittest.main()
