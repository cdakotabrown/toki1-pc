# SPDX-License-Identifier: GPL-3.0-or-later
"""The parity observer must capture the same display contract as SDL."""
import json
import os
from pathlib import Path
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pc_paths import decomp  # noqa: E402

HARNESS = r'''
#include <stdlib.h>
#include <string.h>
#include "parity.h"
#include "gpu_display.h"

u16 g_vram[512][1024];
static u16 original_vram[512][1024];
s32 g_disp_x, g_disp_y, g_disp_w = 2, g_disp_h = 1;
s32 g_disp_rgb24, g_disp_mask = 1;
s32 frame_counter, step_frame_cnt, vblnk_counter, activeBuff;
s32 pad_data, pad_trig, pad_rel;
u8 now_step, now_sub_step, now_sub_sub_step, now_girl, step_mode;

void gpu_get_scanout_state(GpuScanoutState *state) {
    state->x = g_disp_x;
    state->y = g_disp_y;
    state->width = g_disp_w;
    state->height = g_disp_h;
    state->rgb24 = g_disp_rgb24;
    state->enabled = g_disp_mask;
}

u32 hal_clock_vblanks(void) { return 1; }
u32 hal_clock_ticks_per_frame(void) { return 566204; }
void spu_parity_mix(s16 *samples, int frames) {
    memset(samples, 0, (size_t)frames * 2 * sizeof(s16));
}

int main(int argc, char **argv) {
    (void)argc;
    g_disp_x = 2;
    g_disp_y = 2;
    g_vram[2][2] = 0x001f;
    g_vram[2][3] = 0x03e0;
    if (!strcmp(argv[1], "origin")) {
        g_disp_x = g_disp_y = 1;
        g_vram[1][1] = 0x001f;
        g_vram[1][2] = 0x03e0;
    } else if (!strcmp(argv[1], "rgb24")) {
        g_disp_rgb24 = 1;
        g_vram[2][2] = 0x3412;
        g_vram[2][3] = 0x7856;
        g_vram[2][4] = 0xbc9a;
    } else if (!strcmp(argv[1], "disabled")) {
        g_disp_mask = 0;
    }
    memcpy(original_vram, g_vram, sizeof(g_vram));
    parity_anchor();
    parity_vblank();
    if (memcmp(original_vram, g_vram, sizeof(g_vram))) return 4;
    if (g_disp_x != (!strcmp(argv[1], "origin") ? 1 : 2) ||
        g_disp_y != (!strcmp(argv[1], "origin") ? 1 : 2)) return 3;
    return 0;
}
'''


class ParityScanoutTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        directory = Path(cls.directory.name)
        source = directory / 'observer.c'
        source.write_text(HARNESS)
        cls.binary = directory / 'observer'
        run(['gcc', '-std=c89', '-Wall', '-Wextra', '-Werror',
             '-I' + decomp('include'), '-Ipc/hal', str(source), 'pc/hal/parity.c',
             'pc/hal/gpu_scanout.c', '-o', str(cls.binary)],
            cwd=ROOT, timeout=30).require_ok()

    def capture(self, case):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'capture'
            env = dict(os.environ, PC_PARITY_DIR=str(output), PC_PARITY_FRAMES='1')
            run([str(self.binary), case], env=env, cwd=directory,
                timeout=10).require_ok()
            image = (output / 'frame_000001.ppm').read_bytes()
            metadata = json.loads((output / 'state.jsonl').read_text())
            return image.split(b'\n', 3)[3], metadata

    def test_bgr555_in_bounds_is_unchanged(self):
        pixels, _ = self.capture('bgr555')
        self.assertEqual(pixels, bytes((248, 0, 0, 0, 248, 0)))

    def test_origin_one_one_is_not_secretly_relocated(self):
        pixels, metadata = self.capture('origin')
        self.assertEqual(pixels, bytes((248, 0, 0, 0, 248, 0)))
        self.assertEqual(metadata['crop'], [1, 1, 2, 1])

    def test_rgb24_capture_preserves_packed_bytes(self):
        pixels, metadata = self.capture('rgb24')
        self.assertEqual(pixels, bytes((0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc)))
        self.assertEqual(metadata['display_rgb24'], 1)
        self.assertEqual(metadata['rgb_expansion'], 'packed8')
        self.assertEqual(metadata['vram_format'], 'u16le')

    def test_display_disabled_is_black_without_erasing_vram(self):
        pixels, metadata = self.capture('disabled')
        self.assertEqual(pixels, bytes(6))
        self.assertEqual(metadata['display_enabled'], 0)


if __name__ == '__main__':
    unittest.main()
