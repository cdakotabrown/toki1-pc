# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise actual trace.c budgets without SDL, assets or game code."""
import os
from pathlib import Path
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]


class TraceBudgetTests(unittest.TestCase):
    def test_explicit_clocks_and_post_navigation_anchor(self):
        source = f'#include "{ROOT}/pc/hal/trace.c"\n' + '''
u8 now_step, now_sub_step, now_sub_sub_step, now_girl, cd_state2, step_mode;
s32 frame_counter, step_frame_cnt, vblnk_counter, pad_data, pad_trig, pad_rel;
s32 activeBuff;
volatile int g_crash_count;
u8 screen_w[512];
u32 g_psx_rand_seed;
u64 hal_clock_ticks(void) { return 0; }
/* Capture is independently exercised by test_parity_capture.py. */
void parity_anchor(void) { }
void parity_vblank(void) { }
int main(void) {
    int i;
    trace_init();
    frame_counter = 100;
    /* The boot must not spend the post-navigation budget. */
    for (i = 0; i < 8; i++) trace_vblank();
    puts("boot-complete");
    trace_anchor();
    trace_vblank();
    trace_vblank();
    puts("two-vblanks");
    trace_vblank();
    puts("game-clock-still-waiting");
    frame_counter += 3;
    trace_vblank();
    puts("budget-failed");
    return 1;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.c').write_text(source)
            run(['gcc', '-std=c89', '-O2', '-I', str(ROOT / 'pc/include/game'),
                 str(path / 'test.c'), '-ldl', '-o', str(path / 'test')]).require_ok()
            env = {key: value for key, value in os.environ.items()
                   if not key.startswith('PC_')}
            env.update(PC_TRACE_FRAMES='3', SCENE_NAV='208')
            for clock in (None, 'game', 'vblank', 'invalid'):
                with self.subTest(clock=clock):
                    test_env = dict(env)
                    if clock is not None:
                        test_env['PC_TRACE_CLOCK'] = clock
                    result = run([str(path / 'test')], env=test_env)
                    if clock == 'invalid':
                        self.assertEqual(result.returncode, 2)
                        self.assertNotIn('boot-complete', result.stdout)
                        continue
                    result.require_ok()
                    self.assertIn('boot-complete', result.stdout)
                    self.assertIn('two-vblanks', result.stdout)
                    self.assertNotIn('budget-failed', result.stdout)
                    if clock == 'vblank':
                        self.assertNotIn('game-clock-still-waiting', result.stdout)
                        self.assertIn('clock=vblank, frame_counter=100', result.stdout)
                    else:
                        self.assertIn('game-clock-still-waiting', result.stdout)
                        self.assertIn('clock=game, frame_counter=103', result.stdout)


if __name__ == '__main__':
    unittest.main()
