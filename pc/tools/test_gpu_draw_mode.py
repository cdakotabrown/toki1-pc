# SPDX-License-Identifier: GPL-3.0-or-later
"""Real GPU draw-mode packet regressions; asset-free, no decoder substitute.

Prepared without compiling/running. GPU_SOURCE optionally selects an actual
old gpu.c for negative controls. The generated C reuses gpu_ot_harness.c's
backend stubs unchanged; MDFN_GPU=0 is required, not a skipped backend test.

After main releases build serialization:
  . .venv/bin/activate
  python pc/tools/safe_process.py --timeout 120 --max-output 30000 -- \
    flock -w 60 build/gpu-contracts-20261021/worker-tests.lock \
    python -m unittest discover -s pc/tools -p test_gpu_draw_mode.py -v
"""
import os
from pathlib import Path
import shlex
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]

# No copied GPU/SDK implementation: only literal packets, calls and assertions.
# Absolute fixture include lets its own GPU_SOURCE select the real GPU TU.
HARNESS = r'''
#define main gpu_ot_fixture_main
#include OT_FIXTURE
#undef main

#define REQUIRE(condition, text) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s (line %d)\n", text, __LINE__); \
        return 1; \
    } \
} while (0)

static void seed_state(void)
{
    g_tpage_x = 3;
    g_tpage_y = 0;
    g_tpage_bpp = 1;
    g_tpage_abr = 2;
    g_tex_window_mask_x = 1;
    g_tex_window_mask_y = 2;
    g_tex_window_off_x = 3;
    g_tex_window_off_y = 4;
    g_mask_set_or = 0;
    g_mask_eval_and = 0;
}

static int page_is(int x, int y, int bpp, int abr)
{
    if (g_tpage_x == x && g_tpage_y == y && g_tpage_bpp == bpp &&
        g_tpage_abr == abr) return 1;
    fprintf(stderr, "page=(%d,%d,%d,%d), expected=(%d,%d,%d,%d)\n",
            g_tpage_x, g_tpage_y, g_tpage_bpp, g_tpage_abr,
            x, y, bpp, abr);
    return 0;
}

static int window_is(int mx, int my, int ox, int oy)
{
    if (g_tex_window_mask_x == mx && g_tex_window_mask_y == my &&
        g_tex_window_off_x == ox && g_tex_window_off_y == oy) return 1;
    fprintf(stderr, "window=(%u,%u,%u,%u), expected=(%d,%d,%d,%d)\n",
            (unsigned int)g_tex_window_mask_x,
            (unsigned int)g_tex_window_mask_y,
            (unsigned int)g_tex_window_off_x,
            (unsigned int)g_tex_window_off_y, mx, my, ox, oy);
    return 0;
}

static int producer(const char *name)
{
    u32 storage[5] = {0x12345678U, 0xa5123456U, 0, 0, 0x87654321U};
    u32 *packet = storage + 1;
    RECT window = {24, 40, 64, 128};
    int deferred = !strcmp(name, "producer-deferred");

    /* E1: page X=14, Y=1, depth=2, ABR=3, dfe/dtd enabled.
     * E2: mask=(24,16), offset=(3,5), from the literal rectangle above. */
    SetDrawMode(packet, 1, 1, 0x17e, &window);
    if (deferred) {
        REQUIRE(page_is(3, 0, 1, 2), "producer changed draw state early");
        REQUIRE(window_is(1, 2, 3, 4), "producer changed window early");
        REQUIRE(g_mask_set_or == 0 && g_mask_eval_and == 0,
                "producer changed mask state early");
        return 0;
    }
    REQUIRE(packet[0] == 0x02123456U, "len2/low24 link not preserved");
    REQUIRE(packet[1] == 0xe100077eU, "E1 not stored at packet+4");
    REQUIRE(packet[2] == 0xe2028e18U, "E2 not stored at packet+8");
    REQUIRE(storage[0] == 0x12345678U, "preceding guard changed");
    REQUIRE(storage[4] == 0x87654321U, "following guard changed");
    return 0;
}

static int producer_null(void)
{
    u32 packet[4] = {0x5aabcdefU, 0x11111111U,
                     0x22222222U, 0xdeadbeefU};
    SetDrawMode(packet, 0, 0, 0x85, NULL);
    REQUIRE(packet[0] == 0x02abcdefU, "NULL-window len/link changed");
    REQUIRE(packet[1] == 0xe1000085U, "NULL-window E1 position/value");
    /* Retail matched get_tw(NULL) returns zero, not GP0(E2). */
    REQUIRE(packet[2] == 0, "NULL window must emit a zero word");
    REQUIRE(packet[3] == 0xdeadbeefU, "NULL-window guard changed");
    REQUIRE(page_is(3, 0, 1, 2), "NULL-window producer applied state");
    return 0;
}

static int literal_page(int x, int registered)
{
    u32 packet[2] = {0x01ffffffU, 0xe1000170U};
    packet[1] |= (u32)x;
    if (registered) gpu_set_region_bpp(x, 0);
    DrawOTag(packet);
    REQUIRE(page_is(x, 1, 2, 3), "E1 did not retain commanded page");
    REQUIRE(window_is(1, 2, 3, 4), "standalone E1 changed window");
    REQUIRE(g_mask_set_or == 0 && g_mask_eval_and == 0,
            "standalone E1 changed masks");
    return 0;
}

static int standalone_window(void)
{
    /* Len1: the command itself is the only data word. */
    u32 packet[2] = {0x01ffffffU, 0xe2028e18U};
    DrawOTag(packet);
    REQUIRE(window_is(24, 16, 3, 5), "standalone E2 was not applied");
    REQUIRE(page_is(3, 0, 1, 2), "standalone E2 corrupted tpage");
    REQUIRE(g_mask_set_or == 0 && g_mask_eval_and == 0,
            "standalone E2 changed masks");
    return 0;
}

static int combined_mode(void)
{
    u32 packet[3] = {0x02ffffffU, 0xe1000085U, 0xe2028e18U};
    DrawOTag(packet);
    REQUIRE(page_is(5, 0, 1, 0), "combined packet lost E1");
    REQUIRE(window_is(24, 16, 3, 5), "combined packet lost E2");
    REQUIRE(g_mask_set_or == 0 && g_mask_eval_and == 0,
            "combined E1/E2 changed masks");
    return 0;
}

static int traversal_order(void)
{
    u32 first[4] = {0, 0, 0, 0};
    u32 second[4] = {0, 0, 0, 0};
    RECT window = {24, 40, 64, 128};
    /* Create A then B, but execute B then A. Creation order must not win. */
    SetDrawMode(first, 0, 0, 0x85, &window);
    SetDrawMode(second, 0, 0, 0x113, &window);
    REQUIRE(page_is(3, 0, 1, 2), "packet creation bypassed OT ordering");
    pc_ot_setaddr(second, first);
    pc_ot_setaddr(first, TERMINATOR);
    DrawOTag(second);
    REQUIRE(page_is(5, 0, 1, 0), "last traversed mode did not win");
    REQUIRE(window_is(24, 16, 3, 5), "ordered packets lost window");
    return 0;
}

static int mask_packet(unsigned int bits)
{
    void *mapping;
    long page_size;
    u32 *packet;
    int expected_set, expected_check, result;

    /* End an EXACT len1 packet at a protected page, so reading prim+4
     * instead of prim faults. There is no dummy data after the command. */
    page_size = sysconf(_SC_PAGESIZE);
    REQUIRE(page_size > 8, "cannot query host page size");
    mapping = mmap(NULL, (size_t)page_size * 2, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    REQUIRE(mapping != MAP_FAILED, "mask fixture mmap failed");
    if (mprotect((unsigned char *)mapping + page_size,
                 (size_t)page_size, PROT_NONE)) {
        munmap(mapping, (size_t)page_size * 2);
        fprintf(stderr, "mask fixture mprotect failed\n");
        return 1;
    }
    packet = (u32 *)((unsigned char *)mapping + page_size - 8);
    packet[0] = 0x01ffffffU;
    packet[1] = 0xe6000000U | bits;
    pc_ot_setaddr(packet, TERMINATOR);
    DrawOTag(packet);
    expected_set = bits & 1U ? 0x8000 : 0;
    expected_check = bits & 2U ? 0x8000 : 0;
    result = g_mask_set_or != expected_set ||
             g_mask_eval_and != expected_check;
    if (result) {
        fprintf(stderr, "E6 bits=%u: masks=%x,%x expected=%x,%x\n",
                bits, (unsigned int)g_mask_set_or,
                (unsigned int)g_mask_eval_and, (unsigned int)expected_set,
                (unsigned int)expected_check);
    }
    /* Same real len1 record, now zero: both masks must clear. */
    packet[1] = 0xe6000000U;
    DrawOTag(packet);
    if (g_mask_set_or != 0 || g_mask_eval_and != 0) {
        fprintf(stderr, "E6 zero failed to clear masks\n");
        result = 1;
    }
    if (!page_is(3, 0, 1, 2) || !window_is(1, 2, 3, 4)) result = 1;
    if (munmap(mapping, (size_t)page_size * 2)) return 1;
    return result;
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    if (mdfn_gpu_enabled()) return 3;
    seed_state();
    if (!strcmp(argv[1], "producer-layout") ||
        !strcmp(argv[1], "producer-deferred")) return producer(argv[1]);
    if (!strcmp(argv[1], "producer-null")) return producer_null();
    if (!strcmp(argv[1], "page5")) return literal_page(5, 0);
    if (!strcmp(argv[1], "page0")) return literal_page(0, 0);
    if (!strcmp(argv[1], "page8")) return literal_page(8, 0);
    if (!strcmp(argv[1], "page14")) return literal_page(14, 0);
    if (!strcmp(argv[1], "registered")) return literal_page(14, 1);
    if (!strcmp(argv[1], "window")) return standalone_window();
    if (!strcmp(argv[1], "combined")) return combined_mode();
    if (!strcmp(argv[1], "order")) return traversal_order();
    if (!strcmp(argv[1], "mask-set")) return mask_packet(1);
    if (!strcmp(argv[1], "mask-check")) return mask_packet(2);
    if (!strcmp(argv[1], "mask-both")) return mask_packet(3);
    return 2;
}
'''


class GpuDrawModeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='gpu-draw-mode-')
        cls.addClassCleanup(cls.directory.cleanup)
        directory = Path(cls.directory.name)
        harness = directory / 'draw_mode.c'
        # sysconf is fixture plumbing, not replacement GPU behavior.
        harness.write_text('#include <unistd.h>\n' + HARNESS)
        cls.binary = directory / 'draw-mode'
        source = Path(os.environ.get('GPU_SOURCE', ROOT / 'pc/hal/gpu.c'))
        source = source.resolve(strict=True)
        fixture = (ROOT / 'pc/tests/gpu_ot_harness.c').resolve(strict=True)
        flags = shlex.split(run(['pkg-config', '--cflags', 'sdl2'],
                                timeout=10).require_ok().stdout)
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        run([*compiler, '-std=gnu17', '-D_GNU_SOURCE', '-fpermissive', '-O1',
             '-ffunction-sections', '-fdata-sections',
             '-DGPU_SOURCE="' + str(source) + '"',
             '-DOT_FIXTURE="' + str(fixture) + '"',
             '-Ipc/include', '-Ipc/include/game', '-Ipc/hal',
             str(harness), 'pc/hal/gpu_texture.c', 'pc/hal/gpu_vram.c', 'pc/hal/gpu_queue.c',
             '-Wl,--gc-sections',
             '-o', str(cls.binary), *flags], cwd=ROOT,
            timeout=30).require_ok()

    def check_case(self, case):
        env = dict(os.environ, MDFN_GPU='0', GP0_TRACE='0', PC_DEBUG='0')
        result = run([str(self.binary), case], cwd=ROOT, env=env,
                     timeout=10, max_output=16000)
        self.assertTrue(result.ok, f'{case}: {result.reason}\n{result.stdout}')

    def test_setdrawmode_exact_len2_record_preserves_link_and_guards(self):
        self.check_case('producer-layout')

    def test_setdrawmode_does_not_mutate_drawing_state(self):
        self.check_case('producer-deferred')

    def test_null_window_retains_retail_zero_word_and_packet_bounds(self):
        self.check_case('producer-null')

    def test_standalone_e1_normal_page_positive_control(self):
        self.check_case('page5')

    def test_e1_literal_unregistered_page_zero(self):
        self.check_case('page0')

    def test_e1_literal_unregistered_page_eight(self):
        self.check_case('page8')

    def test_e1_literal_unregistered_page_fourteen(self):
        self.check_case('page14')

    def test_e1_depth_is_commanded_despite_contradictory_registration(self):
        self.check_case('registered')

    def test_standalone_e2_updates_window_without_changing_tpage(self):
        self.check_case('window')

    def test_combined_len2_e1_e2_updates_both_states(self):
        self.check_case('combined')

    def test_execution_order_not_packet_creation_order_controls_state(self):
        self.check_case('order')

    def test_exact_len1_e6_set_uses_8000_mask_then_zero_clears(self):
        self.check_case('mask-set')

    def test_exact_len1_e6_check_uses_8000_mask_then_zero_clears(self):
        self.check_case('mask-check')

    def test_exact_len1_e6_both_bits_then_zero_clears(self):
        self.check_case('mask-both')


if __name__ == '__main__':
    unittest.main()
