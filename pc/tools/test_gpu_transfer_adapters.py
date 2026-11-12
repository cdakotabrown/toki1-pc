# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise real SDK image adapters, not just their pure raw-GP0 helpers."""
import os
from pathlib import Path
import shlex
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#include "@GPU_SOURCE@"
u8 graph_env[128];

/* Native backend only: the transfers' cache events must not reach it. */
void mdfn_clear_cache(void) { abort(); }
void mdfn_invalidate_tex_cache(void) { abort(); }

void nbn_trace_load_image(const void *data, u32 length,
                         s32 x, s32 y, s32 width, s32 height) {
    (void)data; (void)length; (void)x; (void)y;
    (void)width; (void)height;
}

static u16 expected[512][1024];
static union { u32 alignment; u16 pixels[1024]; } source;
static union { u32 alignment; u16 pixels[8]; } output;

int main(int argc, char **argv) {
    RECT rect;
    int i;

    (void)argc;
    rect.x = 1023; rect.y = 511; rect.w = 2; rect.h = 2;
    source.pixels[0] = 11; source.pixels[1] = 22;
    source.pixels[2] = 33; source.pixels[3] = 44;
    if (!strcmp(argv[1], "wrap")) {
        expected[511][1023] = 11; expected[511][0] = 22;
        expected[0][1023] = 33; expected[0][0] = 44;
        LoadImage(&rect, &source.alignment);
    } else if (!strcmp(argv[1], "mask")) {
        rect.x = rect.y = 0; rect.w = 2; rect.h = 1;
        g_mask_set_or = g_mask_eval_and = 0x8000;
        g_vram[0][0] = expected[0][0] = 0x8123;
        expected[0][1] = 0x8000 | 22;
        LoadImage(&rect, &source.alignment);
    } else if (!strcmp(argv[1], "clamp")) {
        rect.x = rect.y = 0; rect.w = 1025; rect.h = 1;
        for (i = 0; i < 1024; i++) {
            source.pixels[i] = expected[0][i] = (u16)(i + 1);
        }
        LoadImage(&rect, &source.alignment);
    } else if (!strcmp(argv[1], "store")) {
        rect.x = rect.y = -1;
        g_vram[511][1023] = expected[511][1023] = 11;
        g_vram[511][0] = expected[511][0] = 22;
        g_vram[0][1023] = expected[0][1023] = 33;
        g_vram[0][0] = expected[0][0] = 44;
        for (i = 0; i < 8; i++) output.pixels[i] = 0xcafe;
        StoreImage(&rect, (u32 *)&output.pixels[2]);
        for (i = 0; i < 4; i++) {
            if (output.pixels[i + 2] != source.pixels[i]) return 2;
        }
        if (output.pixels[0] != 0xcafe || output.pixels[1] != 0xcafe ||
            output.pixels[6] != 0xcafe || output.pixels[7] != 0xcafe) return 3;
    } else {
        rect.x = rect.y = 0; rect.w = 0; rect.h = 1;
        g_vram[0][0] = expected[0][0] = 99;
        LoadImage(&rect, &source.alignment);
        rect.w = -1;
        LoadImage(&rect, &source.alignment);
    }
    return memcmp(g_vram, expected, sizeof(g_vram)) ? 1 : 0;
}
'''


class TransferAdapterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        directory = Path(cls.directory.name)
        gpu = Path(os.environ.get('GPU_ADAPTER_SOURCE', ROOT / 'pc/hal/gpu.c'))
        source = directory / 'adapters.c'
        source.write_text(HARNESS.replace('@GPU_SOURCE@', str(gpu.resolve())))
        cls.binary = directory / 'adapters'
        flags = shlex.split(run(['pkg-config', '--cflags', 'sdl2'],
                                timeout=10).require_ok().stdout)
        run(['cc', '-std=gnu17', '-D_GNU_SOURCE', '-fpermissive', '-O1',
             '-fsanitize=undefined', '-fno-sanitize-recover=undefined',
             '-ffunction-sections', '-fdata-sections', '-Ipc/include',
             '-Ipc/include/game', '-Ipc/hal', str(source),
             'pc/hal/gpu_vram.c', 'pc/hal/gpu_queue.c', 'pc/hal/ptr_validate.c',
             '-Wl,--gc-sections', '-o', str(cls.binary), *flags],
            cwd=ROOT, timeout=30).require_ok()

    def check_case(self, case):
        run([str(self.binary), case], timeout=10).require_ok()

    def test_upload_wraps_both_axes_with_full_source_stride(self):
        self.check_case('wrap')

    def test_upload_obeys_current_e6_mask_flags(self):
        self.check_case('mask')

    def test_sdk_upload_clamps_oversize_instead_of_rejecting(self):
        self.check_case('clamp')

    def test_readback_masks_negative_origin_and_keeps_output_guards(self):
        self.check_case('store')

    def test_sdk_empty_and_negative_sizes_do_not_become_raw_full_extent(self):
        self.check_case('empty')


if __name__ == '__main__':
    unittest.main()
