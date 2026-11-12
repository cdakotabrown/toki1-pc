# SPDX-License-Identifier: GPL-3.0-or-later
"""Raw GP0(02h) fill is neither a drawing-mode word nor an SDK image copy."""
import ctypes
import os
from pathlib import Path
import shlex
import struct
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
PIXELS = 1024 * 512
FIXTURE = r'''
#define main gpu_ot_fixture_main
#include "@FIXTURE@"
#undef main
int main(int argc, char **argv) {
    u32 words[4];
    int x, y, state_case, short_case;
    u16 expected;

    (void)argc;
    state_case = !strcmp(argv[1], "state");
    short_case = !strcmp(argv[1], "short");
    for (y = 0; y < 512; y++)
        for (x = 0; x < 1024; x++) g_vram[y][x] = 0x8001;
    g_tpage_x = 14; g_tpage_y = 1; g_tpage_bpp = 2; g_tpage_abr = 3;
    g_mask_set_or = g_mask_eval_and = 0x8000;
    words[0] = (short_case ? 2U : 3U) << 24;
    words[1] = 0x020000F8U;
    words[2] = (3U << 16) | 30U;
    words[3] = (2U << 16) | 17U;
    pc_ot_setaddr(words, (void *)(uintptr_t)0xFFFFFFU);
    DrawOTag(words);
    if (state_case || short_case) {
        if (g_tpage_x != 14 || g_tpage_y != 1 || g_tpage_bpp != 2 ||
            g_tpage_abr != 3 || g_mask_set_or != 0x8000 ||
            g_mask_eval_and != 0x8000) return 1;
    }
    for (y = 0; y < 512; y++) {
        for (x = 0; x < 1024; x++) {
            expected = !short_case && x >= 16 && x < 48 &&
                       y >= 3 && y < 5 ? 0x001F : 0x8001;
            if (g_vram[y][x] != expected) return 2;
        }
    }
    return 0;
}
'''


class FillHelperTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        library = Path(cls.directory.name) / 'fill.so'
        run(['cc', '-std=c89', '-pedantic-errors', '-Wall', '-Wextra',
             '-Werror', '-O1', '-fsanitize=undefined',
             '-fno-sanitize-recover=undefined', '-shared', '-fPIC',
             'pc/hal/gpu_vram.c', '-o', str(library)],
            cwd=ROOT, timeout=30).require_ok()
        cls.library = ctypes.CDLL(str(library))
        cls.fill = cls.library.gpu_vram_fill
        cls.fill.argtypes = [ctypes.POINTER(ctypes.c_ushort), ctypes.c_uint,
                             ctypes.c_uint, ctypes.c_uint]
        cls.fill.restype = None

    def check_fill(self, color, position, size, xs, ys, pixel):
        memory = (ctypes.c_ushort * (PIXELS + 2))()
        ctypes.memset(memory, 0xA5, ctypes.sizeof(memory))
        expected = bytearray(bytes(memory))
        for y in ys:
            for x in xs:
                struct.pack_into('<H', expected, (1 + y * 1024 + x) * 2, pixel)
        data = ctypes.cast(ctypes.byref(memory, 2), ctypes.POINTER(ctypes.c_ushort))
        self.fill(data, color, position, size)
        self.assertEqual(bytes(memory), expected)

    def test_color_truncation_and_cleared_mask_bit(self):
        for color, pixel in [(0xFFFFFF, 0x7FFF), (0x070707, 0),
                             (0x080808, 0x0421), (0x02AABBCC, 0x56F9)]:
            with self.subTest(color=color):
                self.check_fill(color, 0, 0x00010001, range(16), [0], pixel)

    def test_x_alignment_and_width_rounding(self):
        self.check_fill(0xF8, (3 << 16) | 30, (2 << 16) | 17,
                        range(16, 48), [3, 4], 31)

    def test_independent_axis_wrap(self):
        self.check_fill(0xF800, (511 << 16) | 1006, (3 << 16) | 33,
                        [*range(992, 1024), *range(16)], [511, 0, 1], 992)

    def test_zero_and_masked_zero_are_empty_not_full_extent(self):
        for size in (0, 1 << 16, 1, (1 << 16) | 1024, (512 << 16) | 16):
            with self.subTest(size=size):
                self.check_fill(0xFFFFFF, 0, size, [], [], 0)

    def test_sizes_mask_before_rounding_not_saturation(self):
        self.check_fill(0xF8, (0x9FF << 16) | 0x17D1, (513 << 16) | 1025,
                        range(976, 992), [511], 31)

    def test_width_1023_rounds_up_to_full_row(self):
        self.check_fill(0xF80000, 0, (1 << 16) | 1023,
                        range(1024), [0], 31744)


class FillPacketTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        directory = Path(cls.directory.name)
        source = directory / 'fill.c'
        source.write_text(FIXTURE.replace('@FIXTURE@',
                          str(ROOT / 'pc/tests/gpu_ot_harness.c')))
        cls.binary = directory / 'fill'
        gpu = Path(os.environ.get('GPU_SOURCE', ROOT / 'pc/hal/gpu.c')).resolve()
        flags = shlex.split(run(['pkg-config', '--cflags', 'sdl2'],
                                timeout=10).require_ok().stdout)
        run(['cc', '-std=gnu17', '-D_GNU_SOURCE', '-fpermissive', '-O1',
             '-ffunction-sections', '-fdata-sections',
             '-DGPU_SOURCE="' + str(gpu) + '"', '-Ipc/include',
             '-Ipc/include/game', '-Ipc/hal', str(source),
             'pc/hal/gpu_texture.c', 'pc/hal/gpu_vram.c', 'pc/hal/gpu_queue.c',
             '-Wl,--gc-sections', '-o', str(cls.binary), *flags],
            cwd=ROOT, timeout=30).require_ok()

    def check_case(self, case):
        run([str(self.binary), case], cwd=ROOT,
            env=dict(os.environ, MDFN_GPU='0', GP0_TRACE='0'),
            timeout=10).require_ok()

    def test_fill_packet_writes_aligned_wrapped_geometry_ignoring_e6(self):
        self.check_case('fill')

    def test_fill_does_not_change_texture_page_or_mask_registers(self):
        self.check_case('state')

    def test_truncated_fill_is_rejected_without_vram_or_state_changes(self):
        self.check_case('short')


if __name__ == '__main__':
    unittest.main()
