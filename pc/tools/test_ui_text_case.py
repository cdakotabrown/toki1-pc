#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""test_ui_text_case.py -- the PC layer's own screens write sentence case
(docs/pc-port/ENHANCED-MODE.md "The PC layer's UI font and look", "Text
style").

    python3 pc/tools/test_ui_text_case.py

Scans the C string literals of the files that draw the PC layer's screens
(settings menu and its pages, setup screen, save browser, status page,
extras, backlog frame, notes, slot list) and fails on a word written in
capitals that is not an acronym, a key name, a cue-sheet keyword or one
of the nine parameter abbreviations (ALLOWED).  Lines that are logs
(printf / pc_logf), environment names, settings.ini keys, comparisons
(strcmp, strstr, memcmp) or comments are not drawn text and are skipped.

Negative controls: a copy of present.c with one label back in capitals
("SETTINGS") is caught, and so is every file of the tree before the change
(git HEAD~ of the change, via `git show`, when available).
"""
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
# the screens' sources (the audio track's music_pack.c and audio_soundtest.c
# are that track's, not reviewed here)
FILES = ['present.c', 'menu.c', 'save_browser.c', 'status_view.c', 'game_state_view.c',
         'extras_view.c', 'extras.c', 'backlog_view.c', 'backlog.c', 'setup_screen.c',
         'lang.c', 'assist_menu.c', 'quicksave.c', 'rewind.c', 'save_auto.c']
ALLOWED = set('''CRT 3D PNG SHA1 SHA-256 BIOS PS1 SDL CD MB KB GB OS OK PSP EBOOT PBP CHD ECM
ISO BIN CUE BIN/CUE MAME HUD UI XA FF PC MODE2/2352 FILE BINARY TRACK INDEX AUDIO
STA HUM SCI ART SPT TRV LKS GUT STR'''.split())
NOT_DRAWN = re.compile(r'(?<![sn])printf|pc_logf|debug_log|PC_DEBUG|getenv|setenv|"PC_[A-Z_]+"|'
                       r'"TOKI_|\[[A-Z_]+\] |hal_model_on|strcmp|strncmp|strstr\(|memcmp|'
                       r'^\s*/?\*|^\s*#|^\s*//')
LIT = re.compile(r'"((?:[^"\\]|\\.)*)"')
TOKEN = re.compile(r"[A-Za-z0-9'+./-]+")


def offences(text, name='?'):
    """[(line, word, literal)] for each capitals word in a drawn literal."""
    out = []
    depth = 0           # inside a log call that spans lines
    for n, line in enumerate(text.splitlines(), 1):
        code = LIT.sub('""', line)
        if depth > 0 or NOT_DRAWN.search(line):
            if depth == 0:
                code = code[NOT_DRAWN.search(line).start():]
            depth = max(0, depth + code.count('(') - code.count(')'))
            continue
        for m in LIT.finditer(line):
            if m.group(1) == 'S':           # an upper-case plural suffix ("%d save%s")
                out.append((f'{name}:{n}', 'S', 'S'))
                continue
            body = re.sub(r'\\x[0-9A-Fa-f]{2}|\\.|%[-0-9.]*[a-zA-Z]', ' ', m.group(1))
            for w in TOKEN.findall(body):
                w = w.strip("'.-/+")
                if re.search(r'[a-z]', w) or len(re.findall(r'[A-Z]', w)) < 2:
                    continue        # sentence case, a path, a single capital
                if w in ALLOWED or w.rstrip('S') in ALLOWED or re.fullmatch(r'F[0-9]+|[0-9]+X|[A-Z0-9_]*_[A-Z0-9_]*', w):
                    continue
                out.append((f'{name}:{n}', w, m.group(1)))
    return out


def tree_offences(get):
    found = []
    for f in FILES:
        text = get(f)
        if text is not None:
            found += offences(text, f)
    return found


class UiTextCase(unittest.TestCase):
    def test_tree_is_sentence_case(self):
        found = tree_offences(lambda f: (ROOT / 'pc/hal' / f).read_text(errors='replace'))
        self.assertEqual(found, [], '\n'.join(f'{a}: {w} in "{lit}"' for a, w, lit in found))

    def test_negative_control_one_label(self):
        text = (ROOT / 'pc/hal/present.c').read_text()
        self.assertIn('add_header("Display");', text)
        bad = text.replace('add_header("Display");', 'add_header("DISPLAY");')
        found = offences(bad, 'present.c')
        self.assertEqual([w for _, w, _ in found], ['DISPLAY'])

    def test_negative_control_before_the_change(self):
        """The tree before the sentence-case change is caught, file by file."""
        rev = subprocess.run(['git', 'log', '-1', '--format=%H', '--grep', '^pc ui: sentence case'],
                             cwd=ROOT, capture_output=True, text=True).stdout.strip()
        if not rev:
            self.skipTest('the sentence-case commit is not in this checkout')

        def old(f):
            r = subprocess.run(['git', 'show', f'{rev}~1:pc/hal/{f}'], cwd=ROOT,
                               capture_output=True, text=True)
            return r.stdout if r.returncode == 0 else None
        found = tree_offences(old)
        files = {a.split(':')[0] for a, _, _ in found}
        self.assertGreater(len(found), 300)
        self.assertGreaterEqual(len(files), 12, sorted(files))

    def test_allowed_words_pass(self):
        self.assertEqual(offences('x("CRT off");\ny("Ctrl+O   F5   SHA1 of 702 MB");\n'
                                  'z("STA HUM SCI");\nw("1x 2X 3D");\n'), [])
        self.assertEqual([w for _, w, _ in offences('x("PRESS ENTER");\n')], ['PRESS', 'ENTER'])
        self.assertEqual(offences('printf("[PRESENT] SAVED %s\\n", p);\n'), [])
        self.assertEqual([w for _, w, _ in offences('x("%d save%s", n, n == 1 ? "" : "S");\n')], ['S'])


if __name__ == '__main__':
    unittest.main()
