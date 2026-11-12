#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""test_translation_race.py -- several games started at once all get English.

Before 2026-11-02 two processes with language = en starting together raced
on pc/hal/translation.c's preparation of the English files: both wrote and renamed
the same "<file>.part", the loser logged "N failed (cannot write
.../OPTION.EXN)" and "[LANG] English unavailable; playing in Japanese" and
played in Japanese without a word on screen.  Now the preparation holds a
lock (<data>/en/.prepare.lock: flock / LockFileEx; removed when it is
done), writes a per-process
temporary file, and a failure is an on-screen message and an ERROR line.

The test starts N copies of the game at the same moment on a fresh English
tree (a temporary asset dir whose raw/ is the player's pc/assets/raw, so
its en/ starts empty) and requires every one to log
"[LANG] English" and no failed file.  Negative control: the same with
TOKI_TRANSLATION_NO_LOCK=1 (the old behaviour, a test hook in
translation.c) must show the race in at least one of a few rounds.

Needs pc/build/tokimeki-pc built with the translation pack and the player's
pc/assets/raw; skipped otherwise.  Linux.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pc_paths import ENGLISH  # noqa: E402
BIN = os.path.join(ROOT, 'pc', 'build', 'tokimeki-pc')
RAW = os.path.join(ROOT, 'pc', 'assets', 'raw')
PACK = os.path.join(ROOT, 'pc', 'build', 'translation', 'en.tktp')
N = 6


def start_all(n, extra_env=None):
    """n games at once on a fresh <tmp>/data (-> <tmp>/data/en); their logs and
    what they left in en/ (temporaries, the lock)"""
    top = tempfile.mkdtemp(prefix='tktp_race_', dir=os.path.join(ROOT, 'build'))
    try:
        assets = os.path.join(top, 'data')
        os.makedirs(assets)
        os.symlink(RAW, os.path.join(assets, 'raw'))
        procs = []
        for i in range(n):
            home = os.path.join(top, f'home{i}')
            os.makedirs(home)
            # headless: never a window on the desktop (no DISPLAY, dummy SDL)
            env = {k: v for k, v in os.environ.items()
                   if not k.startswith(('PC_', 'TOKI_', 'SDL_'))
                   and k not in ('DISPLAY', 'WAYLAND_DISPLAY')}
            env.update(HOME=home, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy',
                       TOKI_LANG='en', TOKI_TEST_QUIT_FRAMES='5', PC_TURBO='1')
            env.update(extra_env or {})
            log = open(os.path.join(top, f'log{i}.txt'), 'w')
            procs.append((subprocess.Popen([BIN, assets], cwd=home, env=env, stdout=log,
                                           stderr=subprocess.STDOUT), log))
        for p, log in procs:
            try:
                p.wait(timeout=300)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
            log.close()
        logs = []
        for i in range(n):
            with open(os.path.join(top, f'log{i}.txt'), encoding='utf-8', errors='replace') as f:
                logs.append(f.read())
        leftovers = [x for _, _, fs in os.walk(os.path.join(assets, 'en'))
                     for x in fs if x.endswith('.part') or x == '.prepare.lock']
        return logs, leftovers
    finally:
        shutil.rmtree(top, ignore_errors=True)


def english(log):
    return '[LANG] English (files from' in log and ' failed (' not in log


@unittest.skipIf(ENGLISH is None, 'toki1-english is absent (pc/tools/pc_paths.py): '
                 'a build without the English patch has no translation engine')
@unittest.skipUnless(sys.platform.startswith('linux') and os.path.exists(BIN)
                     and os.path.isdir(RAW) and os.path.exists(PACK),
                     'needs pc/build/tokimeki-pc with the pack and pc/assets/raw')
class TranslationRace(unittest.TestCase):
    def test_concurrent_starts_all_get_english(self):
        logs, leftovers = start_all(N)
        bad = [i for i, log in enumerate(logs) if not english(log)]
        self.assertEqual(bad, [], '\n'.join(
            line for i in bad for line in logs[i].splitlines()
            if '[TRANSLATION]' in line or '[LANG]' in line))
        # exactly one process wrote the files, the others found them ready
        self.assertEqual(sum(' 0 written' not in log for log in logs), 1)
        self.assertEqual(leftovers, [])

    def test_negative_control_without_the_lock_races(self):
        for _ in range(4):
            logs, _ = start_all(N, {'TOKI_TRANSLATION_NO_LOCK': '1'})
            if any(not english(log) for log in logs):
                failed = [log for log in logs if not english(log)][0]
                self.assertIn('[LANG] ERROR: English was requested', failed)
                return
        self.fail('the race did not reproduce without the lock in 4 rounds')


if __name__ == '__main__':
    unittest.main()
