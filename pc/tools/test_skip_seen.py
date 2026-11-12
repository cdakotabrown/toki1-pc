# SPDX-License-Identifier: GPL-3.0-or-later
"""Skipping read dialogue ([enhanced] skip_seen; pc/hal/seen_text.c,
backlog_capture.c, enhance.c) on the real port, enhanced profile.

A page counts as read once the backlog capture took it (its arrow wait
started, or it was confirmed away during its voice); its TEXT hash goes into
the read-page file (TOKI_SEEN_FILE here, <saves>/seen_text.dat for a
player).  With skip_seen on and the fast-forward key held (TOKI_FF_HOLD=1
headless), the auto-advance's confirm press is made at the first poll of an
arrow wait on a page read before -- input only, like TOKI_TEXT_AUTO.

Shiori's phone call (date_save.txt up to the calendar):

* recording: a first run with the scripted presses writes the pages it read;
  the call's pages are among them.  Recording is read only: per frame, the
  VRAM / scanout / PS1 RAM hashes and per mixer call the mixed audio equal a
  run without it (the enhanced default with features off);
* skipping: the same call with the script's nine dialogue presses removed
  reaches the calendar with the read-page file, every press a skip press;
* negative controls: with an empty file (nothing read) or without the key
  held, the same input never reaches the calendar; with ONE of the call's
  pages missing from the file the skip stops at that page (only the pages
  before it are confirmed);
* a run that only records never presses (no [SEEN] confirm lines).

Needs the built port and pc/assets; skipped otherwise.  Six playthroughs of
the opening and the call (turbo), about a minute and a half each.
"""
import os
from pathlib import Path
import re
import shutil
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from safe_process import run  # noqa: E402
import game_platform  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
BINARY = game_platform.GAME   # PC_TEST_PLATFORM=windows: the .exe under Wine
ASSETS = ROOT / 'pc/assets'
PAGE = re.compile(r'\[SEEN\] page ([0-9a-f]{16}) read')
CONFIRM = re.compile(r'\[SEEN\] confirm on a read page \((\d+)\)')
PAGES_IN_CALL = 4         # pages of the call the capture takes (enhanced)


def call_script(path, presses=True):
    """date_save.txt up to the end of Shiori's phone call (the calendar);
    without PRESSES, the call's nine dialogue presses are left out."""
    lines, in_call = [], False
    for line in (ROOT / 'pc/tools/playthrough/date_save.txt').read_text().splitlines():
        if line.startswith('wait phonebook '):
            in_call = True
        if in_call and not presses and line.startswith('+120 circle'):
            continue
        lines.append(line)
        if line.startswith('wait calendar '):
            break
    lines.append('end 60')
    Path(path).write_text('\n'.join(lines) + '\n')


def write_seen(path, hashes):
    with open(path, 'wb') as fh:
        fh.write(b'TKSEEN01')
        for h in hashes:
            fh.write(struct.pack('<Q', int(h, 16)))


@unittest.skipUnless(BINARY.is_file() and (ASSETS / 'raw/PSX.EXE').is_file(),
                     'needs the built port and pc/assets')
class SkipSeen(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix='skip-seen-test-'))
        call_script(cls.tmp / 'call.txt')
        call_script(cls.tmp / 'call-nopress.txt', presses=False)
        cls.seen = cls.tmp / 'seen.dat'
        cls.record = cls.play('record', 'call.txt', {'TOKI_SEEN_FILE': cls.seen})
        cls.off = cls.play('off', 'call.txt', {})
        text = cls.record['text']
        cut = text.index('wait phonebook reached')
        cls.before = PAGE.findall(text[:cut])
        cls.call_pages = PAGE.findall(text[cut:])

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    @classmethod
    def play(cls, name, script, sets):
        out = cls.tmp / name
        out.mkdir()
        (out / 'saves').mkdir()
        sets = dict(PC_FRAME_HASH=out / 'frames.txt', PC_FRAME_HASH_RAM='game',
                    PC_AUDIO_HASH=out / 'audio.txt', TOKI_SAVE_DIR=out / 'saves', **sets)
        cmd = ['.venv/bin/python3' if (ROOT / '.venv/bin/python3').exists() else 'python3',
               'pc/tools/playthrough.py', str(cls.tmp / script), '--out', str(out / 'run'),
               '--bin', str(BINARY), '--profile', 'enhanced']
        for k, v in sets.items():
            cmd += ['--set', f'{k}={v}']
        r = run(cmd, cwd=ROOT, timeout=900, max_output=16 * 1024**2)
        log = out / 'run/date.pc.log'
        text = log.read_text(errors='replace') if log.exists() else ''
        return dict(result=r, text=text, dir=out,
                    reached='wait calendar reached' in text,
                    confirms=[int(n) for n in CONFIRM.findall(text)])

    def read(self, run, name):
        p = run['dir'] / name
        return p.read_text().splitlines() if p.exists() else []

    def test_1_recording_is_read_only(self):
        self.assertTrue(self.record['result'].ok, self.record['result'].stdout[-1500:])
        self.assertTrue(self.record['reached'])
        self.assertGreaterEqual(len(self.call_pages), PAGES_IN_CALL, self.call_pages)
        self.assertEqual(self.record['confirms'], [], 'recording alone never presses')
        data = self.seen.read_bytes()
        self.assertEqual(data[:8], b'TKSEEN01')
        self.assertEqual(len(data), 8 + 8 * (len(self.before) + len(self.call_pages)))
        self.assertNotIn('[SEEN]', self.off['text'])
        for name in ('frames.txt', 'audio.txt'):
            a, b = self.read(self.record, name), self.read(self.off, name)
            self.assertGreater(len(a), 1000, name)
            self.assertEqual(a, b, f'{name}: recording changed what the game did')

    def test_2_read_pages_are_skipped_to_the_calendar(self):
        seen = self.tmp / 'seen-copy.dat'
        shutil.copy(self.seen, seen)
        skip = self.play('skip', 'call-nopress.txt',
                         {'TOKI_SKIP_SEEN': '1', 'TOKI_FF_HOLD': '1', 'TOKI_SEEN_FILE': seen})
        self.assertTrue(skip['reached'], skip['text'][-2000:])
        # one press per page that waits on its arrow (the last page ends the
        # call by itself); the script needed nine presses, timed blind
        self.assertGreaterEqual(len(skip['confirms']), PAGES_IN_CALL - 1)
        # no new page: every page of the call was read before
        self.assertEqual(seen.read_bytes(), self.seen.read_bytes())

    def test_3_unread_pages_and_no_key_stop_the_skip(self):
        empty = self.play('empty', 'call-nopress.txt',
                          {'TOKI_SKIP_SEEN': '1', 'TOKI_FF_HOLD': '1',
                           'TOKI_SEEN_FILE': self.tmp / 'nothing.dat'})
        self.assertFalse(empty['reached'])
        self.assertEqual(empty['confirms'], [])
        nokey = self.tmp / 'seen-nokey.dat'
        shutil.copy(self.seen, nokey)
        held = self.play('nokey', 'call-nopress.txt',
                         {'TOKI_SKIP_SEEN': '1', 'TOKI_SEEN_FILE': nokey})
        self.assertFalse(held['reached'])
        self.assertEqual(held['confirms'], [])

    def test_4_the_skip_stops_at_the_first_unread_page(self):
        k = PAGES_IN_CALL - 2      # the third page (the call's first never waits on its arrow)
        partial = self.tmp / 'seen-partial.dat'
        missing = self.call_pages[k]
        write_seen(partial, [h for h in self.before + self.call_pages if h != missing])
        run_ = self.play('partial', 'call-nopress.txt',
                         {'TOKI_SKIP_SEEN': '1', 'TOKI_FF_HOLD': '1', 'TOKI_SEEN_FILE': partial})
        self.assertFalse(run_['reached'])
        # the pages before the unread one were confirmed, then the page was
        # read (recorded) and nothing more pressed
        self.assertGreaterEqual(len(run_['confirms']), 1)
        self.assertLess(len(run_['confirms']), PAGES_IN_CALL - 1)
        self.assertIn(missing, PAGE.findall(run_['text']))


if __name__ == '__main__':
    unittest.main()
