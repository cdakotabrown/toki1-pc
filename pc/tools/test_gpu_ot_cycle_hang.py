# SPDX-License-Identifier: GPL-3.0-or-later
"""A cyclic ordering table's DMA never ends (pc/hal/gpu_queue.c gpu_queue_hang).

Found by the seeded start taco-s1 (docs/pc-port/SEEDED-STARTS.md): on the
reference one TACO sub-step-10 frame lasted 242 VBlanks -- libgpu's DrawSync
waiting out its 240-VBlank alarm behind the GPU walking a cyclic OT -- while
the port, which draws the cycle once (gpu.c ot_cycle_entry), went on at once.

Compiles the real pc/hal/gpu_queue.c with a fake system clock (one VBlank
per 564,480 ticks, the NTSC frame) and the retail set_alarm (VSync(-1) +
240), and checks:
  * after gpu_queue_hang a DrawSync(0) (gpu_queue_sync(0)) returns -1 only
    when the alarm expires, 241 VBlanks later, and the device is idle
    after its reset;
  * a ResetGraph (gpu_queue_reset) before the DrawSync aborts the DMA;
  * negative control: GPU_OT_CYCLE_HANG=0 leaves the device idle (the old
    behaviour: DrawSync returns 0 at once).
No assets, no SDL.
"""
import os
from pathlib import Path
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]

SOURCE = r'''
#include "types.h"
#include <stdio.h>
#include <string.h>
#include "gpu_queue.h"
#define FRAME 564480ull
u8 graph_env[128];
s32 libgpu_alarm_deadline_vcount, libgpu_alarm_spin_count;
static unsigned long long clk;
unsigned long long hal_clock_ticks(void) { return clk; }
s32 hal_sdk_vcount(void) { return (s32)(clk / FRAME); }
void hal_cpu_spend(u32 t) { clk += t; }
void gpu_sdk_set_alarm(void) { libgpu_alarm_deadline_vcount = hal_sdk_vcount() + 240; libgpu_alarm_spin_count = 0; }
int main(int argc, char **argv) {
    s32 v0, r;
    gpu_queue_hang();
    if (argc > 1 && !strcmp(argv[1], "reset")) gpu_queue_reset(0);
    v0 = hal_sdk_vcount();
    r = gpu_queue_sync(0);
    printf("sync=%d vblanks=%d busy_after=%d\n", r, hal_sdk_vcount() - v0,
           gpu_queue_device_busy());
    return 0;
}
'''


class OtCycleHangTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        path = Path(cls.directory.name)
        (path / 'test.c').write_text(SOURCE)
        cls.binary = path / 'test'
        run(['gcc', '-std=gnu89', '-O1', '-I', str(ROOT / 'pc/hal'),
             '-I', str(ROOT / 'pc/include/game'), '-I', str(ROOT / 'pc/include'),
             str(path / 'test.c'), str(ROOT / 'pc/hal/gpu_queue.c'), '-o', str(cls.binary)],
            timeout=30).require_ok()

    def launch(self, *args, hang=None):
        env = {k: v for k, v in os.environ.items() if k != 'GPU_OT_CYCLE_HANG'}
        if hang is not None:
            env['GPU_OT_CYCLE_HANG'] = hang
        r = run([str(self.binary), *args], env=env, timeout=20)
        r.require_ok()
        return dict(kv.split('=') for kv in r.stdout.split())

    def test_drawsync_waits_out_the_libgpu_alarm(self):
        out = self.launch()
        self.assertEqual(out['sync'], '-1')
        self.assertEqual(out['vblanks'], '241')
        self.assertEqual(out['busy_after'], '0')

    def test_resetgraph_aborts_the_dma(self):
        out = self.launch('reset')
        self.assertEqual((out['sync'], out['vblanks']), ('0', '0'))

    def test_negative_control_no_hang(self):
        out = self.launch(hang='0')
        self.assertEqual((out['sync'], out['vblanks']), ('0', '0'))


if __name__ == '__main__':
    unittest.main()
