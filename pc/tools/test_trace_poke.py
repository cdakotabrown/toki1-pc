# SPDX-License-Identifier: GPL-3.0-or-later
"""PC_POKE test fixture: navigation-relative timing and symbol targets.

Compiles the real pc/hal/trace.c (with the real system/irq VBlank path) and
checks that `vN:SYMBOL=HEX` writes the global right after the parity
capture of the Nth VBlank after trace_anchor() -- the order of the
reference's `-poke <anchor+N>:ADDR:HEX`, which its frame hook applies after
ParityReference::OnFrame -- that it never fires without a navigation, that
the game-frame form still works, and that malformed entries are refused.
No assets, no SDL.
"""
import os
from pathlib import Path
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]

SOURCE = '#include "trace.c"\n' + r'''
u8 now_step, now_sub_step, now_sub_sub_step, now_girl, cd_state2, step_mode;
s32 frame_counter, step_frame_cnt, vblnk_counter, pad_data, pad_trig, pad_rel;
s32 activeBuff;
volatile int g_crash_count;
u8 screen_w[512];
u32 g_psx_rand_seed;
u8 fixture_flag;               /* found by dlsym, like class_flag */
u64 hal_clock_ticks(void) { return 123; }
void hal_bios_vblank_tick(void) { vblnk_counter++; }
void pad_update_buffers(void) { }
void hal_sdk_irq_vblank(void) { }
void hal_cpu_interrupt_occupancy(u32 ticks) { (void)ticks; }
static int captures;
void parity_anchor(void) { }
void parity_vblank(void) {
    captures++;
    printf("capture=%d flag=%02x\n", captures, fixture_flag);
}
extern void hal_vblank_interrupt(void);
int main(int argc, char **argv) {
    int i;
    trace_init();
    frame_counter = 5;
    hal_vblank_interrupt();            /* before the navigation */
    if (argc > 1) trace_anchor();
    for (i = 0; i < 3; i++) {
        frame_counter++;
        hal_vblank_interrupt();
    }
    printf("end flag=%02x\n", fixture_flag);
    return 0;
}
'''


class TracePokeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        path = Path(cls.directory.name)
        (path / 'test.c').write_text(SOURCE)
        cls.binary = path / 'test'
        # Export only the fixture global (the port links -rdynamic); the
        # unused HAL is garbage-collected as in test_trace_phase.py.
        run(['gcc', '-std=c89', '-O2', '-ffunction-sections', '-fdata-sections',
             '-Wl,--gc-sections', '-Wl,--export-dynamic-symbol=fixture_flag',
             '-I', str(ROOT / 'pc/hal'), '-I', str(ROOT / 'pc/include/game'),
             '-I', str(ROOT / 'pc/include'), str(path / 'test.c'),
             str(ROOT / 'pc/hal/system.c'), str(ROOT / 'pc/hal/irq.c'),
             '-ldl', '-o', str(cls.binary)], timeout=30).require_ok()

    def launch(self, poke, *args):
        env = {k: v for k, v in os.environ.items()
               if not k.startswith('PC_') and k != 'SCENE_NAV'}
        if poke is not None:
            env['PC_POKE'] = poke
        result = run([str(self.binary), *args], env=env, timeout=5)
        result.require_ok()
        return result.stdout

    def test_navigation_vblank_poke_follows_that_capture(self):
        out = self.launch('v2:fixture_flag=ab', 'nav')
        # capture 1 is the pre-navigation VBlank; 2 and 3 are navigation
        # VBlanks 1 and 2; the poke lands after capture 3 (VBlank 2).
        self.assertIn('capture=3 flag=00', out)
        self.assertIn('capture=4 flag=ab', out)
        self.assertIn('navigation VBlank 2 (test fixture)', out)
        self.assertIn('applied at navigation VBlank 2', out)

    def test_navigation_poke_never_fires_without_navigation(self):
        out = self.launch('v1:fixture_flag=01')
        self.assertIn('end flag=00', out)
        self.assertNotIn('applied', out)

    def test_negative_control_no_fixture_no_write(self):
        out = self.launch(None, 'nav')
        self.assertIn('end flag=00', out)
        self.assertNotIn('PC_POKE', out)

    def test_game_frame_form_reaches_symbols(self):
        out = self.launch('7:fixture_flag=05', 'nav')
        # frame_counter 6 at capture 2, 7 at capture 3 (written before it)
        self.assertIn('capture=2 flag=00', out)
        self.assertIn('capture=3 flag=05', out)

    def test_bad_entries_are_refused(self):
        for spec in ('v1:no_such_symbol=01', 'v1:frame_counter=01',
                     'v0:fixture_flag=01', 'v1:fixture_flag='):
            with self.subTest(spec=spec):
                result = run([str(self.binary), 'nav'],
                             env=dict(os.environ, PC_POKE=spec), timeout=5)
                result.require_ok()
                self.assertIn('bad entry', result.stdout + result.stderr)
                self.assertIn('end flag=00', result.stdout)


if __name__ == '__main__':
    unittest.main()
