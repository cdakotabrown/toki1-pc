# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic texture contract tests; no assets and no hardware-parity claim.

Intentionally not executed during the preservation job. Later, run under
build/gpu-contracts-20261021/worker-tests.lock via safe_process.py.
GPU_SOURCE=/absolute/path/to/old-gpu.c selects a real old translation unit
for negative controls. No sampler source is copied or textually extracted.
The pure helper uses strict C89; the real legacy gpu.c uses its existing
GNU compatibility flags and requires SDL2 development headers, not a window.
"""
import ctypes as C
import os
from pathlib import Path
import shlex
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
Vram = C.c_ushort * (1024 * 512)


def clut_id(x, y):
    return (y << 6) | (x >> 4)


def tpage_id(x, y, depth):
    return (x >> 6) | ((y >> 8) << 4) | (depth << 7)


class TextureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        library = Path(cls.directory.name) / 'texture.so'
        run(['cc', '-std=c89', '-pedantic', '-Wall', '-Wextra', '-Werror',
             '-O1', '-fPIC', '-shared', '-fsanitize=undefined',
             '-fno-sanitize-recover=undefined', 'pc/hal/gpu_texture.c',
             '-o', str(library)], cwd=ROOT, timeout=30).require_ok()
        cls.library = C.CDLL(str(library))
        cls.sample = cls.library.gpu_texture_sample
        cls.sample.argtypes = [C.POINTER(C.c_ushort), C.c_ubyte, C.c_ubyte,
                                C.c_ushort, C.c_ushort]
        cls.sample.restype = C.c_ushort

    def setUp(self):
        self.vram = Vram()

    def test_literal_clut_rows_and_nonzero_x(self):
        for y in (0, 1, 239, 479, 480, 481, 485, 511):
            for x in (0, 16, 64, 1008):
                with self.subTest(x=x, y=y):
                    self.vram[256 * 1024 + 128] = 0x1111
                    self.vram[y * 1024 + x + 1] = 0x9234
                    before = bytes(self.vram)
                    self.assertEqual(self.sample(self.vram, 0, 0,
                                                 clut_id(x, y), 0x12),
                                     0x9234)
                    self.assertEqual(bytes(self.vram), before)

    def test_all_four_nibbles_and_all_16_indices(self):
        for index in range(16):
            self.vram[490 * 1024 + index] = 0x8100 + index
        for u in range(256):
            base = u & ~3
            self.vram[16 * 1024 + 128 + u // 4] = sum(
                ((base + lane) & 15) << (lane * 4) for lane in range(4))
        for u in range(256):
            self.assertEqual(self.sample(self.vram, u, 16,
                                         clut_id(0, 490), 2),
                             0x8100 + (u & 15))

    def test_both_bytes_and_all_256_indices(self):
        for index in range(256):
            self.vram[490 * 1024 + index] = 0x8100 + index
        for word in range(128):
            self.vram[16 * 1024 + 128 + word] = 2 * word | (
                (2 * word + 1) << 8)
        for u in range(256):
            self.assertEqual(self.sample(self.vram, u, 16,
                                         clut_id(0, 490), 0x82),
                             0x8100 + u)

    def test_index_zero_reads_color_and_actual_zero_remains_zero(self):
        for depth in (0, 1):
            page = tpage_id(128, 0, depth)
            self.vram[490 * 1024] = 0x4567
            self.assertEqual(self.sample(self.vram, 0, 0,
                                         clut_id(0, 490), page), 0x4567)
            self.vram[490 * 1024] = 0
            self.assertEqual(self.sample(self.vram, 0, 0,
                                         clut_id(0, 490), page), 0)
            self.vram[490 * 1024] = 0x8000
            self.assertEqual(self.sample(self.vram, 0, 0,
                                         clut_id(0, 490), page), 0x8000)
            self.vram[128] = 1
            self.vram[490 * 1024 + 1] = 0
            self.assertEqual(self.sample(self.vram, 0, 0,
                                         clut_id(0, 490), page), 0)
            self.vram[128] = 0

    def test_direct_color_literal_origin_bit15_and_ignored_clut(self):
        for x, y in ((0, 0), (64, 256), (768, 256)):
            for u, v, color in ((0, 0, 0), (1, 2, 0x8000),
                                 (255, 255, 0xFFFF)):
                with self.subTest(x=x, y=y, u=u, v=v):
                    self.vram[(y + v) * 1024 + x + u] = color
                    page = tpage_id(x, y, 2)
                    self.assertEqual(self.sample(self.vram, u, v, 0, page),
                                     color)
                    self.assertEqual(self.sample(self.vram, u, v, 65535,
                                                 page), color)

    def test_commanded_depth_selects_same_word_without_metadata(self):
        self.vram[16 * 1024 + 128] = 0x4321
        for index in range(256):
            self.vram[490 * 1024 + index] = 0x100 + index
        palette = clut_id(0, 490)
        self.assertEqual(self.sample(self.vram, 1, 16, palette, 2), 0x102)
        self.assertEqual(self.sample(self.vram, 1, 16, palette, 0x82), 0x143)
        self.assertEqual(self.sample(self.vram, 0, 16, palette, 0x102), 0x4321)

    def test_irrelevant_tpage_bits_and_clut_bit15_do_not_relocate(self):
        self.vram[16 * 1024 + 128] = 1
        self.vram[490 * 1024 + 33] = 0x3456
        palette = clut_id(32, 490)
        for bits in (0, 0x20, 0x40, 0xFE60):
            self.assertEqual(self.sample(self.vram, 0, 16, palette | 0x8000,
                                         2 | bits), 0x3456)

    # VRAM addressing wraps at the right edge to X & 1023 of the SAME row:
    # PSX-SPX "Wrapping" (no carry from X into Y); Mednafen GetTexel /
    # Update_CLUT_Cache and the reference emulator's % VRAM_WIDTH do the same.
    def test_texture_word_wraps_to_column_zero_of_the_same_row(self):
        self.vram[0] = 0x1234
        self.vram[1023] = 0x5678
        self.vram[1024] = 0x7777          # row 1, X 0: a carry would read it
        self.assertEqual(self.sample(self.vram, 63, 0, 0, 0x10F), 0x5678)
        self.assertEqual(self.sample(self.vram, 64, 0, 0, 0x10F), 0x1234)
        self.vram[191] = 0x2345
        self.assertEqual(self.sample(self.vram, 255, 0, 0, 0x10F), 0x2345)
        # 8-bit page 15: u 128 is word X 1024 -> X 0, low byte 0x34.
        for index in range(256):
            self.vram[490 * 1024 + index] = 0x0100 + index
        self.assertEqual(self.sample(self.vram, 128, 0, clut_id(0, 490),
                                     0x8F), 0x0134)
        self.assertEqual(self.sample(self.vram, 129, 0, clut_id(0, 490),
                                     0x8F), 0x0112)

    def test_clut_entry_wraps_to_column_zero_of_the_same_row(self):
        self.vram[128] = 0x100F
        self.vram[490 * 1024 + 1023] = 0x4567
        self.vram[490 * 1024] = 0x6543
        self.vram[491 * 1024] = 0x7654     # a carry into Y would read this
        palette = clut_id(1008, 490)
        self.assertEqual(self.sample(self.vram, 0, 0, palette, 0x82), 0x4567)
        self.assertEqual(self.sample(self.vram, 1, 0, palette, 0x82), 0x6543)

    def test_reserved_depth_and_null_vram_are_explicitly_unsupported(self):
        self.vram[0] = 0x1234
        self.assertEqual(self.sample(self.vram, 0, 0, 0, 0x180), 0)
        self.assertEqual(self.sample(None, 0, 0, 0, 0x100), 0)


# Include the selected REAL GPU translation unit, exposing its diagnostics
# and actual registration bookkeeping. Link-time GC drops unrelated devices.
HARNESS = r'''
#include GPU_SOURCE
#include <stdio.h>
#include <string.h>

static int check(unsigned char u, unsigned char v, unsigned short clut,
                 unsigned short page, unsigned short expected)
{
    unsigned short actual = texel_fetch_debug(u, v, clut, page);
    if (actual == expected) return 0;
    fprintf(stderr, "u=%u v=%u clut=%04x page=%04x: %04x != %04x\n",
            u, v, clut, page, actual, expected);
    return 1;
}

int main(int argc, char **argv)
{
    int i;
    unsigned short clut = (unsigned short)(490 << 6);
    if (argc != 2) return 2;
    memset(g_vram, 0, sizeof(g_vram));
    for (i = 0; i < 256; i++) g_vram[490][i] = 0x100 + i;
    if (!strcmp(argv[1], "low-clut")) {
        g_vram[16][128] = 1;
        g_vram[10][65] = 0x1234;
        g_vram[481][65] = 0x5678;
        return check(0, 16, (10 << 6) | 4, 2, 0x1234);
    }
    if (!strcmp(argv[1], "row480-clut")) {
        g_vram[16][128] = 1;
        g_vram[480][65] = 0x1234;
        g_vram[480][1] = 0x5678;
        return check(0, 16, (480 << 6) | 4, 2, 0x1234);
    }
    if (!strcmp(argv[1], "literal-origin")) {
        gpu_set_region_bpp(0, 1);
        g_vram[0][0] = 1;
        g_vram[256][0] = 2;
        return check(0, 0, clut, 0x80, 0x101);
    }
    if (!strcmp(argv[1], "registered4")) {
        gpu_set_region_bpp(2, 1);
        g_vram[16][128] = 0x4321;
        return check(1, 16, clut, 2, 0x102);
    }
    if (!strcmp(argv[1], "registered8")) {
        gpu_set_region_bpp(2, 0);
        g_vram[16][128] = 0x0201;
        return check(1, 16, clut, 0x82, 0x102);
    }
    if (!strcmp(argv[1], "registered15")) {
        gpu_set_region_bpp(2, 0);
        g_vram[16][128] = 0x5432;
        return check(0, 16, clut, 0x102, 0x5432);
    }
    if (!strcmp(argv[1], "index-zero")) {
        if (check(0, 16, clut, 2, 0x100)) return 1;
        g_vram[490][0] = 0;
        if (check(0, 16, clut, 2, 0)) return 1;
        g_vram[490][0] = 0x8000;
        return check(0, 16, clut, 2, 0x8000);
    }
    if (!strcmp(argv[1], "all4")) {
        for (i = 0; i < 64; i++) {
            unsigned int n = (i * 4) & 15;
            g_vram[16][128 + i] = n | ((n + 1) << 4) |
                                   ((n + 2) << 8) | ((n + 3) << 12);
        }
        for (i = 0; i < 256; i++)
            if (check(i, 16, clut, 2, 0x100 + (i & 15))) return 1;
        return 0;
    }
    if (!strcmp(argv[1], "all8")) {
        for (i = 0; i < 128; i++)
            g_vram[16][128 + i] = 2 * i | ((2 * i + 1) << 8);
        for (i = 0; i < 256; i++)
            if (check(i, 16, clut, 0x82, 0x100 + i)) return 1;
        return 0;
    }
    if (!strcmp(argv[1], "all15")) {
        for (i = 0; i < 256; i++) g_vram[16][128 + i] = 0x8000 + i;
        for (i = 0; i < 256; i++)
            if (check(i, 16, 0, 0x102, 0x8000 + i)) return 1;
        return 0;
    }
    return 2;
}
'''


class RealGpuTextureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        directory = Path(cls.directory.name)
        harness = directory / 'texture_harness.c'
        harness.write_text(HARNESS)
        cls.binary = directory / 'texture'
        source = Path(os.environ.get('GPU_SOURCE', ROOT / 'pc/hal/gpu.c'))
        source = source.resolve()
        if not source.is_file():
            raise ValueError(f'GPU_SOURCE does not exist: {source}')
        flags = shlex.split(run(['pkg-config', '--cflags', '--libs', 'sdl2'],
                                timeout=10).require_ok().stdout)
        run(['gcc', '-std=gnu17', '-D_GNU_SOURCE', '-fpermissive', '-O1',
             '-ffunction-sections', '-fdata-sections', '-Ipc/include',
             '-Ipc/include/game', '-Ipc/hal', f'-DGPU_SOURCE="{source}"',
             str(harness), 'pc/hal/gpu_texture.c', '-Wl,--gc-sections',
             '-lm', '-o', str(cls.binary), *flags],
            cwd=ROOT, timeout=30).require_ok()

    def check_case(self, case):
        result = run([str(self.binary), case], cwd=ROOT, timeout=10)
        self.assertTrue(result.ok, result.reason or result.stdout)

    def test_literal_low_y_clut(self):
        self.check_case('low-clut')

    def test_literal_nonzero_x_clut_at_row480(self):
        self.check_case('row480-clut')

    def test_literal_tpage_origin_despite_registration(self):
        self.check_case('literal-origin')

    def test_commanded_4bit_despite_8bit_registration(self):
        self.check_case('registered4')

    def test_commanded_8bit_despite_4bit_registration(self):
        self.check_case('registered8')

    def test_commanded_15bit_despite_4bit_registration(self):
        self.check_case('registered15')

    def test_index_zero_color_and_actual_zero(self):
        self.check_case('index-zero')

    def test_all_nibbles_and_indices(self):
        self.check_case('all4')

    def test_all_bytes_and_indices(self):
        self.check_case('all8')

    def test_direct_color_positions_and_bit15(self):
        self.check_case('all15')


if __name__ == '__main__':
    unittest.main()
