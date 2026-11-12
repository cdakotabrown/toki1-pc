# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise actual HAL interrupt/observer order without game assets or SDL."""
import os
from pathlib import Path
import tempfile
import unittest

from safe_process import run
from trace_compare import parse_trace

ROOT = Path(__file__).resolve().parents[2]


class TracePhaseTests(unittest.TestCase):
    def test_observation_does_not_move_callback_capture_or_budget(self):
        source = '#include "trace.c"\n' + '''
u8 now_step, now_sub_step, now_sub_sub_step, now_girl, cd_state2, step_mode;
s32 frame_counter, step_frame_cnt, vblnk_counter, pad_data, pad_trig, pad_rel;
s32 activeBuff;
volatile int g_crash_count;
u8 screen_w[512];
u32 g_psx_rand_seed;
static int callbacks, polls, captures;
u64 hal_clock_ticks(void) { return 123; }
void hal_bios_vblank_tick(void) { callbacks++; vblnk_counter++; }
void pad_update_buffers(void) { polls++; pad_data = 0x4000; }
void hal_sdk_irq_vblank(void) { }
void hal_cpu_interrupt_occupancy(u32 ticks) { (void)ticks; }
void parity_anchor(void) { }
void parity_vblank(void) {
    captures++;
    if (callbacks != captures || polls != captures)
        _exit(3);
    printf("capture=%d counter=%d pad=%d\\n",
           captures, vblnk_counter, pad_data);
}
extern void hal_vblank_interrupt(void);
int main(void) {
    trace_init();
    frame_counter = 1;
    vblnk_counter = 56;
    hal_vblank_interrupt();
    /* Budget must exit only after the callback and capture, in either phase. */
    puts("budget-failed");
    return 1;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.c').write_text(source)
            run(['gcc', '-std=c89', '-O2', '-ffunction-sections',
                 '-fdata-sections', '-I', str(ROOT / 'pc/hal'),
                 '-I', str(ROOT / 'pc/include/game'), '-I', str(ROOT / 'pc/include'),
                 str(path / 'test.c'), str(ROOT / 'pc/hal/system.c'),
                 str(ROOT / 'pc/hal/irq.c'),
                 '-Wl,--gc-sections', '-ldl', '-o', str(path / 'test')],
                timeout=30).require_ok()
            env = {key: value for key, value in os.environ.items()
                   if not key.startswith('PC_') and key != 'SCENE_NAV'}
            env.update(PC_TRACE_FRAMES='1', PC_TRACE_CLOCK='vblank')
            cases = [(None, False), ('pre_irq', False), ('post_irq', False),
                     ('invalid', False), ('pre_irq', True), ('post_irq', True)]
            for phase, poke in cases:
                with self.subTest(phase=phase, poke=poke):
                    output = path / f'{phase}-{poke}.trace'
                    test_env = dict(env, PC_TRACE=str(output))
                    if phase is not None:
                        test_env['PC_TRACE_PHASE'] = phase
                    if poke:
                        test_env['PC_POKE'] = '1:vblnk_counter=00000000'
                    result = run([str(path / 'test')], env=test_env, timeout=5)
                    if phase == 'invalid':
                        self.assertEqual(result.returncode, 2)
                        self.assertFalse(output.exists())
                        continue
                    result.require_ok()
                    completed = (1 if phase == 'pre_irq' else 0) if poke else 57
                    self.assertIn(f'capture=1 counter={completed} pad=16384',
                                  result.stdout)
                    self.assertNotIn('budget-failed', result.stdout)
                    actual_phase = phase or 'post_irq'
                    self.assertTrue(output.read_text().startswith(
                        f'# observation={actual_phase}\n'))
                    rows = parse_trace(output)
                    self.assertEqual(len(rows), 1)
                    self.assertEqual(rows[0]['tick'], 123)
                    sampled = 0 if poke else (56 if phase == 'pre_irq' else 57)
                    self.assertEqual(rows[0]['vblnk_counter'], sampled)
                    self.assertEqual(rows[0]['pad'],
                                     0 if phase == 'pre_irq' else 0x4000)


if __name__ == '__main__':
    unittest.main()
