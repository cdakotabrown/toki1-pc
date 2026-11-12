# SPDX-License-Identifier: GPL-3.0-or-later
"""PC_INPUT `poke N TARGET HEX`: the seeded-start state write (pc/hal/trace.c).

Compiles the real pc/hal/trace.c with the real system/irq VBlank path and
checks that a `poke` item of an anchored script

  * writes its bytes N frames after the cursor (the last wait or press),
    before that VBlank's trace line -- the reference's `-poke FRAME:ADDR:HEX`
    is applied by its frame hook before TraceFrame, so the two traces show
    the seeded value on the same row;
  * follows a wait like a press does (anchored, not absolute);
  * prints the `(SEEDED RUN)` tag playthrough.py reports;
  * is refused when malformed or outside PS1 RAM (exit 2, nothing written);

and the negative control: the same script without the `poke` line never
writes.  No assets, no SDL.
"""
import os
from pathlib import Path
import tempfile
import unittest

from safe_process import run
from trace_compare import parse_trace

ROOT = Path(__file__).resolve().parents[2]

SOURCE = '#include "trace.c"\n' + r'''
u8 now_step, now_sub_step, now_sub_sub_step, now_girl, cd_state2, step_mode;
s32 frame_counter, step_frame_cnt, vblnk_counter, pad_data, pad_trig, pad_rel;
s32 activeBuff;
volatile int g_crash_count;
u8 screen_w[512];
u32 g_psx_rand_seed;
u8 fixture_flag;               /* found by dlsym, like a game global */
u8 fixture_gate;
u64 hal_clock_ticks(void) { return 123; }
void hal_bios_vblank_tick(void) { vblnk_counter++; }
void pad_update_buffers(void) { }
void hal_sdk_irq_vblank(void) { }
void hal_cpu_interrupt_occupancy(u32 ticks) { (void)ticks; }
void parity_anchor(void) { }
void parity_vblank(void) { }
extern void hal_vblank_interrupt(void);
int main(void) {
    int i;
    trace_init();
    for (i = 0; i < 12; i++) {
        frame_counter++;
        if (frame_counter == 5) fixture_gate = 1;
        hal_vblank_interrupt();
    }
    printf("end flag=%02x\n", fixture_flag);
    return 0;
}
'''


class SeedPokeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        cls.path = Path(cls.directory.name)
        (cls.path / 'test.c').write_text(SOURCE)
        cls.binary = cls.path / 'test'
        run(['gcc', '-std=gnu89', '-O2', '-ffunction-sections', '-fdata-sections',
             '-Wl,--gc-sections', '-Wl,--export-dynamic-symbol=fixture_flag',
             '-Wl,--export-dynamic-symbol=fixture_gate',
             '-I', str(ROOT / 'pc/hal'), '-I', str(ROOT / 'pc/include/game'),
             '-I', str(ROOT / 'pc/include'), str(cls.path / 'test.c'),
             str(ROOT / 'pc/hal/system.c'), str(ROOT / 'pc/hal/irq.c'),
             '-ldl', '-o', str(cls.binary)], timeout=30).require_ok()

    def launch(self, script, name):
        inp = self.path / (name + '.input')
        inp.write_text(script)
        trace = self.path / (name + '.trace')
        env = {k: v for k, v in os.environ.items()
               if not k.startswith('PC_') and k != 'SCENE_NAV'}
        env.update(PC_INPUT=str(inp), PC_TRACE=str(trace), PC_TRACE_PHASE='pre_irq',
                   PC_TRACE_SYMS='fixture_flag:1')
        result = run([str(self.binary)], env=env, timeout=5)
        rows = parse_trace(str(trace)) if trace.exists() else []
        return result, rows

    def flag_by_frame(self, rows):
        return {r['frame_counter']: r['fixture_flag'] for r in rows}

    def test_poke_after_wait_lands_before_that_trace_line(self):
        result, rows = self.launch(
            'hold 2\nwait gate fixture_gate:1=1\npoke 2 fixture_flag ab\n', 'after_wait')
        result.require_ok()
        flags = self.flag_by_frame(rows)
        # the wait holds at frame 5; the poke is 2 frames later, at 7, and
        # that frame's line already shows it (pre_irq, before the line)
        self.assertEqual(flags[6], 0)
        self.assertEqual(flags[7], 0xAB)
        self.assertEqual(flags[12], 0xAB)
        self.assertIn('PC_INPUT poke fixture_flag 1 bytes: game frame 7', result.stdout)
        self.assertIn('(SEEDED RUN)', result.stdout)
        self.assertIn('end flag=ab', result.stdout)

    def test_negative_control_without_poke_nothing_is_written(self):
        result, rows = self.launch('hold 2\nwait gate fixture_gate:1=1\n', 'control')
        result.require_ok()
        self.assertEqual(set(self.flag_by_frame(rows).values()), {0})
        self.assertNotIn('SEEDED RUN', result.stdout)
        self.assertIn('end flag=00', result.stdout)

    def test_rand_seed_address_writes_the_port_kernel_seed(self):
        # the kernel's rand seed is at 0x80009010 on the PS1 and in
        # g_psx_rand_seed on the port: the poke goes where the trace reads it
        result, rows = self.launch(
            'hold 2\nwait gate fixture_gate:1=1\npoke 1 0x80009010 45040000\n', 'seed')
        result.require_ok()
        seeds = {r['frame_counter']: r['rand_seed'] for r in rows}
        self.assertEqual(seeds[5], 0)
        self.assertEqual(seeds[6], 0x445)
        self.assertIn('PC_INPUT poke 0x80009010 4 bytes', result.stdout)

    def test_bad_pokes_are_refused(self):
        for line in ('poke 1 0x80009010 4504',        # half of the kernel seed
                     'poke 1 0x7FFFFFFF 01',          # below PS1 RAM
                     'poke 1 0x801FFFFF 0102',        # runs past its end
                     'poke 1 no_such_symbol 01',
                     'poke 1 frame_counter 01',       # a traced field: its exact size
                     'poke 1 fixture_flag 0',         # odd hex
                     'poke -1 fixture_flag 01',
                     'poke 1 fixture_flag'):
            with self.subTest(line=line):
                result, _ = self.launch('hold 2\n' + line + '\n', 'bad')
                self.assertEqual(result.returncode, 2)
                self.assertIn('bad poke line', result.stdout + result.stderr)
                self.assertNotIn('end flag', result.stdout)


if __name__ == '__main__':
    unittest.main()
