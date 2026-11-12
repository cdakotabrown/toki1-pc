# SPDX-License-Identifier: GPL-3.0-or-later
"""Pure VRAM contracts, not a renderer/scene parity claim.

Run under the shared compile/test lock:
  flock build/gpu-contracts-20261021/worker-tests.lock \
    .venv/bin/python -m unittest discover -s pc/tools -p test_gpu_vram.py -v

Expected images use independent scalar address arithmetic and compare EVERY
VRAM/output/source halfword, including guard words and odd-size padding.
Raw GP0 zero/masked sizes and SDK empty/clamped sizes are separate contracts.
No overlap behavior is asserted; no emulator implementation is used as oracle.
"""
from array import array
import ctypes
import os
from pathlib import Path
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
HAL = ROOT / 'pc/hal'
WIDTH, HEIGHT = 1024, 512
COUNT = WIDTH * HEIGHT
GUARD = 8
SENTINEL = 0xDEAD
WORD = ctypes.c_ushort
POINTER = ctypes.POINTER(WORD)
BACKGROUND = array('H', ((i * 109 + i // WIDTH * 37) & 65535
                         for i in range(COUNT)))


def guarded(values):
    values = array('H', [SENTINEL] * GUARD) + values
    values.extend([SENTINEL] * GUARD)
    storage = (WORD * len(values)).from_buffer_copy(values)
    pointer = ctypes.cast(ctypes.byref(storage, GUARD * 2), POINTER)
    return storage, pointer


def packed(storage):
    return bytes(storage)


def reference_indices(x, y, width, height):
    """A linear CPU stream; independently wrap each VRAM coordinate."""
    for pixel in range(width * height):
        row, column = divmod(pixel, width)
        yield ((y + row) % HEIGHT) * WIDTH + ((x + column) % WIDTH)


class GpuVramTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='gpu-vram-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.path = Path(cls.directory.name)
        cls.compiler = os.environ.get('CC', 'cc')
        cls.library_path = cls.path / 'vram.so'
        run([cls.compiler, '-std=c89', '-pedantic', '-Wall', '-Wextra',
             '-Werror', '-O2', '-fPIC', '-shared', str(HAL / 'gpu_vram.c'),
             '-o', str(cls.library_path)], timeout=30).require_ok()
        cls.lib = ctypes.CDLL(str(cls.library_path))
        cls.lib.gpu_vram_load.argtypes = [POINTER] + [ctypes.c_int] * 4 + [
            POINTER, ctypes.c_int, ctypes.c_int]
        cls.lib.gpu_vram_load.restype = None
        cls.lib.gpu_vram_store.argtypes = [POINTER] + [ctypes.c_int] * 4 + [
            POINTER]
        cls.lib.gpu_vram_store.restype = None
        cls.lib.gpu_vram_image_pixels.argtypes = [ctypes.c_int, ctypes.c_int,
            ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint)]
        cls.lib.gpu_vram_image_pixels.restype = ctypes.c_size_t

    def check_transfer(self, x, y, width, height, mask_set=0, mask_check=0):
        copy_width = (width - 1) % WIDTH + 1
        copy_height = (height - 1) % HEIGHT + 1
        count = copy_width * copy_height
        # One padding halfword even for an even image, to detect extra writes.
        image = array('H', ((i * 71 + 0x1234) & 65535
                            for i in range(count + 1)))
        source, pixels = guarded(image)
        original_source = packed(source)
        vram, memory = guarded(BACKGROUND)
        expected = array('H', BACKGROUND)
        for pixel, index in enumerate(reference_indices(
                x, y, copy_width, copy_height)):
            if not (mask_check and expected[index] & 0x8000):
                expected[index] = image[pixel] | (0x8000 if mask_set else 0)
        expected_vram, _ = guarded(expected)
        self.lib.gpu_vram_load(memory, x, y, width, height, pixels,
                                mask_set, mask_check)
        self.assertEqual(packed(vram), packed(expected_vram))
        self.assertEqual(packed(source), original_source)

        output, destination = guarded(array('H', [SENTINEL] * (count + 1)))
        readback = array('H', (expected[index] for index in reference_indices(
            x, y, copy_width, copy_height)))
        readback.append(SENTINEL)
        expected_output, _ = guarded(readback)
        before_store = packed(vram)
        self.lib.gpu_vram_store(memory, x, y, width, height, destination)
        self.assertEqual(packed(output), packed(expected_output))
        self.assertEqual(packed(vram), before_store)

    def test_unchanged_valid_in_bounds(self):
        for rect in [(0, 0, 1, 1), (960, 128, 3, 3), (32, 40, 64, 8),
                     (0, 480, 256, 1), (640, 240, 320, 240)]:
            with self.subTest(rect=rect):
                self.check_transfer(*rect)

    def test_right_wrap_retains_full_cpu_row_stride(self):
        self.check_transfer(1022, 9, 5, 3)

    def test_bottom_and_corner_wrap_without_axis_carry(self):
        for rect in [(17, 511, 3, 3), (1023, 511, 3, 3)]:
            with self.subTest(rect=rect):
                self.check_transfer(*rect)

    def test_negative_origins_are_masked_not_host_indices(self):
        for origin in [(-1, -1), (-1025, -513), (-32768, 32767),
                       (-2147483648, 2147483647)]:
            with self.subTest(origin=origin):
                self.check_transfer(*origin, 5, 3)

    def test_raw_gp0_dimension_masks(self):
        for sizes in [(1025, 513), (2049, 1026), (-1, 1), (1, -1)]:
            with self.subTest(sizes=sizes):
                self.check_transfer(1023, 511, *sizes)

    def test_raw_gp0_zero_is_full_extent(self):
        for sizes in [(0, 1), (1, 0), (0, 0), (1024, 512)]:
            with self.subTest(sizes=sizes):
                self.check_transfer(7, 11, *sizes)

    def test_upload_mask_flags_consume_every_source_pixel(self):
        for mask_set in (0, 1):
            for mask_check in (0, 1):
                with self.subTest(set=mask_set, check=mask_check):
                    self.check_transfer(1022, 511, 5, 3, mask_set, mask_check)

    def test_sdk_preparation_is_not_gp0_masking(self):
        for width, height in [(0, 0), (0, 7), (8, 0), (-1, 1), (1, -1),
                              (-32768, 32767), (1025, 513), (32767, 32767),
                              (1024, 512), (5, 3)]:
            with self.subTest(width=width, height=height):
                out_width, out_height = ctypes.c_uint(), ctypes.c_uint()
                count = self.lib.gpu_vram_image_pixels(
                    width, height, ctypes.byref(out_width),
                    ctypes.byref(out_height))
                expected_width = max(0, min(WIDTH, width))
                expected_height = max(0, min(HEIGHT, height))
                self.assertEqual((out_width.value, out_height.value, count),
                                 (expected_width, expected_height,
                                  expected_width * expected_height))

    def test_sanitized_exact_storage_and_extreme_coordinates(self):
        source = self.path / 'safety.c'
        source.write_text(r'''
#include <limits.h>
#include "gpu_vram.h"
static unsigned short vram[GPU_VRAM_PIXELS];
static unsigned short pixels[GPU_VRAM_PIXELS];
int main(void)
{
    unsigned int width, height;
    if (USHRT_MAX != 65535U) return 1;
    if (gpu_vram_image_pixels(0, 0, &width, &height) != 0) return 2;
    gpu_vram_load(vram, INT_MIN, INT_MAX, 0, 0, pixels, 0, 0);
    gpu_vram_store(vram, INT_MAX, INT_MIN, 0, 0, pixels);
    gpu_vram_load(vram, -1, -1, INT_MIN, INT_MIN, pixels, 1, 1);
    gpu_vram_store(vram, -1, -1, INT_MIN, INT_MIN, pixels);
    return 0;
}
''')
        executable = self.path / 'safety'
        run([self.compiler, '-std=c89', '-pedantic', '-Wall', '-Wextra',
             '-Werror', '-O1', '-g', '-fsanitize=address,undefined',
             '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
             '-I', str(HAL), str(source), str(HAL / 'gpu_vram.c'),
             '-o', str(executable)], timeout=30).require_ok()
        run([str(executable)], timeout=15).require_ok()

    def test_legacy_negative_origin_sanitizer_control(self):
        # The old StoreImage address expression, in an isolated executable.
        # This is a recorded defect control, not a supported GPU operation.
        source = self.path / 'legacy_negative.c'
        source.write_text(r'''
static unsigned short vram[512][1024];
static unsigned short output;
static void legacy_store(int x, int y)
{
    output = vram[y][x];
}
int main(void)
{
    legacy_store(-1, 0);
    return output;
}
''')
        executable = self.path / 'legacy_negative'
        run([self.compiler, '-std=c89', '-pedantic', '-Wall', '-Wextra',
             '-Werror', '-O1', '-fsanitize=undefined',
             '-fno-sanitize-recover=all', str(source),
             '-o', str(executable)], timeout=30).require_ok()
        result = run([str(executable)], timeout=15)
        self.assertIsNone(result.reason)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('index -1 out of bounds', result.stdout)

    def test_recorded_legacy_clipping_negative_controls(self):
        # Models of the previous gpu.c inner loops, NOT a second GPU oracle.
        # These controls show whole-image comparisons reject both old defects.
        image = list(range(15))
        legacy_vram = list(BACKGROUND)
        consumed = 0
        for row in range(3):
            for column in range(2):  # old right-edge loop truncates width 5
                legacy_vram[(9 + row) * WIDTH + 1022 + column] = image[consumed]
                consumed += 1
        correct = list(BACKGROUND)
        for pixel, index in enumerate(reference_indices(1022, 9, 5, 3)):
            correct[index] = image[pixel]
        self.assertNotEqual(legacy_vram, correct)
        self.assertEqual(legacy_vram[10 * WIDTH + 1022], 2)
        self.assertEqual(correct[10 * WIDTH + 1022], 5)
        legacy_output = [SENTINEL] * 16
        copied = [BACKGROUND[(9 + row) * WIDTH + 1022 + column]
                  for row in range(3) for column in range(2)]
        legacy_output[:len(copied)] = copied
        expected_output = [BACKGROUND[index] for index in reference_indices(
            1022, 9, 5, 3)] + [SENTINEL]
        self.assertNotEqual(legacy_output, expected_output)
        self.assertEqual(legacy_output[6], SENTINEL)
        self.assertNotEqual(expected_output[6], SENTINEL)


if __name__ == '__main__':
    unittest.main()
