# SPDX-License-Identifier: GPL-3.0-or-later
"""File writes made during play run off the game thread (pc/hal/async_io.c).

Harness (pc/hal/async_io.c with SDL, jobs that sleep like a write held by an
on-access virus scan):
  * submitting ten 100 ms jobs returns at once; pc_async_flush waits for all
    of them; they run in submission order, on another thread;
  * back-pressure: jobs holding more than the 64 MB bound make the caller
    wait instead of queueing without limit;
  * negative control: TOKI_SYNC_WRITES=1 runs every job inline (the caller
    waits the full second).
The game (the selected build, PC_TEST_PLATFORM=windows for the exe under
Wine): an enhanced boot with the texture dump on writes the same files, with
the same bytes, and the same index, threaded and with TOKI_SYNC_WRITES=1;
the threaded run says so in its log, the inline one says it is inline.
"""
import hashlib
import os
from pathlib import Path
import shlex
import tempfile
import unittest

from safe_process import run
import game_platform

ROOT = Path(__file__).resolve().parents[2]
ASSETS = ROOT / 'pc/assets'
HARNESS = r'''
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pc_async_io.h"
static int order[64], n;
static SDL_threadID main_id, seen_id;
static void job(void *arg) {
    SDL_Delay(100);
    seen_id = SDL_ThreadID();
    order[n++] = (int)(long)arg;
}
static void big(void *arg) { SDL_Delay(50); (void)arg; }
int main(int argc, char **argv) {
    Uint32 t0, t_submit, t_flush;
    int i;
    main_id = SDL_ThreadID();
    if (argc > 1 && !strcmp(argv[1], "pressure")) {
        t0 = SDL_GetTicks();
        for (i = 0; i < 6; i++) pc_async_run(big, NULL, 30u << 20);   /* 180 MB */
        t_submit = SDL_GetTicks() - t0;
        pc_async_flush();
        printf("submit=%u\n", t_submit);
        return 0;
    }
    t0 = SDL_GetTicks();
    for (i = 0; i < 10; i++) pc_async_run(job, (void *)(long)i, 1000);
    t_submit = SDL_GetTicks() - t0;
    pc_async_flush();
    t_flush = SDL_GetTicks() - t0;
    printf("submit=%u flush=%u threaded=%d other_thread=%d order=", t_submit, t_flush,
           pc_async_threaded(), seen_id != main_id);
    for (i = 0; i < n; i++) printf("%d,", order[i]);
    printf("\n");
    return 0;
}
'''


def parse(out):
    line = [l for l in out.splitlines() if l.startswith('submit=')][-1]
    return dict(kv.split('=', 1) for kv in line.split())


class Harness(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        d = Path(cls.tmp.name)
        (d / 'h.c').write_text(HARNESS)
        flags = shlex.split(run(['pkg-config', '--cflags', '--libs', 'sdl2']).require_ok().stdout)
        cls.binary = d / 'h'
        run(['cc', '-std=gnu17', '-O1', '-I', str(ROOT / 'pc/include'), str(d / 'h.c'),
             str(ROOT / 'pc/hal/async_io.c'), *flags, '-o', str(cls.binary)],
            timeout=120).require_ok()

    def go(self, *args, **env):
        e = {k: v for k, v in os.environ.items() if k != 'TOKI_SYNC_WRITES'}
        e.update(env)
        return parse(run([str(self.binary), *args], env=e, timeout=60).require_ok().stdout)

    def test_threaded_submit_returns_at_once_and_flush_waits(self):
        r = self.go()
        self.assertEqual(r['threaded'], '1')
        self.assertEqual(r['other_thread'], '1')
        self.assertLess(int(r['submit']), 50)
        self.assertGreaterEqual(int(r['flush']), 1000)
        self.assertEqual(r['order'], ','.join(map(str, range(10))) + ',')

    def test_back_pressure(self):
        # six 30 MB jobs against the 64 MB bound: the caller waits for some
        self.assertGreaterEqual(int(self.go('pressure')['submit']), 100)

    def test_negative_control_sync_writes_inline(self):
        r = self.go(TOKI_SYNC_WRITES='1')
        self.assertEqual(r['threaded'], '0')
        self.assertEqual(r['other_thread'], '0')
        self.assertGreaterEqual(int(r['submit']), 1000)
        self.assertEqual(r['order'], ','.join(map(str, range(10))) + ',')


def tree(path):
    return {str(p.relative_to(path)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(Path(path).rglob('*')) if p.is_file()}


@unittest.skipUnless(game_platform.game_available() and (ASSETS / 'raw/PSX.EXE').is_file(),
                     'needs the built port and pc/assets')
class GameDumps(unittest.TestCase):
    def dump(self, name, **extra):
        work = Path(self.tmp.name) / name
        (work / 'pc').mkdir(parents=True)
        (work / 'pc/assets').symlink_to(ASSETS)
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(('PC_', 'TOKI_', 'SCENE', 'AUTOPLAY'))}
        env.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', PC_TURBO='1',
                   PC_TRACE=str(work / 'trace'), PC_TRACE_FRAMES='900',
                   TOKI_PROFILE='enhanced', TOKI_TEXTURE_DUMP='1',
                   TOKI_TEXTURE_DIR=str(work / 'textures'), TOKI_CONFIG_DIR=str(work / 'cfg'))
        env.update(extra)
        r = run([str(game_platform.GAME), str(ASSETS)], cwd=work, env=env, timeout=900,
                max_output=64 * 1024**2)
        self.assertTrue(r.ok, r.stdout[-2000:])
        return r.stdout, tree(work / 'textures'), (work / 'trace').read_bytes()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=ROOT / 'build')
        self.addCleanup(self.tmp.cleanup)

    def test_threaded_dump_equals_inline_dump(self):
        out_a, files_a, trace_a = self.dump('threaded')
        out_s, files_s, trace_s = self.dump('inline', TOKI_SYNC_WRITES='1')
        pngs = [f for f in files_a if f.endswith('.png')]
        self.assertGreater(len(pngs), 20)
        self.assertEqual(files_a, files_s)          # same files, same bytes, same index
        self.assertEqual(trace_a, trace_s)
        self.assertNotIn('TOKI_SYNC_WRITES', out_a)
        self.assertIn('[ASYNC] TOKI_SYNC_WRITES', out_s)     # control: really inline


if __name__ == '__main__':
    unittest.main()
