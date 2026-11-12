# SPDX-License-Identifier: GPL-3.0-or-later
"""Asset/SDL-independent scanout tests; legacy parity is not hardware parity.

Run under build/gpu-contracts-20261021/worker-tests.lock when coordinating
workers. Builds one tiny shared library, with bounded safe_process commands.
The pinned baseline is extracted mechanically from committed gpu.c, not a
second handwritten approximation. RGB24 row wrapping remains unverified;
row-edge tests below deliberately lock the existing black clipping policy.
"""
import ctypes as C
from pathlib import Path
import random
import tempfile
import unittest

from safe_process import run
from pc_paths import NO_HISTORY, have_commit

ROOT = Path(__file__).resolve().parents[2]
BASELINE = 'fa8eae707b'
WIDTH, HEIGHT = 320, 240
COUNT = WIDTH * HEIGHT
BLACK = 0xFF
U16 = C.c_ushort
U32 = C.c_uint
Vram = U16 * (1024 * 512)
Pixels = U32 * (COUNT + 2)


class State(C.Structure):
    _fields_ = [(name, C.c_int) for name in
                ('x', 'y', 'width', 'height', 'rgb24', 'enabled')]


def bgr555(value):
    return (((value & 31) << 27) | (((value >> 5) & 31) << 19)
            | (((value >> 10) & 31) << 11) | 255)


def baseline_source():
    source = run(['git', 'show', f'{BASELINE}:pc/hal/gpu.c'], cwd=ROOT,
                 timeout=10).require_ok().stdout
    start = source.index('static u32 psx_to_rgba(u16 psx) {')
    convert = source[start:source.index('\n}', start) + 2]
    start = source.index('        for (j = 0; j < 240; j++) {',
                         source.index('void gpu_display(void) {'))
    end = source.index('        SDL_UpdateTexture(g_display_tex', start)
    loop = source[start:end]
    return '''
#include "gpu_scanout.h"
typedef unsigned short u16;
typedef unsigned int u32;
typedef int s32;
#define VRAM_WIDTH 1024
#define VRAM_HEIGHT 512
''' + convert + '''
void legacy_pack(const GpuScanoutState *state, const u16 *vram, u32 *pixels)
{
    const u16 (*g_vram)[1024];
    int g_disp_x, g_disp_y, g_disp_w, g_disp_h, g_disp_rgb24;
    int i, j;
    g_vram = (const u16 (*)[1024])vram;
    g_disp_x = state->x;
    g_disp_y = state->y;
    g_disp_w = state->width;
    g_disp_h = state->height;
    g_disp_rgb24 = state->rgb24;
    /* Normalize the old SDL black clear into an observable pixel buffer. */
    if (!state->enabled) {
        for (i = 0; i < 320 * 240; i++) pixels[i] = 0xFF;
        return;
    }
''' + loop + '\n}\n'


class ScanoutTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not have_commit(BASELINE):
            raise unittest.SkipTest(NO_HISTORY % BASELINE)
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        directory = Path(cls.directory.name)
        fixture = directory / 'baseline.c'
        fixture.write_text(baseline_source())
        library = directory / 'scanout.so'
        run(['cc', '-std=c89', '-pedantic', '-Wall', '-Wextra', '-Werror',
             '-O1', '-fPIC', '-shared', '-fsanitize=undefined',
             '-fno-sanitize-recover=undefined', '-Ipc/hal',
             'pc/hal/gpu_scanout.c', str(fixture), '-o', str(library)],
            cwd=ROOT, timeout=30).require_ok()
        cls.library = C.CDLL(str(library))
        cls.pack = cls.library.gpu_scanout_pack
        cls.pack.argtypes = [C.POINTER(State), C.POINTER(U16), C.POINTER(U32),
                             C.c_size_t, C.POINTER(C.c_int),
                             C.POINTER(C.c_int)]
        cls.pack.restype = C.c_int
        cls.legacy = cls.library.legacy_pack
        cls.legacy.argtypes = [C.POINTER(State), C.POINTER(U16),
                               C.POINTER(U32)]
        cls.legacy.restype = None

    def setUp(self):
        self.vram = Vram()
        self.state = State(0, 0, WIDTH, HEIGHT, 0, 1)
        self.output = Pixels(*([0xDEADBEEF] * (COUNT + 2)))
        self.width, self.height = C.c_int(-1), C.c_int(-1)
        self.target = C.cast(C.byref(self.output, C.sizeof(U32)),
                             C.POINTER(U32))

    def render(self):
        before = bytes(self.state), bytes(self.vram)
        self.assertEqual(self.pack(C.byref(self.state), self.vram, self.target,
                                   COUNT, C.byref(self.width),
                                   C.byref(self.height)), 1)
        self.assertEqual((self.width.value, self.height.value), (320, 240))
        self.assertEqual((bytes(self.state), bytes(self.vram)), before)
        self.assertEqual(self.output[0], 0xDEADBEEF)
        self.assertEqual(self.output[COUNT + 1], 0xDEADBEEF)
        return list(self.output)[1:-1]

    def test_all_65536_bgr555_words_shift3_and_ignore_mask_bit(self):
        # Every possible word, including both values of bit 15, is scanned.
        for y in range(HEIGHT):
            for x in range(WIDTH):
                self.vram[y * 1024 + x] = (y * WIDTH + x) & 65535
        pixels = self.render()
        self.assertEqual(pixels, [bgr555(i & 65535) for i in range(COUNT)])
        self.assertEqual(pixels[32767], 0xF8F8F8FF)
        self.assertEqual(pixels[32768], BLACK)

    def test_disabled_is_black_even_with_bright_vram(self):
        self.vram[0] = 0x7FFF
        self.state.enabled = 0
        self.assertEqual(self.render(), [BLACK] * COUNT)

    def test_nonzero_mask_and_mode_are_boolean(self):
        self.state.enabled = -9
        self.state.rgb24 = 7
        self.vram[0], self.vram[1] = 0x3412, 0x0056
        self.assertEqual(self.render()[0], 0x123456FF)

    def test_small_rectangle_initializes_all_padding(self):
        self.state.width, self.state.height = 1, 1
        self.vram[0] = 0x001F
        pixels = self.render()
        self.assertEqual(pixels[0], 0xF80000FF)
        self.assertEqual(pixels[1:], [BLACK] * (COUNT - 1))

    def test_nonzero_origin_and_negative_origin_clipping(self):
        self.state.x, self.state.y = 17, 243
        self.vram[243 * 1024 + 17] = 0x03E0
        self.assertEqual(self.render()[0], 0x00F800FF)
        self.state.x, self.state.y = -1, -1
        self.vram[0] = 0x7C00
        pixels = self.render()
        self.assertEqual(pixels[:WIDTH + 1], [BLACK] * (WIDTH + 1))
        self.assertEqual(pixels[WIDTH + 1], 0x0000F8FF)

    def test_bottom_right_clips_without_wrap(self):
        self.state.x, self.state.y = 1023, 511
        self.vram[-1] = 0x7FFF
        self.vram[0] = 31
        pixels = self.render()
        self.assertEqual(pixels[0], 0xF8F8F8FF)
        self.assertEqual(pixels[1:], [BLACK] * (COUNT - 1))

    def test_rgb24_exact_bytes_across_halfwords_and_mode_switch(self):
        self.state = State(17, 23, 2, 1, 1, 1)
        base = 23 * 1024 + 17
        self.vram[base:base + 3] = [0x3412, 0x7856, 0xBC9A]
        pixels = self.render()
        self.assertEqual(pixels[:2], [0x123456FF, 0x789ABCFF])
        self.assertEqual(pixels[2:], [BLACK] * (COUNT - 2))
        self.state.rgb24 = 0
        self.assertEqual(self.render()[:2], [bgr555(0x3412), bgr555(0x7856)])

    def test_rgb24_legacy_row_edge_is_black_not_next_row_bytes(self):
        self.state = State(1021, 4, 3, 1, 1, 1)
        base = 4 * 1024 + 1021
        self.vram[base:base + 3] = [0x3412, 0x7856, 0xBC9A]
        self.vram[5 * 1024] = 0xFFFF
        self.assertEqual(self.render()[:3],
                         [0x123456FF, 0x789ABCFF, BLACK])
        self.state.x = 1023
        self.assertEqual(self.render(), [BLACK] * COUNT)

    def test_rgb24_negative_origin_keeps_legacy_coordinate_gate(self):
        self.state = State(-1, 0, 2, 1, 1, 1)
        self.vram[:2] = [0x3412, 0x7856]
        self.assertEqual(self.render()[:2], [BLACK, 0x345678FF])

    def test_unsupported_extents_are_legacy_crop_or_black(self):
        for width, height in [(0, 240), (320, 0), (-1, 240), (320, -1)]:
            with self.subTest(width=width, height=height):
                self.state.width, self.state.height = width, height
                self.assertEqual(self.render(), [BLACK] * COUNT)
        self.state.width, self.state.height = 640, 480
        self.vram[239 * 1024 + 319] = 31
        self.assertEqual(self.render()[-1], 0xF80000FF)

    def test_extreme_origins_are_safe_black_not_signed_overflow(self):
        for origin in (-2147483648, 2147483647):
            for field in ('x', 'y'):
                for mode in (0, 1):
                    with self.subTest(origin=origin, field=field, mode=mode):
                        self.state = State(0, 0, WIDTH, HEIGHT, mode, 1)
                        setattr(self.state, field, origin)
                        self.assertEqual(self.render(), [BLACK] * COUNT)

    def test_invalid_arguments_do_not_write_any_output(self):
        normal = [C.byref(self.state), self.vram, self.target, COUNT,
                  C.byref(self.width), C.byref(self.height)]
        for index in (0, 1, 2, 4, 5, 3):
            args = normal.copy()
            args[index] = COUNT - 1 if index == 3 else None
            before = bytes(self.output)
            self.assertEqual(self.pack(*args), 0)
            self.assertEqual(bytes(self.output), before)
            self.assertEqual((self.width.value, self.height.value), (-1, -1))

    def test_pinned_real_old_loop_parity_64_states(self):
        rng = random.Random(20261021)
        for i in range(len(self.vram)):
            self.vram[i] = rng.randrange(65536)
        expected = (U32 * COUNT)()
        for case in range(64):
            self.state = State(rng.choice([-12, -1, 0, 17, 640, 1019, 1023]),
                               rng.choice([-2, 0, 23, 240, 510, 511]),
                               rng.choice([-1, 0, 2, 256, 320, 640]),
                               rng.choice([-1, 1, 224, 240, 480]),
                               case % 2, int(case % 11 != 0))
            with self.subTest(case=case, state=bytes(self.state).hex()):
                self.legacy(C.byref(self.state), self.vram, expected)
                self.assertEqual(self.render(), list(expected))


if __name__ == '__main__':
    unittest.main()
