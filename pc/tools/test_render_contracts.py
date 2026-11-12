# SPDX-License-Identifier: GPL-3.0-or-later
"""Real queue producers/consumer, GPU transfers and raster state contracts.

No assets or alternate renderer model. GPU_SOURCE and IMAGE_QUEUE_SOURCE
select complete older translation units for negative controls. Queues compile
at -O2 to exercise the one-entry-tail optimization that lost uploads.
"""
import os
from pathlib import Path
import shlex
import tempfile
import unittest
from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pc_paths import decomp  # noqa: E402

HARNESS = r"""
#define main ot_fixture_main
#include OT_FIXTURE
#undef main
#include "image_upload_queue.h"
#include <assert.h>

u8 screen_w[8192];
static u16 source[32][4];
static u16 before[512][1024];
extern u8 image_queue_n;
extern s32 image_upload_queue_add(u32, u8, u16, u16, u16, u16);
extern s32 image_upload_queue_add_flags(u32, u8, u8, u16, u16, u16, u16);

void nbn_trace_load_image(const void *p, u32 n,
                         s32 x, s32 y, s32 w, s32 h)
{
    (void)p; (void)n; (void)x; (void)y; (void)w; (void)h;
}
void nbn_trace_move_image(s32 x, s32 y, s32 w, s32 h, s32 a, s32 b)
{
    (void)x; (void)y; (void)w; (void)h; (void)a; (void)b;
}

static void queue_case(int count, int flags)
{
    int i, x;
    memset(screen_w, 0x5a, sizeof(screen_w));
    image_queue_n = 0;
    for (i = 0; i < count; i++) {
        for (x = 0; x < 4; x++) source[i][x] = (i + 1) * 32 + x;
        if (flags) {
            assert(image_upload_queue_add_flags((u32)(uintptr_t)source[i],
                   17, 1, 0, i, 8, 1) == 1);
        } else {
            assert(image_upload_queue_add((u32)(uintptr_t)source[i],
                   0, 0, 300 + i, 4, 1) == 1);
        }
    }
    if (count == 32) {
        assert(image_upload_queue_add(0, 0, 0, 0, 1, 1) == 0);
    }
    assert(image_queue_n == count);
    image_upload_queue_flush(0);
    assert(image_queue_n == 0);
    for (i = 0; i < count; i++) {
        for (x = 0; x < 4; x++) {
            assert(g_vram[(flags ? 256 : 300) + i]
                         [(flags ? 64 : 0) + x] == source[i][x]);
        }
    }
    /* The queue never touches the bytes after its 32-entry allocation. */
    for (i = sizeof(PcImageUploadQueue); i < sizeof(screen_w); i++)
        assert(screen_w[i] == 0x5a);
}

static void primitive(int family)
{
    u16 page = 0x108;
    switch (family) {
    case 0: draw_tile_blend(0,0,24,24,248,0,0,0,0); break;
    case 1: draw_sprt_blend(0,0,24,24,0,128,0,page,128,128,128,0,0);
            break;
    case 2: draw_poly_f3_blend(0,0,24,0,0,24,248,0,0,0,0); break;
    case 3: draw_poly_g3_blend(0,0,248,0,0,24,0,248,0,0,
                             0,24,248,0,0,0,0); break;
    case 4: draw_poly_ft3_blend(0,0,0,128,24,0,0,128,0,24,0,128,
                              0,page,128,128,128,0,0); break;
    case 5: draw_poly_gt3_blend(0,0,0,128,128,128,128,
                              24,0,0,128,128,128,128,
                              0,24,0,128,128,128,128,0,page,0,0); break;
    case 6: draw_line_g2_blend(0,0,248,0,0,24,24,248,0,0,0,0); break;
    default: assert(0);
    }
}

static void raster_case(int family, int mask)
{
    int x, y;
    u16 expected;
    for (y = 0; y < 512; y++)
        for (x = 0; x < 1024; x++) g_vram[y][x] = 0x9234;
    for (y = 128; y < 152; y++)
        for (x = 512; x < 536; x++) g_vram[y][x] = 31;
    memcpy(before, g_vram, sizeof(before));
    assert(gpu_draw_command(0xe3000000u | 8u | (8u << 10)));
    assert(gpu_draw_command(0xe4000000u | 11u | (11u << 10)));
    assert(gpu_draw_command(0xe6000000u | (u32)mask));
    primitive(family);
    expected = mask == 2 ? 0x9234 : mask == 1 ? 0x801f : 31;
    assert(g_vram[9][9] == expected);
    for (y = 0; y < 512; y++)
        for (x = 0; x < 1024; x++)
            if (x < 8 || x > 11 || y < 8 || y > 11)
                assert(g_vram[y][x] == before[y][x]);
}

/* Literal results follow saturating texel*RGB/128 (Mednafen ModTexel).
 * Raw commands bypass RGB. Zero is transparent; STP black is opaque. */
static void sprite_colors(void)
{
    static const u16 texels[4] = {0, 0x6208, 0xe208, 0x8000};
    static const u8 rgb[4][3] = {
        {0, 0, 0}, {128, 128, 128}, {64, 64, 64}, {64, 128, 255}
    };
    static const u16 tinted[4][4] = {
        {0x4210, 0, 0x8000, 0x8000},
        {0x4210, 0x6208, 0xe208, 0x8000},
        {0x4210, 0x3104, 0xb104, 0x8000},
        {0x4210, 0x7e04, 0xfe04, 0x8000}
    };
    static const int codes[3] = {0x64, 0x74, 0x7c};
    u32 packet[5];
    u16 expected;
    int size, tint, tex, raw, x, y, width;
    assert(gpu_draw_command(0xe1000108u));
    assert(gpu_draw_command(0xe3000000u));
    assert(gpu_draw_command(0xe4000000u | 63u | (63u << 10)));
    for (size = 0; size < 3; size++)
        for (tint = 0; tint < 4; tint++)
            for (tex = 0; tex < 4; tex++)
                for (raw = 0; raw < 2; raw++) {
                    width = size == 0 ? 3 : size == 1 ? 8 : 16;
                    for (y = 0; y < 64; y++)
                        for (x = 0; x < 64; x++) g_vram[y][x] = 0x4210;
                    for (y = 128; y < 144; y++)
                        for (x = 512; x < 528; x++)
                            g_vram[y][x] = texels[tex];
                    /* New texels: GP0(01h), as software must. */
                    packet[0] = 0x01ffffffu;
                    packet[1] = 0x01000000u;
                    DrawOTag(packet);
                    packet[0] = ((size ? 3u : 4u) << 24) | 0x00ffffffu;
                    packet[1] = ((u32)(codes[size] | raw) << 24)
                              | rgb[tint][0] | ((u32)rgb[tint][1] << 8)
                              | ((u32)rgb[tint][2] << 16);
                    packet[2] = 20u | (20u << 16);
                    packet[3] = 128u << 8;
                    packet[4] = (u32)width | ((u32)width << 16);
                    DrawOTag(packet);
                    expected = raw ? (tex ? texels[tex] : 0x4210)
                                   : tinted[tint][tex];
                    for (y = 0; y < 64; y++)
                        for (x = 0; x < 64; x++) {
                            u16 pixel;
                            pixel = x >= 20 && x < 20 + width &&
                                    y >= 20 && y < 20 + width ?
                                    expected : 0x4210;
                            if (g_vram[y][x] != pixel) {
                                fprintf(stderr, "sprite %02x tint=%d tex=%04x raw=%d at %d,%d: %04x != %04x\n",
                                        codes[size], tint, texels[tex], raw,
                                        x, y, g_vram[y][x], pixel);
                                abort();
                            }
                        }
                }
}

int main(int argc, char **argv)
{
    int family, mask;
    assert(argc == 3);
    g_use_mdfn_gpu = 0;
    gpu_queue_reset(1);
    if (argv[1][0] == 's') {
        sprite_colors();
    } else if (argv[1][0] == 'q') {
        queue_case(atoi(argv[1] + 1), atoi(argv[2]));
    } else {
        family = atoi(argv[1]);
        mask = atoi(argv[2]);
        raster_case(family, mask);
    }
    return 0;
}
"""


class RenderContractsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        harness = folder / 'render.c'
        harness.write_text(HARNESS)
        cls.binary = folder / 'render'
        gpu = Path(os.environ.get('GPU_SOURCE', ROOT / 'pc/hal/gpu.c'))
        queue = Path(os.environ.get('IMAGE_QUEUE_SOURCE',
                     ROOT / 'src/main/pc-port/image_upload_queue_flush.c'))
        flags = shlex.split(run(['pkg-config', '--cflags', 'sdl2'])
                            .require_ok().stdout)
        queue_object = folder / 'queue.o'
        run(['cc', '-std=gnu17', '-fpermissive', '-w', '-O2',
             '-fno-pie', '-fno-strict-aliasing',
             '-Ipc/include/game', '-Ipc/include',
             *shlex.split(os.environ.get('RENDER_CFLAGS', '')),
             '-c', str(queue), '-o', str(queue_object)],
            cwd=ROOT, timeout=30).require_ok()
        run(['cc', '-std=gnu17', '-D_GNU_SOURCE', '-fpermissive', '-w',
             '-O1', '-fno-pie', '-no-pie', '-fno-strict-aliasing',
             '-ffunction-sections', '-fdata-sections',
             '-DGPU_SOURCE="' + str(gpu.resolve()) + '"',
             '-DOT_FIXTURE="' + str(ROOT / 'pc/tests/gpu_ot_harness.c') + '"',
             '-Ipc/include', '-Ipc/include/game', '-Ipc/hal',
             *shlex.split(os.environ.get('RENDER_CFLAGS', '')),
             str(harness), str(queue_object),
             decomp('src/main/matched/image_upload_queue_add.c'),
             decomp('src/main/matched/image_upload_queue_add_flags.c'),
             decomp('src/main/matched/load_image_rect.c'),
             'pc/hal/gpu_queue.c', 'pc/hal/gpu_texture.c',
             'pc/hal/gpu_vram.c', 'pc/hal/ptr_validate.c',
             '-Wl,--gc-sections',
             '-Wl,--defsym=image_queue_n=screen_w+0x1429',
             '-o', str(cls.binary), *flags], cwd=ROOT, timeout=30).require_ok()

    def test_sprite_commands_apply_tint_and_raw_bit_without_losing_black(self):
        run([str(self.binary), 's', '0'], timeout=10).require_ok()

    def test_queue_uploads_all_entries_and_preserves_capacity_guard(self):
        for flags in (0, 1):
            for count in (0, 1, 2, 19, 32):
                with self.subTest(flags=flags, count=count):
                    run([str(self.binary), 'q' + str(count), str(flags)],
                        timeout=10).require_ok()

    def test_all_raster_families_obey_inclusive_area_and_keep_textures(self):
        for family in range(7):
            with self.subTest(family=family):
                run([str(self.binary), str(family), '0'],
                    timeout=10).require_ok()

    def test_all_raster_families_obey_mask_set_and_destination_mask(self):
        for family in range(7):
            for mask in (1, 2):
                with self.subTest(family=family, mask=mask):
                    run([str(self.binary), str(family), str(mask)],
                        timeout=10).require_ok()
