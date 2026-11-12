# SPDX-License-Identifier: GPL-3.0-or-later
"""The scene-music table is not a retail audio path and stays off by default.

pc/hal/audio_scene.c started SEQ files from a hand-written now_step table,
independent of the game's own sound-driver requests (retail scene 64 is
silent where it played ROMEO.SEQ; retail's title sequence is the one the
game's SD driver opens from RAM, not DAYS.SEQ).  It now runs only with
PC_SCENE_MUSIC=1.
"""
from pathlib import Path
import os
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(os.environ.get('AUDIO_SCENE_SOURCE', ROOT / 'pc/hal/audio_scene.c'))

FIXTURE = r'''
#include <stdio.h>
#include <string.h>
typedef unsigned char u8; typedef short s16;
u8 now_month;
static int seq_starts, xa_starts, stops;
static char last[256];
void audio_seq_stop(void) { stops++; }
void audio_seq_load_and_play(const char *p, int loop, s16 l, s16 r) {
    (void)loop; (void)l; (void)r; seq_starts++; strncpy(last, p, sizeof last - 1);
}
int audio_xa_start(const char *p) { (void)p; xa_starts++; return 1; }
void audio_xa_stop(void) {}
void debug_log(const char *f, ...) { (void)f; }
void audio_scene_tick(u8 scene);
int main(void) {
    audio_scene_tick(16); audio_scene_tick(18); audio_scene_tick(64);
    audio_scene_tick(0); audio_scene_tick(1);
    printf("%d %d %d %s\n", seq_starts, xa_starts, stops, last);
    return 0;
}
'''


def run_fixture(env_value):
    with tempfile.TemporaryDirectory() as d:
        p = Path(d)
        (p / 'fixture.c').write_text(FIXTURE)
        run(['gcc', '-std=gnu17', '-w', '-O1', '-I', str(ROOT / 'pc/include/game'), '-I', str(ROOT / 'pc/include'),
             '-include', str(ROOT / 'pc/include/game/types.h'), str(p / 'fixture.c'), str(SOURCE),
             '-o', str(p / 'fixture')]).require_ok()
        env = {k: v for k, v in os.environ.items() if k != 'PC_SCENE_MUSIC'}
        if env_value is not None:
            env['PC_SCENE_MUSIC'] = env_value
        out = run([str(p / 'fixture')], env=env)
        out.require_ok()
        return out.stdout.split()


class AudioSceneDefaultTests(unittest.TestCase):
    def test_default_build_starts_no_table_music(self):
        seq, xa, stops, *_ = run_fixture(None)
        self.assertEqual((seq, xa, stops), ('0', '0', '0'))

    def test_zero_keeps_it_off(self):
        self.assertEqual(run_fixture('0')[:3], ['0', '0', '0'])

    def test_opt_in_still_routes_the_table(self):
        seq, xa, stops, last = run_fixture('1')
        self.assertGreater(int(seq), 0)
        self.assertTrue(last.endswith('VISUAL.SEQ'), last)


if __name__ == '__main__':
    unittest.main()
