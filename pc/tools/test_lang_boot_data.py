#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""test_lang_boot_data.py -- the retail-initialised globals follow the language.

pc/src/main.c loads the language's PSX.EXE into PS1 RAM and then fills the
pc_retail_init[] globals (pc/src/missing_globals_gen.c, pc/src/globals.c)
from an executable.  Most of those names link to their PS1 addresses
(pc/ld/psx_globals.syms), so the fill writes PS1 RAM: until 2026-11-02 it
read the JAPANESE executable, which put the Japanese bytes back over 43
English strings and tables of the English executable -- the club-joining
message ("Swimming部に□部した。"), the captain's club-day line, the
options-menu labels, the save dialogs, the club-select names.

The test boots the game to the title menu and checks, for every such
global whose English and Japanese bytes differ (and that the symbol ledger
names), that RAM holds the English bytes with TOKI_LANG=en and the
Japanese bytes with TOKI_LANG=ja.  Negative control: the same checks with
the other language's bytes must FAIL (so the check can see the difference).

Needs the PC build (pc/build/tokimeki-pc) with an English pack; skipped
otherwise.  The English tree pc/assets-en is prepared by the test itself:
setUpClass boots the game once with TOKI_LANG=en (prepare_english_tree),
which writes the tree from the pack embedded in the binary and rewrites
every file that does not match it, so a fresh checkout has one and a tree
left by an older pack is brought up to date before its PSX.EXE is read.
No retail byte is stored: the expected values are read from the player's
two executables at run time.
"""
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pc_paths import DECOMP, ENGLISH  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BIN = os.path.join(ROOT, 'pc', 'build', 'tokimeki-pc')
JA_EXE = os.path.join(ROOT, 'pc', 'assets', 'raw', 'PSX.EXE')
EN_EXE = os.path.join(ROOT, 'pc', 'assets-en', 'raw', 'PSX.EXE')
TABLES = ('pc/src/missing_globals_gen.c', 'pc/src/globals.c')
ROW = re.compile(r'\{\s*([A-Za-z_]\w*)\s*,\s*0x([0-9A-Fa-f]{8})u?\s*,\s*(\d+)u?\s*\}')
MAX_SIZE = 32          # bytes per check (playthrough conditions)


def exe_image(path):
    with open(path, 'rb') as f:
        b = f.read()
    load, size = struct.unpack_from('<II', b, 0x18)
    return load, b[0x800:0x800 + size]


def retail_init_rows():
    rows = []
    for rel in TABLES:
        with open(os.path.join(ROOT, rel), encoding='utf-8') as f:
            rows += [(m.group(1), int(m.group(2), 16), int(m.group(3)))
                     for m in ROW.finditer(f.read())]
    return rows


def differing(rows, ja, en, load, names):
    """(name, size, ja bytes, en bytes) of the checkable globals that differ"""
    out = []
    for name, addr, size in rows:
        o = addr - load
        a, b = ja[o:o + size], en[o:o + size]
        if a == b or name not in names:
            continue
        n = min(size, MAX_SIZE)
        if a[:n] == b[:n]:
            continue
        out.append((name, n, a[:n], b[:n]))
    return out


def script(checks, which):
    """a `check` line per <= 6 conditions (PC_INPUT allows 8 conditions and a
    1,024-byte line per wait)"""
    lines = ['hold 2', 'phase boot',
             'wait title_menu now_step=0x12 now_sub_step=2 now_sub_sub_step=1 within 3000']
    def lit(size, b):
        # playthrough.parse_cond: SIZE <= 4 takes an integer, longer ones hex bytes
        return hex(int.from_bytes(b, 'little')) if size <= 4 else b.hex()
    conds = [f'{n}:{s}={lit(s, e if which == "en" else j)}' for n, s, j, e in checks]
    for i in range(0, len(conds), 6):
        lines.append(f'check boot_data_{i // 6} ' + ' '.join(conds[i:i + 6]))
    return '\n'.join(lines) + '\nend 4\n'


def run(lang, text, out, extra=()):
    with tempfile.NamedTemporaryFile('w', suffix='.txt', dir=out, delete=False) as f:
        f.write(text)
    r = subprocess.run([sys.executable, os.path.join(ROOT, 'pc/tools/playthrough.py'), f.name,
                        '--set', f'TOKI_LANG={lang}', '--out', os.path.join(out, lang),
                        '--timeout', '300'] + [x for e in extra for x in ('--set', e)],
                       cwd=ROOT, capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


# title -> Game Start -> the memory-card check dialog: the first strings the
# English engine draws (english_tour.txt's opening presses)
TEXTLOG_SCRIPT = ('hold 2\nphase log\n'
                  'wait title_menu now_step=0x12 now_sub_step=2 now_sub_sub_step=1 within 3000\n'
                  '+30 start\n'
                  'wait album_prompt now_step=0x13 now_sub_step=2 now_sub_sub_step=0x41 within 3000\n'
                  'end 60\n')


def prepare_english_tree(out):
    """Boot once with TOKI_LANG=en (hal_lang_init -> translation_prepare):
    the game writes pc/assets-en from its embedded pack, checking every
    file against the pack's SHA-1, so the tree is the current pack's.
    Returns the run's log when the tree has no PSX.EXE afterwards."""
    rc, log = run('en', 'hold 2\nphase prepare\nend 1\n', out)
    game = ''
    try:
        with open(os.path.join(out, 'en', 'prepare.pc.log'), errors='replace') as f:
            game = f.read()
    except OSError:
        pass
    if os.path.exists(EN_EXE) and '[LANG] English' in game:
        return None
    lang = '\n'.join(x for x in game.splitlines() if 'LANG' in x or 'TRANSLATION' in x)
    return 'exit %d\n%s\n%s' % (rc, lang[-2000:], log[-2000:])


@unittest.skipIf(ENGLISH is None, 'toki1-english is absent (pc/tools/pc_paths.py): '
                 'a build without the English patch has no English boot data')
@unittest.skipUnless(os.path.exists(BIN) and os.path.exists(JA_EXE),
                     'needs pc/build/tokimeki-pc and the extracted disc pc/assets')
class LangBootData(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out = tempfile.mkdtemp(prefix='lang_boot_', dir=os.path.join(ROOT, 'build'))
        failed = prepare_english_tree(cls.out)
        if failed:
            shutil.rmtree(cls.out, ignore_errors=True)
            raise AssertionError('TOKI_LANG=en did not prepare the English tree '
                                 'pc/assets-en (does pc/build/tokimeki-pc embed the '
                                 'pack? pc/tools/translation_pack.py build, then '
                                 'relink): ' + failed)
        with open(os.path.join(DECOMP, 'config', 'symbol_names.json'), encoding='utf-8') as f:
            led = json.load(f)
        recs = led.get('symbols', led) if isinstance(led, dict) else led
        names = {r['name'] for r in (recs.values() if isinstance(recs, dict) else recs)
                 if isinstance(r, dict) and 'name' in r}
        load, ja = exe_image(JA_EXE)
        load2, en = exe_image(EN_EXE)
        assert load == load2
        cls.checks = differing(retail_init_rows(), ja, en, load, names)

    @classmethod
    def tearDownClass(cls):
        if not os.environ.get('KEEP_TEST_OUTPUT'):
            shutil.rmtree(cls.out, ignore_errors=True)

    def test_checks_cover_the_reported_strings(self):
        got = {c[0] for c in self.checks}
        for n in ('join_club_message_str0', 'join_club_message_str2', 'cursor_speed_init_str0'):
            self.assertIn(n, got)

    def test_english_ram_holds_english(self):
        rc, log = run('en', script(self.checks, 'en'), self.out)
        self.assertEqual(rc, 0, log[-3000:])

    def test_japanese_ram_holds_japanese(self):
        rc, log = run('ja', script(self.checks, 'ja'), self.out)
        self.assertEqual(rc, 0, log[-3000:])

    def test_negative_control_english_is_not_japanese(self):
        """the check sees the difference: en RAM does not hold the ja bytes"""
        rc, _ = run('en', script(self.checks, 'ja'), self.out)
        self.assertNotEqual(rc, 0)

    def test_textlog(self):
        """pc/hal/text_audit.c: TOKI_TEXTLOG logs the English engine's draws
        (with their PS1 addresses); in Japanese it writes nothing"""
        for lang in ('en', 'ja'):
            log = os.path.join(self.out, f'text_{lang}.jsonl')
            rc, out = run(lang, TEXTLOG_SCRIPT, self.out, (f'TOKI_TEXTLOG={log}',))
            self.assertEqual(rc, 0, out[-3000:])
            rows = []
            if os.path.exists(log):
                with open(log, encoding='utf-8') as f:
                    rows = [json.loads(x) for x in f]
            if lang == 'ja':
                self.assertEqual(rows, [])
                continue
            self.assertTrue(rows, 'no draw logged in English')
            self.assertTrue(all(set(r) >= {'f', 'step', 'x', 'y', 'addr', 'host', 'hex'}
                                for r in rows))
            # the memory-card check dialog: PS1-RAM strings, English bytes
            ps1 = [r for r in rows if not r['host']]
            self.assertTrue(ps1)
            self.assertTrue(any(bytes.fromhex(r['hex']).isascii() for r in ps1))


if __name__ == '__main__':
    unittest.main()
