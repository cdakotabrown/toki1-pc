#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The PC layer's UI font (pc/include/pc_ui.h): latin12 compiled in.

  * the compiled tables, read back through ui_glyph() by
    pc/tests/ui_font_harness.c, equal pc/font/latin12.txt and
    pc/font/ui_extra12.txt glyph for glyph (advance and every row), parsed
    here independently of the generator; ui_text_width() equals the patch
    tool's own measure (fontgen.text_width) on sample strings;
  * pc/font/latin12.txt is byte for byte the English patch's font,
    tools/patch/font/latin12.txt, and the generator reads it as the patch's
    own parser (tools/patch/fontgen.py) does -- both need toki1-english and
    are skipped, by name, without it;
  * negative controls: a copy of latin12 with one pixel flipped, or one
    advance changed, compiles to tables the comparison rejects; an extra
    glyph that tries to redefine ASCII is refused by the generator;
  * the unit tests of pc/tests/test_ui_font.c pass, and each of six
    mutations of pc/hal/ui_font.c (wrap, cut, UTF-8, spans, measure) makes
    them fail.
Needs only gcc and python3.
"""
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pc_paths import ENGLISH, english_path  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
LATIN = ROOT / 'pc/font/latin12.txt'
NO_ENGLISH = 'toki1-english is absent (pc/tools/pc_paths.py): no English patch font to compare'
EXTRA = ROOT / 'pc/font/ui_extra12.txt'
GEN = ROOT / 'pc/tools/gen_ui_font.py'
SRC = ROOT / 'pc/hal/ui_font.c'

MUTATIONS = [
    ('wrap ignores spaces', 'if (space && space > s) p = space;', 'if (0) p = space;'),
    ('wrap ignores path separators', 'else if (sep) p = sep;', 'else if (0) p = sep;'),
    ('cut forgets the ellipsis width', 'if (w + a + ell > maxw) break;', 'if (w + a > maxw) break;'),
    ('UTF-8 takes any continuation', 'if ((s[i] & 0xC0) != 0x80)', 'if (0)'),
    ('spans one pixel long', 'fn(ctx, x + start, r, len);', 'fn(ctx, x + start, r, len + 1);'),
    ('measure adds a pixel', 'w += ui_glyph(cp, NULL);', 'w += ui_glyph(cp, NULL) + 1;'),
]


def parse(path):
    """{code point: (adv, [rows as '.#' strings padded to adv-1])}"""
    out = {}
    lines = Path(path).read_text(encoding='utf-8').splitlines()
    i = 0
    while i < len(lines):
        line = lines[i]
        i += 1
        if not line.startswith('glyph '):
            continue
        _, name, _, adv = line.split()
        cp = 0x20 if name == 'space' else int(name[2:], 16) if name.startswith('U+') else ord(name)
        out[cp] = (int(adv), lines[i:i + 12])
        i += 12
    return out


def rows_bits(rows):
    return [sum(1 << (15 - x) for x, c in enumerate(r) if c == '#') for r in rows]


def fontgen():
    spec = importlib.util.spec_from_file_location('fontgen', english_path('tools/patch/fontgen.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def build(folder, latin=LATIN, extra=EXTRA, src=SRC):
    folder = Path(folder)
    data = folder / 'ui_font_data.c'
    r = subprocess.run([sys.executable, str(GEN), '--out', str(data), '--latin', str(latin),
                        '--extra', str(extra)], capture_output=True, text=True)
    if r.returncode:
        return None, r.stderr + r.stdout
    out = folder / 'ui_font_harness'
    subprocess.run(['gcc', '-std=gnu17', '-O1', '-w', '-I', 'pc/include', 'pc/tests/ui_font_harness.c',
                    'pc/tests/test_ui_font.c', str(src), str(data), '-o', str(out)],
                   cwd=ROOT, check=True, capture_output=True, text=True)
    return out, ''


def dump(binary):
    out = {}
    for line in subprocess.run([str(binary), 'dump'], capture_output=True, text=True,
                               check=True).stdout.splitlines():
        p = line.split()
        out[int(p[0][2:], 16)] = (int(p[1]), [int(x, 16) for x in p[2:]])
    return out


def differences(compiled, *sources):
    """what differs between the compiled tables and the text files"""
    want = {}
    for s in sources:
        want.update(parse(s))
    diff = []
    for cp in sorted(set(want) | set(compiled)):
        if cp not in want or cp not in compiled:
            diff.append(f'U+{cp:04X} only in {"compiled" if cp in compiled else "text"}')
            continue
        adv, rows = want[cp]
        if compiled[cp][0] != adv:
            diff.append(f'U+{cp:04X} adv {compiled[cp][0]} != {adv}')
        if compiled[cp][1] != rows_bits(rows):
            diff.append(f'U+{cp:04X} rows differ')
    return diff


class Tables(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.bin, err = build(cls.tmp.name)
        assert cls.bin, err

    def test_tables_equal_the_font_files(self):
        compiled = dump(self.bin)
        self.assertEqual(differences(compiled, LATIN, EXTRA), [])
        self.assertEqual(len(compiled), 95 + len(parse(EXTRA)))
        self.assertTrue(all(0x20 <= c < 0x7F for c in parse(LATIN)))

    @unittest.skipIf(ENGLISH is None, NO_ENGLISH)
    def test_latin12_is_the_english_patch_font(self):
        self.assertEqual(LATIN.read_bytes(),
                         english_path('tools/patch/font/latin12.txt').read_bytes(),
                         'pc/font/latin12.txt differs from the English patch font: '
                         'copy tools/patch/font/latin12.txt over it')

    @unittest.skipIf(ENGLISH is None, NO_ENGLISH)
    def test_generator_parses_as_the_patch_does(self):
        sys.path.insert(0, str(GEN.parent))
        import gen_ui_font
        self.assertEqual(gen_ui_font.parse_latin(LATIN), fontgen().parse(LATIN))

    @unittest.skipIf(ENGLISH is None, NO_ENGLISH)
    def test_widths_equal_the_patch_measure(self):
        fg = fontgen()
        glyphs = fg.parse(LATIN)
        samples = ['SETTINGS', 'Status at a glance', "THE GAME'S FOLDER IS READ-ONLY",
                   'Tokimeki Memorial: Forever with You', '100% {braces} ~tilde~ |pipe|', '',
                   ' '.join(chr(c) for c in range(0x20, 0x7F))]
        r = subprocess.run([str(self.bin), 'width'], input='\n'.join(samples) + '\n',
                           capture_output=True, text=True, check=True)
        got = [int(x) for x in r.stdout.split()]
        self.assertEqual(got, [fg.text_width(glyphs, s) for s in samples])

    def test_unit_tests_pass(self):
        r = subprocess.run([str(self.bin), 'selftest'], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(r.stdout.count('PASS'), 5)


class NegativeControls(unittest.TestCase):
    def test_changed_pixel_is_caught(self):
        with tempfile.TemporaryDirectory() as d:
            text = LATIN.read_text(encoding='utf-8')
            head = 'glyph A adv 7\n......\n.####.\n'
            self.assertIn(head, text)
            bad = Path(d) / 'latin12.txt'
            bad.write_text(text.replace(head, 'glyph A adv 7\n......\n.###..\n'), encoding='utf-8')
            b, err = build(d, latin=bad)
            self.assertIsNotNone(b, err)
            self.assertEqual(differences(dump(b), LATIN, EXTRA), ['U+0041 rows differ'])

    def test_changed_advance_is_caught(self):
        with tempfile.TemporaryDirectory() as d:
            text = LATIN.read_text(encoding='utf-8')
            bad = Path(d) / 'latin12.txt'
            bad.write_text(text.replace('glyph M adv 8\n', 'glyph M adv 9\n'), encoding='utf-8')
            b, err = build(d, latin=bad)
            self.assertIsNotNone(b, err)
            self.assertEqual(differences(dump(b), LATIN, EXTRA), ['U+004D adv 9 != 8'])

    def test_extra_may_not_redefine_ascii(self):
        with tempfile.TemporaryDirectory() as d:
            bad = Path(d) / 'extra.txt'
            bad.write_text(EXTRA.read_text(encoding='utf-8') +
                           '\nglyph U+0041 adv 2\n' + '.\n' * 12, encoding='utf-8')
            b, err = build(d, extra=bad)
            self.assertIsNone(b)
            self.assertIn('latin12.txt owns it', err)

    def test_mutations_fail_the_unit_tests(self):
        text = SRC.read_text()
        for name, old, new in MUTATIONS:
            with self.subTest(name), tempfile.TemporaryDirectory() as d:
                self.assertIn(old, text, name)
                src = Path(d) / 'ui_font.c'
                src.write_text(text.replace(old, new, 1))
                b, err = build(d, src=src)
                self.assertIsNotNone(b, err)
                r = subprocess.run([str(b), 'selftest'], capture_output=True, text=True)
                self.assertNotEqual(r.returncode, 0, f'{name}: {r.stdout}')


if __name__ == '__main__':
    unittest.main()
