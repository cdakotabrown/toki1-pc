# SPDX-License-Identifier: GPL-3.0-or-later
"""Real GP0 textured polygons through native and vendored rasterizers.

No assets, replacement decoder or renderer model. Literal packet layouts and
fixed arithmetic results check command modes, mask writes and persistent page
state. Coverage is sampled inside faces: raster edge fidelity is separate.
GPU_SOURCE selects a complete older HAL for negative controls.
State-prefixed packets (libgs GsSortSprite: E1 + SPRT in one tag) are one
GP0 word stream; the primitive after the state words must be drawn.
"""
import os
from pathlib import Path
import shlex
import tempfile
import unittest
from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
# The second backend is Mednafen's rasteriser behind pc/3rd/mednafen-gpu's
# shim: local parity sources that are not in the repository (.gitignore,
# THIRD-PARTY.md), so a fresh clone or worktree has none of them.
SHIM_SOURCE = Path(os.environ.get('POLYGON_SHIM_SOURCE',
                                  ROOT / 'pc/3rd/mednafen-gpu/shim/mdfn_gpu_shim.cpp'))
# POLYGON_SHIM_SOURCE (a negative control's older shim) is still compiled
# against this tree's pc/3rd/mednafen-gpu: git.h, the shim headers, upstream/
MDFN_SOURCES = (SHIM_SOURCE,) + tuple(ROOT / 'pc/3rd/mednafen-gpu' / p for p in (
    'git.h', 'shim/beetle_psx_globals.h', 'upstream/gpu.h', 'upstream/gpu_polygon.cpp'))
HAVE_MDFN = all(p.is_file() for p in MDFN_SOURCES)

HARNESS = r'''#define main ot_fixture_main
#define GPU_REAL_BACKEND
#include OT_FIXTURE
#undef main
#include <assert.h>

/* tag, color/code, then literal PS1 XY/UV/color records. */
static const u32 packets[4][13] = {
    {0x07ffffff, 0x24000000, 0x00140014, 0x00008000,
     0x00140024, 0x00008000, 0x00240014, 0x00008000},
    {0x09ffffff, 0x2c000000, 0x00140014, 0x00008000,
     0x00140024, 0x00008000, 0x00240014, 0x00008000,
     0x00240024, 0x00008000},
    {0x09ffffff, 0x34000000, 0x00140014, 0x00008000,
     0, 0x00140024, 0x00008000, 0, 0x00240014, 0x00008000},
    {0x0cffffff, 0x3c000000, 0x00140014, 0x00008000,
     0, 0x00140024, 0x00008000, 0, 0x00240014, 0x00008000,
     0, 0x00240024, 0x00008000}
};
static const u16 raw_blend[4] = {0xd20c, 0xfff8, 0x8008, 0xda92};
static const u16 mod_blend[2][4] = {
    {0xde0a, 0xfff4, 0x800c, 0xde91},
    {0xdde9, 0xfff3, 0x802d, 0xde70}
};

static void expect(int family, const char *stage, int x, int y, u16 pixel)
{
    if (g_vram[y][x] != pixel) {
        fprintf(stderr, "backend=%d family=%d %s at %d,%d: %04x != %04x\n",
                g_use_mdfn_gpu, family, stage, x, y, g_vram[y][x], pixel);
        abort();
    }
}

static void setup(u16 texel, int depth, int dither, int mask)
{
    int x, y;
    /* Each case seeds new VRAM, so start with cold backend caches. */
    mdfn_gpu_init(&g_vram[0][0]);
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++) g_vram[y][x] = 0x4210;
    /* Source at page 8, UV=(0,128); palette entry five at (69,400). */
    g_vram[128][512] = depth == 2 ? texel : 5;
    g_vram[400][69] = texel;
    /* Seeded VRAM, so clear the caches as software must: GP0(01h). */
    {
        u32 clear[2] = {0x01ffffffu, 0x01000000u};
        DrawOTag(clear);
    }
    assert(gpu_draw_command(0xe1000083u | (dither ? 0x200u : 0)));
    assert(gpu_draw_command(0xe3000000u));
    assert(gpu_draw_command(0xe4000000u | 63u | (63u << 10)));
    assert(gpu_draw_command(0xe5000000u));
    assert(gpu_draw_command(0xe6000000u | mask));
}

static void submit(int family, int raw, int blend, int abr, int depth,
                   int dither)
{
    u32 packet[13], color;
    u16 clut, page;
    memcpy(packet, packets[family], sizeof(packet));
    color = raw ? 0x000000u : 0xff8040u; /* R64 G128 B255 */
    packet[1] |= color | ((u32)(raw | (blend << 1)) << 24);
    clut = (400u << 6) | 4u;
    /* High page bits may NOT override E1 dither. */
    page = 8u | (abr << 5) | (depth << 7) | (!dither ? 0x200u : 0);
    packet[3] |= (u32)clut << 16;
    if (family < 2) packet[5] |= (u32)page << 16;
    else {
        packet[4] = packet[7] = packet[10] = color;
        packet[6] |= (u32)page << 16;
    }
    DrawOTag(packet);
}

static void color_cases(void)
{
    int family, raw, blend, abr, stp, depth, dither;
    u16 texel, expected;
    u32 sprite[5];
    for (family = 0; family < 4; family++)
    for (raw = 0; raw < 2; raw++)
    for (blend = 0; blend < 2; blend++)
    for (abr = 0; abr < 4; abr++)
    for (stp = 0; stp < 2; stp++)
    for (depth = 0; depth < 3; depth++)
    for (dither = 0; dither < 2; dither++) {
        texel = 0x6208u | (stp ? 0x8000u : 0);
        setup(texel, depth, dither, 0);
        submit(family, raw, blend, abr, depth, dither);
        assert(g_tpage_x == 8 && g_tpage_y == 0 &&
               g_tpage_bpp == depth && g_tpage_abr == abr);
        assert(g_dither_enabled == dither);
        expected = raw ? texel : (dither ? 0x7de3u : 0x7e04u) |
                                 (stp ? 0x8000u : 0);
        if (blend && stp)
            expected = raw ? raw_blend[abr] : mod_blend[dither][abr];
        expect(family, "polygon color", 23, 23, expected);
        if (family & 1) expect(family, "second quad face", 33, 33, expected);
        expect(family, "outside", 19, 19, 0x4210);
        /* A following sprite consumes the polygon's retained page and ABR. */
        sprite[0] = 0x04ffffffu;
        sprite[1] = 0x65000000u | (blend ? 0x02000000u : 0);
        sprite[2] = 50u | (50u << 16);
        sprite[3] = (128u << 8) | ((u32)((400u << 6) | 4u) << 16);
        sprite[4] = 1u | (1u << 16);
        DrawOTag(sprite);
        expect(family, "following sprite", 50, 50,
               blend && stp ? raw_blend[abr] : texel);
        assert(g_vram[128][512] == (depth == 2 ? texel : 5));
    }
}

static void masks_and_black(void)
{
    static const u16 texels[4] = {0, 0x8000, 0x6208, 0xe208};
    int family, mask, tex, raw;
    u16 expected;
    for (family = 0; family < 4; family++)
    for (mask = 0; mask < 3; mask++)
    for (tex = 0; tex < 4; tex++)
    for (raw = 0; raw < 2; raw++) {
        setup(texels[tex], 2, 0, mask);
        if (mask == 2) g_vram[23][23] = 0xc210;
        submit(family, raw, 0, 0, 2, 0);
        expected = raw ? texels[tex] : tex == 1 ? 0x8000 :
                   tex == 2 ? 0x7e04 : 0xfe04;
        if (mask == 1) expected |= 0x8000;
        if (tex == 0) expected = 0x4210;
        if (mask == 2) expected = 0xc210;
        expect(family, "mask/transparent/black", 23, 23, expected);
    }
}

static void gradient_and_uv(void)
{
    u32 packet[13];
    int family, raw;
    for (family = 2; family < 4; family++)
    for (raw = 0; raw < 2; raw++) {
        setup(0x4210, 2, 0, 0);
        memcpy(packet, packets[family], sizeof(packet));
        packet[1] |= 0x808040u | ((u32)raw << 24);
        packet[4] = 0x808080u;
        packet[7] = 0x8080c0u;
        packet[10] = 0x808000u;
        packet[6] |= 0x01080000u;
        DrawOTag(packet);
        expect(family, "Gouraud ramp", 23, 23, raw ? 0x4210 : 0x420c);
        if (family == 3)
            expect(family, "Gouraud second face", 33, 33,
                   raw ? 0x4210 : 0x4207);
    }
    for (family = 0; family < 4; family++) {
        setup(0, 2, 0, 0);
        g_vram[131][515] = 0x6208;
        g_vram[141][525] = 0x001f;
        memcpy(packet, packets[family], sizeof(packet));
        packet[1] |= 0x01000000u; /* Raw: vertex colors are irrelevant. */
        if (family < 2) {
            packet[5] = 0x01088010u;
            packet[7] = 0x00009000u;
            packet[9] = 0x00009010u;
        } else {
            packet[6] = 0x01088010u;
            packet[9] = 0x00009000u;
            packet[12] = 0x00009010u;
        }
        DrawOTag(packet);
        expect(family, "varying UV", 23, 23, 0x6208);
        if (family & 1) expect(family, "second face UV", 33, 33, 0x001f);
    }
}

/* Untextured gouraud colour: both backends must produce the same pixels.
 * The three G4 packets are scene 64's message-window gradients (week_day,
 * GP0_TRACE_FRAME); Mednafen's colours equal the retail reference there.
 * The G3 set permutes one triangle's vertices (core-vertex choice) and
 * steepens its gradients.  Every pixel is compared: coverage (the fill
 * rule) and colour. */
static void gouraud_backends(void)
{
    static const u32 g4[3][9] = {
        {0x08ffffff, 0x38008C1E, 0x0107001C, 0x00007018, 0x01070124,
         0x00005412, 0x0131001C, 0x0000380C, 0x01310124},
        {0x08ffffff, 0x38008C1E, 0x0196001B, 0x00007018, 0x01960125,
         0x00005412, 0x01CA001B, 0x0000380C, 0x01CA0125},
        {0x08ffffff, 0x38008C1E, 0x011D0126, 0x00007018, 0x011D013A,
         0x00005412, 0x018F0126, 0x0000380C, 0x018F013A}
    };
    static const u32 v[3][2] = {
        {0x000000ff, 0x00040004}, {0x0000ff00, 0x00140064},
        {0x00ff0010, 0x004a001e}
    };
    static const int order[6][3] = {
        {0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}
    };
    static u16 first[480][320];
    u32 packet[9];
    int dither, backend, i, x, y;

    for (dither = 0; dither < 2; dither++) {
        for (backend = 0; backend < 2; backend++) {
            g_use_mdfn_gpu = backend;
            mdfn_gpu_init(&g_vram[0][0]);
            for (y = 0; y < 480; y++)
                for (x = 0; x < 320; x++) g_vram[y][x] = 0x8000;
            {
                u32 clear[2] = {0x01ffffffu, 0x01000000u};
                DrawOTag(clear);
            }
            assert(gpu_draw_command(0xe1000000u | (dither ? 0x200u : 0)));
            assert(gpu_draw_command(0xe3000000u));
            assert(gpu_draw_command(0xe4000000u | 319u | (479u << 10)));
            assert(gpu_draw_command(0xe5000000u));
            assert(gpu_draw_command(0xe6000000u));
            for (i = 0; i < 3; i++) {
                memcpy(packet, g4[i], sizeof(packet));
                DrawOTag(packet);
            }
            for (i = 0; i < 6; i++) {
                int k;
                packet[0] = 0x06ffffffu;
                for (k = 0; k < 3; k++) {
                    const u32 *p = v[order[i][k]];
                    packet[1 + 2 * k] = p[0] | (k ? 0 : 0x30000000u);
                    /* each permutation in its own cell of a 3 x 2 grid
                     * above the G4 windows: no overlaps */
                    packet[2 + 2 * k] = p[1] + (u32)((i % 3) * 104) +
                                        (u32)((i / 3) * 100 * 0x10000);
                }
                DrawOTag(packet);
            }
            for (y = 0; y < 480; y++)
                for (x = 0; x < 320; x++) {
                    if (!backend) {
                        first[y][x] = g_vram[y][x];
                    } else if (g_vram[y][x] != first[y][x]) {
                        fprintf(stderr, "dither=%d gouraud at %d,%d: "
                                "native %04x mednafen %04x\n", dither, x, y,
                                first[y][x], g_vram[y][x]);
                        abort();
                    }
                }
        }
    }
}

static void short_packets(void)
{
    u32 packet[2];
    int family;
    for (family = 0; family < 4; family++) {
        setup(0x6208, 2, 1, 0);
        packet[0] = 0x01ffffffu;
        packet[1] = packets[family][1];
        DrawOTag(packet);
        assert(g_tpage_x == 3 && g_tpage_bpp == 1);
        assert(g_frame_poly_count == 0);
        expect(family, "short packet", 23, 23, 0x4210);
    }
}

/* GsSortSprite's fast path links ONE packet: E1 draw mode, then SPRT.
 * Retail DMA feeds every word to GP0, so the sprite is drawn with the
 * page that its own prefix selected.  The same holds for a polygon. */
static void state_prefixed(void)
{
    static const u16 texel = 0x6208;
    u32 sprite[6], poly[11];
    int i;
    setup(texel, 2, 0, 0);
    g_vram[128][513] = g_vram[129][512] = g_vram[129][513] = texel;
    sprite[0] = 0x05ffffffu;
    sprite[1] = 0xe1000108u; /* page 8, 15-bit */
    sprite[2] = 0x65000000u; /* raw textured sprite */
    sprite[3] = 50u | (50u << 16);
    sprite[4] = (128u << 8) | ((u32)((400u << 6) | 4u) << 16);
    sprite[5] = 2u | (2u << 16);
    DrawOTag(sprite);
    assert(g_tpage_x == 8 && g_tpage_bpp == 2);
    expect(4, "E1-prefixed sprite", 50, 50, texel);
    expect(4, "E1-prefixed sprite", 51, 51, texel);
    expect(4, "beside E1-prefixed sprite", 52, 52, 0x4210);

    setup(texel, 2, 0, 0);
    poly[0] = 0x0affffffu;
    poly[1] = 0xe1000083u; /* a different page: the polygon's own wins */
    for (i = 1; i < 10; i++) poly[i + 1] = packets[1][i];
    poly[2] |= 0x01000000u; /* raw */
    poly[4] |= (u32)((400u << 6) | 4u) << 16;
    poly[6] |= 0x108u << 16;
    DrawOTag(poly);
    assert(g_tpage_x == 8 && g_tpage_bpp == 2);
    expect(5, "E1-prefixed polygon", 23, 23, texel);
    expect(5, "E1-prefixed polygon second face", 33, 33, texel);

    /* State-only packets still draw nothing. */
    setup(texel, 2, 0, 0);
    sprite[0] = 0x02ffffffu;
    sprite[1] = 0xe1000108u;
    sprite[2] = 0xe2000000u;
    DrawOTag(sprite);
    assert(g_tpage_x == 8 && g_tpage_bpp == 2);
    expect(6, "state only", 50, 50, 0x4210);
}

/* One DMA packet is one GP0 word stream: every command in it executes,
 * each consuming exactly its own word count, with state words applied in
 * order between primitives (PSX-SPX command formats). */
static void several_commands(void)
{
    static const u16 a = 0x6208, b = 0x001f;
    u32 packet[40];
    int n;
    setup(a, 2, 0, 0);
    g_vram[128][576] = b; /* page 9, UV (0,128) */
    n = 1;
    packet[n++] = 0xe1000108u;            /* page 8, 15-bit */
    packet[n++] = 0xe2000000u;
    packet[n++] = 0x65000000u;            /* raw SPRT, 1x1 */
    packet[n++] = 40u | (40u << 16);
    packet[n++] = 128u << 8;
    packet[n++] = 1u | (1u << 16);
    packet[n++] = 0xe1000109u;            /* page 9 for the next sprite */
    packet[n++] = 0x65000000u;
    packet[n++] = 42u | (40u << 16);
    packet[n++] = 128u << 8;
    packet[n++] = 1u | (1u << 16);
    packet[n++] = 0x6800f800u;            /* TILE_1, green */
    packet[n++] = 44u | (40u << 16);
    packet[n++] = 0x48000000u;            /* flat polyline, 3 vertices */
    packet[n++] = 0x68006800u;            /* vertex words that would be */
    packet[n++] = 0x68006800u;            /* TILE_1 commands if misread */
    packet[n++] = 0x68006800u;
    packet[n++] = 0x55555555u;            /* terminator */
    packet[n++] = 0x6800f800u;            /* TILE_1 after the polyline */
    packet[n++] = 46u | (40u << 16);
    packet[n++] = 0xa0000000u;            /* upload 2x1 at (600,128) */
    packet[n++] = 600u | (128u << 16);
    packet[n++] = 2u | (1u << 16);
    packet[n++] = (u32)a | ((u32)b << 16);
    packet[n++] = 0x65000000u;            /* sprite samples the upload */
    packet[n++] = 48u | (40u << 16);
    packet[n++] = (128u << 8) | 24u;
    packet[n++] = 2u | (1u << 16);
    packet[n++] = 0x65000000u;            /* incomplete: two words only */
    packet[n++] = 52u | (40u << 16);
    packet[0] = ((u32)(n - 1) << 24) | 0xffffffu;
    DrawOTag(packet);
    expect(7, "first sprite, page 8", 40, 40, a);
    expect(7, "second sprite, page 9", 42, 40, b);
    expect(7, "tile after sprites", 44, 40, 0x03e0);
    expect(7, "tile after polyline", 46, 40, 0x03e0);
    expect(7, "uploaded texels", 600, 128, a);
    expect(7, "uploaded texels", 601, 128, b);
    expect(7, "sprite from upload", 48, 40, a);
    expect(7, "sprite from upload", 49, 40, b);
    expect(7, "incomplete sprite", 52, 40, 0x4210);
    /* Misread vertex words would draw a TILE_1 at (0,0). */
    expect(7, "no misread vertex", 0, 0, 0x4210);
    assert(g_tpage_x == 9 && g_tpage_bpp == 2);

    /* A primitive-led packet runs its later commands too. */
    setup(a, 2, 0, 0);
    assert(gpu_draw_command(0xe1000108u));
    packet[0] = 0x06ffffffu;
    packet[1] = 0x65000000u;
    packet[2] = 40u | (40u << 16);
    packet[3] = 128u << 8;
    packet[4] = 1u | (1u << 16);
    packet[5] = 0x6800f800u;
    packet[6] = 42u | (40u << 16);
    DrawOTag(packet);
    expect(8, "leading sprite", 40, 40, a);
    expect(8, "tile after leading sprite", 42, 40, 0x03e0);
}

void nbn_trace_load_image(const void *data, u32 bytes, s32 x, s32 y,
                          s32 w, s32 h)
{
    (void)data; (void)bytes; (void)x; (void)y; (void)w; (void)h;
}
void nbn_trace_move_image(s32 x, s32 y, s32 w, s32 h, s32 dx, s32 dy)
{
    (void)x; (void)y; (void)w; (void)h; (void)dx; (void)dy;
}

/* Submit `count` GP0 words as one DMA packet. */
static void gp0(const u32 *words, int count)
{
    u32 packet[16];
    assert(count < 16);
    packet[0] = ((u32)count << 24) | 0xffffffu;
    memcpy(packet + 1, words, (size_t)count * 4);
    DrawOTag(packet);
}

/* Raw 1x1 sprite at (x,40) sampling UV (0,128) with its own E1 prefix. */
static u16 sample(int x, u32 page, u16 clut)
{
    u32 words[5];
    words[0] = page;
    words[1] = 0x65000000u;
    words[2] = (u32)x | (40u << 16);
    words[3] = (128u << 8) | ((u32)clut << 16);
    words[4] = 1u | (1u << 16);
    gp0(words, 5);
    return g_vram[40][x];
}

static void upload(u32 x, u32 y, u16 value)
{
    u32 words[4] = {0xa0000000u, 0, 0x00010001u, 0};
    words[1] = x | (y << 16);
    words[3] = value;
    gp0(words, 4);
}

static void clear_cache(void)
{
    u32 word = 0x01000000u;
    gp0(&word, 1);
}

#define PAGE15 0xe1000109u /* page 9 (VRAM 576,0), 15-bit */
#define PAGE8  0xe1000089u
#define PAGE4  0xe1000009u
#define CLUT_A ((401u << 6) | 4u)  /* (64,401) */
#define CLUT_B ((401u << 6) | 5u)  /* (80,401) */

/* Warm-cache lifetimes: what clears the texture and CLUT caches and what
 * does not (pc/hal/gpu.c texel_fetch has the evidence).  A sampler without
 * caches fails every "stale" step; one that never clears its texture cache
 * fails every "fresh" step; one that reloads the CLUT per primitive fails
 * the stale-CLUT steps. */
static void cache_lifetime(void)
{
    static const u16 A = 0x6208, B = 0x1234, C = 0x2345, D = 0x3456,
                     E = 0x4567;
    u32 words[3];
    RECT rect;
    u16 value;
    int x = 0;
    setup(0x4210, 2, 0, 0);
    assert(gpu_draw_command(0xe4000000u | 1023u | (511u << 10)));

    g_vram[128][576] = A;
    clear_cache();
    expect(9, "cold", x, 40, (sample(x, PAGE15, 0), A)); x++;
    /* GP0(02h) fill: no invalidation. */
    words[0] = 0x02000000u | 0x00f800u;
    words[1] = 576u | (128u << 16);
    words[2] = 16u | (1u << 16);
    gp0(words, 3);
    assert(g_vram[128][576] == 0x03e0);
    expect(9, "stale after fill", x, 40, (sample(x, PAGE15, 0), A)); x++;
    clear_cache();
    expect(9, "fresh after 01h", x, 40, (sample(x, PAGE15, 0), 0x03e0)); x++;
    /* Drawing into the texture: no invalidation. */
    words[0] = 0x680000f8u;
    words[1] = 576u | (128u << 16);
    gp0(words, 2);
    assert(g_vram[128][576] == 0x001f);
    expect(9, "stale after drawing", x, 40, (sample(x, PAGE15, 0), 0x03e0));
    x++;
    /* GP0(A0h) upload: texture cache cleared. */
    upload(576, 128, B);
    expect(9, "fresh after A0h", x, 40, (sample(x, PAGE15, 0), B)); x++;
    /* MoveImage (GP0 80h): texture cache cleared. */
    g_vram[130][580] = C;
    rect.x = 580; rect.y = 130; rect.w = 1; rect.h = 1;
    MoveImage(&rect, 576, 128);
    assert(g_vram[128][576] == C);
    expect(9, "fresh after MoveImage", x, 40, (sample(x, PAGE15, 0), C)); x++;
    /* LoadImage (01h + A0h). */
    value = D;
    rect.x = 576; rect.y = 128;
    LoadImage(&rect, (u32 *)&value);
    expect(9, "fresh after LoadImage", x, 40, (sample(x, PAGE15, 0), D)); x++;
    /* 15-bit <-> 8-bit on one page and E2 keep the texture cache. */
    g_vram[128][576] = E;
    assert(gpu_draw_command(PAGE8));
    assert(gpu_draw_command(0xe2000421u));
    assert(gpu_draw_command(0xe2000000u));
    expect(9, "stale after depth/window", x, 40,
           (sample(x, PAGE15, 0), D)); x++;
    /* A page change clears it. */
    assert(gpu_draw_command(0xe1000108u));
    expect(9, "fresh after page change", x, 40,
           (sample(x, PAGE15, 0), E)); x++;

    /* CLUT cache: index 1 at (576,128), entry 1 of CLUT_A at (65,401). */
    g_vram[128][576] = 0x0001;
    g_vram[401][65] = A;
    g_vram[401][81] = B;
    clear_cache();
    expect(10, "4-bit cold", x, 40, (sample(x, PAGE4, CLUT_A), A)); x++;
    /* A0h clears the texture cache only: the CLUT stays cached. */
    upload(65, 401, C);
    expect(10, "CLUT stale after A0h", x, 40,
           (sample(x, PAGE4, CLUT_A), A)); x++;
    /* LoadImage sends 01h first. */
    value = D;
    rect.x = 65; rect.y = 401;
    LoadImage(&rect, (u32 *)&value);
    expect(10, "CLUT fresh after LoadImage", x, 40,
           (sample(x, PAGE4, CLUT_A), D)); x++;
    /* Another CLUT, then back: reloaded. */
    upload(65, 401, E);
    expect(10, "CLUT stale again", x, 40,
           (sample(x, PAGE4, CLUT_A), D)); x++;
    expect(10, "other CLUT", x, 40, (sample(x, PAGE4, CLUT_B), B)); x++;
    expect(10, "CLUT reloaded after switch", x, 40,
           (sample(x, PAGE4, CLUT_A), E)); x++;
    /* Same CLUT at 8-bit depth: a different cache key, reloaded. */
    upload(65, 401, C);
    expect(10, "CLUT reloaded for 8-bit", x, 40,
           (sample(x, PAGE8, CLUT_A), C)); x++;
}

/* Vertex decoding (PSX-SPX "Vertex"/"Rectangle" parameters, GP0(E5h)):
 * X/Y are signed 11-bit, the E5 offset is added to every vertex, and a
 * polygon whose vertices are 1024+ apart in X or 512+ in Y is not drawn.
 * Rectangles add the offset to their signed 11-bit corner. */
static void vertex_decoding(void)
{
    static const u16 texel = 0x6208;
    u32 packet[13], words[4];
    int family, high;
    for (family = 0; family < 4; family++)
    for (high = 0; high < 2; high++) {
        setup(texel, 2, 0, 0);
        /* Offset (3,2): the face sampled at 23,23 lands at 26,25. */
        assert(gpu_draw_command(0xe5000000u | 3u | (2u << 11)));
        memcpy(packet, packets[family], sizeof(packet));
        packet[1] |= 0x01000000u; /* raw */
        packet[3] |= (u32)((400u << 6) | 4u) << 16;
        packet[family < 2 ? 5 : 6] |= 0x108u << 16;
        /* Bits 11..15 are not part of the coordinate. */
        if (high) {
            packet[2] |= 0xf800f800u;
            packet[family < 2 ? 4 : 5] |= 0x08000800u;
        }
        DrawOTag(packet);
        expect(20 + family * 2 + high, "offset face", 26, 25, texel);
        expect(20 + family * 2 + high, "offset: origin side clear", 21, 21, 0x4210);
    }
    /* Oversized: vertices 1024 apart in X or 512 in Y; one less draws.
     * Flat F3 and Gouraud G4, green, sampled where the face would be. */
    for (family = 0; family < 2; family++)
    for (high = 0; high < 4; high++) {
        int yaxis = high >> 1, limit = high & 1;
        u32 far;
        u16 want = limit ? 0x4210 : 0x03e0;
        setup(texel, 2, 0, 0);
        /* X: v1 = (20-1024+!limit, 20); Y: v2 = (20, 20+512-!limit) */
        far = yaxis ? (20u | ((u32)(532 - !limit) << 16))
                    : ((u32)(u16)(s16)(20 - 1024 + !limit) | (20u << 16));
        if (family == 0) {
            words[0] = 0x2000f800u;
            words[1] = 20u | (20u << 16);
            words[2] = yaxis ? 36u | (20u << 16) : far;
            words[3] = yaxis ? far : 20u | (36u << 16);
            gp0(words, 4);
        } else {
            memset(packet, 0, sizeof(packet));
            packet[0] = 0x08ffffffu;
            packet[1] = 0x3800f800u;
            packet[2] = 20u | (20u << 16);
            packet[3] = 0x00f800u;
            packet[4] = yaxis ? 36u | (20u << 16) : far;
            packet[5] = 0x00f800u;
            packet[6] = yaxis ? far : 20u | (36u << 16);
            packet[7] = 0x00f800u;
            packet[8] = 36u | (36u << 16);
            DrawOTag(packet);
        }
        expect(30 + family * 4 + high, limit ? "oversized polygon" : "largest polygon",
               yaxis ? 22 : 10, 22, want);
    }
    /* Spans starting left of X=0 are clipped, not lost, and keep their
     * interpolation: Gouraud red ramp from x=-16 (0) to x=16 (248), so
     * x=0 is about half red with no green/blue. */
    setup(texel, 2, 0, 0);
    memset(packet, 0, sizeof(packet));
    packet[0] = 0x06ffffffu;
    packet[1] = 0x30000000u;
    packet[2] = 0xfff0u | (20u << 16);
    packet[3] = 0x0000f8u;
    packet[4] = 16u | (20u << 16);
    packet[5] = 0;
    packet[6] = 0xfff0u | (36u << 16);
    DrawOTag(packet);
    if ((g_vram[22][0] >> 5) != 0 || (g_vram[22][0] & 31) < 12 ||
        (g_vram[22][0] & 31) > 16) {
        fprintf(stderr, "backend=%d left-clipped span at 0,22: %04x\n",
                g_use_mdfn_gpu, g_vram[22][0]);
        abort();
    }

    /* Untextured TILE_1 with the offset, and an 11-bit corner. */
    setup(texel, 2, 0, 0);
    assert(gpu_draw_command(0xe5000000u | 3u | (2u << 11)));
    words[0] = 0x6800f800u;
    words[1] = 10u | (10u << 16) | 0xf800f800u;
    gp0(words, 2);
    expect(13, "offset tile", 13, 12, 0x03e0);
    expect(13, "offset tile origin", 10, 10, 0x4210);
}

/* Untextured polygon coverage (the GPU's fill rule, PSX-SPX "Render
 * Polygon Commands"; Mednafen DrawTriangle): rows from the top vertex to
 * the row above the bottom one, each from its left edge to the column
 * before its right edge.  A quad is the triangles (v0,v1,v2), (v1,v2,v3)
 * and their shared edge belongs to exactly one, so a semi-transparent
 * quad blends every covered pixel once. */
static void edge_setup(int area_right, int area_bottom)
{
    int x, y;
    mdfn_gpu_init(&g_vram[0][0]);
    for (y = 0; y < 256; y++)
        for (x = 0; x < 320; x++) g_vram[y][x] = 0x4210;
    {
        u32 clear[2] = {0x01ffffffu, 0x01000000u};
        DrawOTag(clear);
    }
    assert(gpu_draw_command(0xe1000000u));
    assert(gpu_draw_command(0xe3000000u));
    assert(gpu_draw_command(0xe4000000u | (u32)area_right |
                            ((u32)area_bottom << 10)));
    assert(gpu_draw_command(0xe5000000u));
    assert(gpu_draw_command(0xe6000000u));
}

static u32 xy(int x, int y)
{
    return (u32)(u16)(s16)x | ((u32)(u16)(s16)y << 16);
}

static void edge_rules(void)
{
    u32 w[8];
    int x, y, count, inside;

    /* Opaque red F4 with corners (20,20) and (30,30): exactly 10 x 10. */
    edge_setup(319, 255);
    w[0] = 0x280000f8u; w[1] = xy(20, 20); w[2] = xy(30, 20);
    w[3] = xy(20, 30); w[4] = xy(30, 30);
    gp0(w, 5);
    for (y = 18; y < 33; y++)
        for (x = 18; x < 33; x++) {
            inside = x >= 20 && x < 30 && y >= 20 && y < 30;
            expect(40, inside ? "F4 face" : "F4 outside right/bottom",
                   x, y, inside ? 0x001f : 0x4210);
        }

    /* Right isosceles G3 (0,0),(8,0),(0,8) at (100,100): rows 0..7 of
     * width 8-y, 36 pixels; the hypotenuse's own pixels are not drawn. */
    w[0] = 0x30f8f8f8u; w[1] = xy(100, 100); w[2] = 0x00f8f8f8u;
    w[3] = xy(108, 100); w[4] = 0x00f8f8f8u; w[5] = xy(100, 108);
    gp0(w, 6);
    count = 0;
    for (y = 96; y < 112; y++)
        for (x = 96; x < 112; x++)
            if (g_vram[y][x] != 0x4210) {
                if (x - 100 >= 108 - y || x < 100 || y < 100) {
                    fprintf(stderr, "backend=%d G3 pixel %d,%d outside\n",
                            g_use_mdfn_gpu, x, y);
                    abort();
                }
                count++;
            }
    if (count != 36) {
        fprintf(stderr, "backend=%d G3 covers %d pixels, not 36\n",
                g_use_mdfn_gpu, count);
        abort();
    }

    /* Semi-transparent additive F4s (E1 ABR=1): colour 8 adds one step to
     * the 16/16/16 background, so every covered pixel is exactly 0x4631;
     * a pixel drawn by both triangles would be 0x4a52.  An axis-aligned
     * quad and a skewed one whose diagonal crosses many pixels. */
    edge_setup(319, 255);
    assert(gpu_draw_command(0xe1000020u));
    w[0] = 0x2a080808u; w[1] = xy(20, 20); w[2] = xy(30, 20);
    w[3] = xy(20, 30); w[4] = xy(30, 30);
    gp0(w, 5);
    w[1] = xy(40, 20); w[2] = xy(61, 26); w[3] = xy(44, 41);
    w[4] = xy(66, 47);
    gp0(w, 5);
    count = 0;
    for (y = 0; y < 64; y++)
        for (x = 0; x < 80; x++) {
            if (g_vram[y][x] == 0x4210) continue;
            if (g_vram[y][x] != 0x4631) {
                fprintf(stderr, "backend=%d blended quad at %d,%d: %04x "
                        "(shared edge drawn twice?)\n", g_use_mdfn_gpu, x, y,
                        g_vram[y][x]);
                abort();
            }
            count++;
        }
    if (g_vram[25][25] != 0x4631 || g_vram[30][46] != 0x4631 ||
        g_vram[33][58] != 0x4631) {
        fprintf(stderr, "backend=%d blended quad faces missing\n",
                g_use_mdfn_gpu);
        abort();
    }
    /* The axis-aligned quad contributes exactly 100. */
    for (y = 20; y < 30; y++)
        for (x = 20; x < 30; x++)
            expect(41, "blended F4 face", x, y, 0x4631);
    (void)count;
}

/* Both backends render the same pseudo-random untextured F3/F4/G3/G4
 * stream (opaque and all four semi-transparency modes, dither on and off,
 * drawing offset, a drawing area clipping polygons that start outside it)
 * and every pixel must agree: coverage and colour. */
static void edge_backends(void)
{
    static const u32 codes[8] = {0x20, 0x22, 0x28, 0x2a, 0x30, 0x32,
                                 0x38, 0x3a};
    static u16 first[256][320];
    u32 seed, w[9];
    int backend, i, k, x, y, n;

    for (backend = 0; backend < 2; backend++) {
        g_use_mdfn_gpu = backend;
        edge_setup(300, 230);
        for (y = 0; y < 256; y++)
            for (x = 0; x < 320; x++)
                g_vram[y][x] = (u16)((x * 7 + y * 13) & 0x7fff);
        assert(gpu_draw_command(0xe3000000u | 5u | (3u << 10)));
        seed = 12345u;
        for (i = 0; i < 400; i++) {
            u32 code;
            int vertices, gouraud;
#define EDGE_RAND() (seed = seed * 1103515245u + 12345u, (seed >> 8) & 0xffff)
            code = codes[EDGE_RAND() & 7];
            vertices = (code & 8) ? 4 : 3;
            gouraud = (code & 0x10) != 0;
            assert(gpu_draw_command(0xe1000000u |
                                    ((EDGE_RAND() & 3u) << 5) |
                                    ((EDGE_RAND() & 1u) << 9)));
            assert(gpu_draw_command(0xe5000000u |
                                    ((EDGE_RAND() & 15u)) |
                                    ((EDGE_RAND() & 7u) << 11)));
            n = 0;
            {
                int cx = (int)(EDGE_RAND() % 340u) - 10;
                int cy = (int)(EDGE_RAND() % 270u) - 10;
                int size = 1 + (int)(EDGE_RAND() % 60u);
                for (k = 0; k < vertices; k++) {
                    if (k == 0 || gouraud)
                        w[n++] = (k ? 0 : code << 24) | (EDGE_RAND() << 8 & 0xff00u) |
                                 (EDGE_RAND() & 0xffu) | ((EDGE_RAND() & 0xffu) << 16);
                    w[n++] = xy(cx + (int)(EDGE_RAND() % (u32)(2 * size + 1)) - size,
                                cy + (int)(EDGE_RAND() % (u32)(2 * size + 1)) - size);
                }
            }
#undef EDGE_RAND
            gp0(w, n);
        }
        for (y = 0; y < 256; y++)
            for (x = 0; x < 320; x++) {
                if (!backend) {
                    first[y][x] = g_vram[y][x];
                } else if (g_vram[y][x] != first[y][x]) {
                    fprintf(stderr, "untextured stream at %d,%d: native %04x "
                            "mednafen %04x\n", x, y, first[y][x],
                            g_vram[y][x]);
                    abort();
                }
            }
    }
}

/* Textured polygon interpolation: UV and colour step from the core vertex
 * as 12.12 plane gradients with a half-unit bias (as untextured colour).
 * A 16x16 FT4 mapped 1:1 samples texel (u0+x, v0+y) at every pixel; the
 * same quad as GT4 with one vertex colour per corner modulates by the
 * fixed-point colour. */
static void tex_seed(void)
{
    u32 seed = 777u;
    int x, y;
    for (y = 256; y < 512; y++)
        for (x = 0; x < 1024; x++) {
            seed = seed * 1103515245u + 12345u;
            /* about 1/16 transparent, 1/4 with STP */
            g_vram[y][x] = ((seed >> 12) & 15u) == 0 ? 0 :
                (u16)((seed >> 8) & 0xffffu);
        }
    {
        u32 clear[2] = {0x01ffffffu, 0x01000000u};
        DrawOTag(clear);
    }
}

static void tex_rules(void)
{
    u32 w[12];
    int x, y;
    edge_setup(319, 255);
    tex_seed();
    /* 15-bit page at (512,256): tpage 0x118 (x 8, y 1, depth 2). */
    w[0] = 0x2d808080u; w[1] = xy(40, 40); w[2] = 0x00000403u;
    w[3] = xy(56, 40); w[4] = 0x01180413u;
    w[5] = xy(40, 56); w[6] = 0x00001403u;
    w[7] = xy(56, 56); w[8] = 0x00001413u;
    gp0(w, 9);
    for (y = 38; y < 58; y++)
        for (x = 38; x < 58; x++) {
            int inside = x >= 40 && x < 56 && y >= 40 && y < 56;
            u16 want = inside ? g_vram[256 + 4 + y - 40][512 + 3 + x - 40]
                              : 0x4210;
            if (inside && !want) want = 0x4210; /* transparent texel */
            expect(50, inside ? "1:1 FT4 texel" : "1:1 FT4 outside",
                   x, y, want);
        }
}

/* The save menu's album list (date_save.txt, shot save_confirm), replayed
 * from its GP0_TRACE_FRAME dump: the page is a gouraud F4-shaped G4 (E0
 * grey to B3) at (0..283, 273..398), then E1 240h (ABR 2 = B-F, dither on)
 * and 13 semi-transparent G4s of colour 808080 -- the greyed-out albums.
 * The words are verbatim, including the pad bytes of the second..fourth
 * colour words (00/78/64/7E/E1/68/38/01: whatever the packet buffer held;
 * the GPU ignores them).  An earlier native rasterizer left the quads'
 * shared diagonal unfilled, which showed as a thin light diagonal across
 * each grey slot.  Rule (PSX-SPX polygon notes, quads are two triangles
 * whose shared edge is drawn once): every covered pixel is the page minus
 * ONE foreground subtraction, 0x80 dithered to 15 or 16 per 5-bit channel;
 * outside the slots the page is untouched. */
static const u32 album_page[8] = {
    0x38E0E0E0u, 0x01110000u, 0x78E0E0E0u, 0x0111011Bu,
    0x7CE0E0E0u, 0x018E0000u, 0x7EB3B3B3u, 0x018E011Bu};
static const u32 album_slots[13][8] = {
    {0x3A808080u, 0x01580064u, 0x00808080u, 0x01580082u, 0x64808080u, 0x01670064u, 0x78808080u, 0x01670082u},
    {0x3A808080u, 0x01480064u, 0x78808080u, 0x01480082u, 0x64808080u, 0x01570064u, 0x78808080u, 0x01570082u},
    {0x3A808080u, 0x01380064u, 0x78808080u, 0x01380082u, 0x64808080u, 0x01470064u, 0x78808080u, 0x01470082u},
    {0x3A808080u, 0x01280064u, 0x78808080u, 0x01280082u, 0x64808080u, 0x01370064u, 0x78808080u, 0x01370082u},
    {0x3A808080u, 0x01680044u, 0x78808080u, 0x01680062u, 0x68808080u, 0x01770044u, 0x00808080u, 0x01770062u},
    {0x3A808080u, 0x01580044u, 0x7E808080u, 0x01580062u, 0x64808080u, 0x01670044u, 0x78808080u, 0x01670062u},
    {0x3A808080u, 0x01480044u, 0x78808080u, 0x01480062u, 0xE1808080u, 0x01570044u, 0x78808080u, 0x01570062u},
    {0x3A808080u, 0x01380044u, 0x78808080u, 0x01380062u, 0xE1808080u, 0x01470044u, 0x78808080u, 0x01470062u},
    {0x3A808080u, 0x01280044u, 0x00808080u, 0x01280062u, 0x38808080u, 0x01370044u, 0x78808080u, 0x01370062u},
    {0x3A808080u, 0x01680024u, 0x78808080u, 0x01680042u, 0x00808080u, 0x01770024u, 0x00808080u, 0x01770042u},
    {0x3A808080u, 0x01580024u, 0x00808080u, 0x01580042u, 0x68808080u, 0x01670024u, 0x78808080u, 0x01670042u},
    {0x3A808080u, 0x01480024u, 0x78808080u, 0x01480042u, 0x64808080u, 0x01570024u, 0x78808080u, 0x01570042u},
    {0x3A808080u, 0x01380024u, 0x78808080u, 0x01380042u, 0x01808080u, 0x01470024u, 0x00808080u, 0x01470042u}};

static void album_slot_rule(void)
{
    static u16 page[512][320];
    int x, y, i, c, inside;
    for (i = 0; i < 2; i++) {
        edge_setup(319, 255);
        assert(gpu_draw_command(0xe4000000u | 319u | (511u << 10)));
        assert(gpu_draw_command(0xe1000200u));
        gp0(album_page, 8);
        if (i == 0) {
            for (y = 0; y < 512; y++)
                for (x = 0; x < 320; x++) page[y][x] = g_vram[y][x];
            continue;
        }
        assert(gpu_draw_command(0xe1000240u));
        for (c = 0; c < 13; c++) gp0(album_slots[c], 8);
    }
    for (y = 240; y < 420; y++)
        for (x = 0; x < 320; x++) {
            inside = 0;
            for (c = 0; c < 13; c++) {
                int x0 = (int)(album_slots[c][1] & 0xffffu);
                int y0 = (int)(album_slots[c][1] >> 16);
                int x1 = (int)(album_slots[c][3] & 0xffffu);
                int y1 = (int)(album_slots[c][5] >> 16);
                if (x >= x0 && x < x1 && y >= y0 && y < y1) inside = 1;
            }
            if (!inside) {
                if (g_vram[y][x] != page[y][x]) {
                    fprintf(stderr, "backend=%d album page touched at %d,%d: "
                            "%04x != %04x\n", g_use_mdfn_gpu, x, y,
                            g_vram[y][x], page[y][x]);
                    abort();
                }
                continue;
            }
            for (c = 0; c < 15; c += 5) {
                int before = (page[y][x] >> c) & 31;
                int after = (g_vram[y][x] >> c) & 31;
                int sub = before - after;
                if (sub != 15 && sub != 16) {
                    fprintf(stderr, "backend=%d album slot at %d,%d channel %d:"
                            " page %d -> %d (want one subtraction of 15/16)\n",
                            g_use_mdfn_gpu, x, y, c / 5, before, after);
                    abort();
                }
            }
        }
}

/* VRAM is 1024 words wide and texture/CLUT addressing wraps at the right
 * edge to X & 1023 of the same row (PSX-SPX "Wrapping": no carry from X
 * into Y; Mednafen GetTexel/Update_CLUT_Cache, the reference emulator % VRAM_WIDTH).
 * Page 15 (X=960) in 15-bit mode covers X 960..1215, in 8-bit mode
 * 960..1087; a CLUT at X=1008 holds entries 0..15 at 1008..1023 and the
 * rest from X=0.  Every expected value below is computed from that rule
 * against seeded VRAM, never read back from a rasterizer.  The row past
 * the wrap (Y+1 at X 0..) holds a different pattern, so a carry into Y
 * fails too. */
static u16 wrap_word(int x, int y) { return (u16)(0x8001u + x * 37u + y * 11u) | 1u; }

static void wrap_seed(void)
{
    int x, y;
    for (y = 256; y < 512; y++)
        for (x = 0; x < 1024; x++) g_vram[y][x] = wrap_word(x, y);
    /* 8-bit palette at (1008,400): entry e at X (1008+e) & 1023. */
    for (x = 0; x < 256; x++)
        g_vram[400][(1008 + x) & 1023] = (u16)(0x0400u + x) | 1u;
    /* 8-bit index words on row 256+40.. of page 15: byte = column. */
    for (y = 296; y < 312; y++)
        for (x = 0; x < 1024; x++)
            g_vram[y][x] = (u16)(((x * 2) & 255) | (((x * 2 + 1) & 255) << 8));
    {
        u32 clear[2] = {0x01ffffffu, 0x01000000u};
        DrawOTag(clear);
    }
}

static void wrap_expect(const char *stage, int x, int y, u16 want)
{
    if (!want) want = 0x4210; /* transparent texel leaves the background */
    expect(60, stage, x, y, want);
}

static void tex_vram_wrap(void)
{
    u32 w[12];
    int x, y;

    /* 1. Raw FT4, 15-bit page 15 (tpage 0x11F), u 56..71 v 4..19:
     * texel X = (960 + u) & 1023, i.e. 1016..1023 then 0..7. */
    edge_setup(319, 255);
    wrap_seed();
    w[0] = 0x2d808080u; w[1] = xy(40, 40); w[2] = 0x00000438u;
    w[3] = xy(56, 40); w[4] = 0x011f0448u;
    w[5] = xy(40, 56); w[6] = 0x00001438u;
    w[7] = xy(56, 56); w[8] = 0x00001448u;
    gp0(w, 9);
    for (y = 40; y < 56; y++)
        for (x = 40; x < 56; x++)
            wrap_expect("15-bit page 15 wraps", x, y,
                        g_vram[256 + 4 + y - 40][(960 + 56 + x - 40) & 1023]);

    /* 2. Raw SPRT (65h) on the same page via E1, 16x4 at u 60: the
     * sprite path samples through the same fetch. */
    assert(gpu_draw_command(0xe100011fu));
    w[0] = 0x65808080u; w[1] = xy(100, 40); w[2] = 0x0000083cu;
    w[3] = 16u | (4u << 16);
    gp0(w, 4);
    for (y = 40; y < 44; y++)
        for (x = 100; x < 116; x++)
            wrap_expect("15-bit sprite wraps", x, y,
                        g_vram[256 + 8 + y - 40][(960 + 60 + x - 100) & 1023]);

    /* 3. Raw FT4, 8-bit page 15 (tpage 0x09F), CLUT (1008,400), u 120..135
     * v 40..55: index word X = (960 + u/2) & 1023 = 1020..1023 then 0..3;
     * index = the byte, palette entry at X (1008 + index) & 1023. */
    w[0] = 0x2d808080u; w[1] = xy(40, 100);
    w[2] = 0x00002878u | ((u32)((400u << 6) | 63u) << 16);
    w[3] = xy(56, 100); w[4] = 0x009f2888u;
    w[5] = xy(40, 116); w[6] = 0x00003878u;
    w[7] = xy(56, 116); w[8] = 0x00003888u;
    gp0(w, 9);
    for (y = 100; y < 116; y++)
        for (x = 40; x < 56; x++) {
            int u = 120 + x - 40;
            u16 word = g_vram[256 + 40 + y - 100][(960 + u / 2) & 1023];
            int index = (word >> ((u & 1) * 8)) & 255;
            wrap_expect("8-bit page 15 + CLUT wrap", x, y,
                        g_vram[400][(1008 + index) & 1023]);
        }

    /* 4. Texture window after the page: E2 mask_x 8, offset_x 8 forces
     * u bit 6, so u 0..15 samples u 64..79 -> X 1024..1039 -> 0..15. */
    assert(gpu_draw_command(0xe2000000u | 8u | (8u << 10)));
    w[0] = 0x2d808080u; w[1] = xy(100, 100); w[2] = 0x00000c00u;
    w[3] = xy(116, 100); w[4] = 0x011f0c10u;
    w[5] = xy(100, 116); w[6] = 0x00001c00u;
    w[7] = xy(116, 116); w[8] = 0x00001c10u;
    gp0(w, 9);
    for (y = 100; y < 116; y++)
        for (x = 100; x < 116; x++)
            wrap_expect("texture window then page wrap", x, y,
                        g_vram[256 + 12 + y - 100][(960 + 64 + x - 100) & 1023]);
    assert(gpu_draw_command(0xe2000000u));
}

/* Both backends render the same pseudo-random textured stream: FT3/FT4/
 * GT3/GT4, raw and modulated, semi-transparent, 4/8/15-bit pages and
 * CLUTs, texture windows, dither on and off, offsets and a clipping
 * drawing area.  Every pixel must agree. */
static void tex_backends(void)
{
    static const u32 codes[8] = {0x24, 0x25, 0x27, 0x2c, 0x2e, 0x34, 0x36,
                                 0x3c};
    static u16 first[256][320];
    u32 seed, w[13];
    int backend, i, k, x, y, n;

    for (backend = 0; backend < 2; backend++) {
        g_use_mdfn_gpu = backend;
        edge_setup(300, 230);
        for (y = 0; y < 256; y++)
            for (x = 0; x < 320; x++)
                g_vram[y][x] = (u16)((x * 7 + y * 13) & 0x7fff);
        tex_seed();
        assert(gpu_draw_command(0xe3000000u | 5u | (3u << 10)));
        seed = 4242u;
        for (i = 0; i < 400; i++) {
            u32 code, page, clut;
            int vertices, gouraud;
#define TEX_RAND() (seed = seed * 1103515245u + 12345u, (seed >> 8) & 0xffff)
            code = codes[TEX_RAND() & 7];
            if (TEX_RAND() & 1) code |= 1u;           /* raw */
            vertices = (code & 8) ? 4 : 3;
            gouraud = (code & 0x10) != 0;
            assert(gpu_draw_command(0xe1000000u | ((TEX_RAND() & 1u) << 9)));
            assert(gpu_draw_command(0xe5000000u | (TEX_RAND() & 15u) |
                                    ((TEX_RAND() & 7u) << 11)));
            if ((TEX_RAND() & 3) == 0)
                assert(gpu_draw_command(0xe2000000u | (TEX_RAND() & 0xfffffu)));
            else
                assert(gpu_draw_command(0xe2000000u));
            /* page x 8..15, y 1; depth 0..2; any ABR.  Pages 12..15
             * in 8/15-bit mode reach past X=1023 and wrap. */
            page = (8u + (TEX_RAND() & 7u)) | 0x10u | ((TEX_RAND() & 3u) << 5) |
                   ((TEX_RAND() % 3u) << 7);
            /* CLUT inside the seeded area: x 512..1008 (16-aligned; an
             * 8-bit CLUT past X=768 wraps), y 256..511 */
            clut = ((512u + (TEX_RAND() & 0x1f0u)) >> 4) |
                   ((256u + (TEX_RAND() & 0xffu)) << 6);
            n = 0;
            {
                int cx = (int)(TEX_RAND() % 340u) - 10;
                int cy = (int)(TEX_RAND() % 270u) - 10;
                int size = 1 + (int)(TEX_RAND() % 60u);
                int bu = (int)(TEX_RAND() & 0xffu), bv = (int)(TEX_RAND() & 0xffu);
                int span = 1 + (int)(TEX_RAND() % 90u);
                for (k = 0; k < vertices; k++) {
                    u32 uv;
                    if (k == 0 || gouraud)
                        w[n++] = (k ? 0 : code << 24) |
                                 (TEX_RAND() & 0xffu) |
                                 ((TEX_RAND() & 0xffu) << 8) |
                                 ((TEX_RAND() & 0xffu) << 16);
                    w[n++] = xy(cx + (int)(TEX_RAND() % (u32)(2 * size + 1)) - size,
                                cy + (int)(TEX_RAND() % (u32)(2 * size + 1)) - size);
                    uv = (u32)((bu + (int)(TEX_RAND() % (u32)span)) & 0xff) |
                         ((u32)((bv + (int)(TEX_RAND() % (u32)span)) & 0xff) << 8);
                    if (k == 0) uv |= clut << 16;
                    if (k == 1) uv |= page << 16;
                    w[n++] = uv;
                }
            }
#undef TEX_RAND
            gp0(w, n);
        }
        for (y = 0; y < 256; y++)
            for (x = 0; x < 320; x++) {
                if (!backend) {
                    first[y][x] = g_vram[y][x];
                } else if (g_vram[y][x] != first[y][x]) {
                    fprintf(stderr, "textured stream at %d,%d: native %04x "
                            "mednafen %04x\n", x, y, first[y][x],
                            g_vram[y][x]);
                    abort();
                }
            }
    }
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    g_use_mdfn_gpu = atoi(argv[1]);
    mdfn_gpu_init(&g_vram[0][0]);
    gpu_queue_reset(1);
    if (argv[2][0] == 'c') color_cases();
    else if (argv[2][0] == 'm') masks_and_black();
    else if (argv[2][0] == 'g') gradient_and_uv();
    else if (argv[2][0] == 'p') state_prefixed();
    else if (argv[2][0] == 'n') several_commands();
    else if (argv[2][0] == 'k') cache_lifetime();
    else if (argv[2][0] == 'v') vertex_decoding();
    else if (argv[2][0] == 'q') gouraud_backends();
    else if (argv[2][0] == 'e') edge_rules();
    else if (argv[2][0] == 'r') edge_backends();
    else if (argv[2][0] == 't') tex_rules();
    else if (argv[2][0] == 'u') tex_backends();
    else if (argv[2][0] == 'w') tex_vram_wrap();
    else if (argv[2][0] == 's') album_slot_rule();
    else short_packets();
    mdfn_gpu_shutdown();
    return 0;
}
'''


@unittest.skipUnless(HAVE_MDFN, 'EXCLUDED (needs the local Mednafen parity sources: '
                     'pc/3rd/mednafen-gpu/shim/mdfn_gpu_shim.cpp and upstream/)')
class TexturedPolygonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        source = folder / 'polygon.c'
        source.write_text(HARNESS)
        gpu = Path(os.environ.get('GPU_SOURCE', ROOT / 'pc/hal/gpu.c'))
        extra = shlex.split(os.environ.get('POLYGON_CFLAGS', ''))
        flags = shlex.split(run(['pkg-config', '--cflags', 'sdl2'])
                            .require_ok().stdout)
        objects = []
        for i, item in enumerate((str(source), 'pc/hal/gpu_queue.c',
                                  'pc/hal/gpu_texture.c', 'pc/hal/gpu_vram.c',
                                  'pc/hal/ptr_validate.c')):
            obj = folder / (str(i) + '.o')
            run(['cc', '-std=gnu17', '-D_GNU_SOURCE', '-fpermissive', '-w', '-O1',
                 '-fno-pie', '-fno-strict-aliasing', '-ffunction-sections',
                 '-fdata-sections', '-Ipc/include', '-Ipc/include/game',
                 '-Ipc/hal', '-DGPU_SOURCE="' + str(gpu.resolve()) + '"',
                 '-DOT_FIXTURE="' + str(ROOT / 'pc/tests/gpu_ot_harness.c') + '"',
                 *extra, *flags, '-c', item, '-o', str(obj)],
                cwd=ROOT, timeout=30).require_ok()
            objects.append(obj)
        # Instrument the native HAL, not upstream third-party raster code.
        shim = folder / 'shim.o'
        run(['c++', '-std=c++11', '-O2', '-w', '-ffunction-sections',
             '-fdata-sections', '-Ipc/3rd/mednafen-gpu', '-iquote',
             'pc/3rd/mednafen-gpu/shim', '-c',
             str(SHIM_SOURCE),
             '-o', str(shim)],
            cwd=ROOT, timeout=120).require_ok()
        cls.binary = folder / 'polygon'
        run(['c++', '-no-pie', *extra, *map(str, objects), str(shim),
             '-Wl,--gc-sections', '-o', str(cls.binary)],
            cwd=ROOT, timeout=30).require_ok()

    def check(self, mode):
        for backend in (0, 1):
            with self.subTest(backend=backend):
                run([str(self.binary), str(backend), mode],
                    timeout=60).require_ok()

    def test_colors_blending_depth_dither_and_retained_page(self):
        self.check('color')

    def test_mask_writes_transparent_zero_and_opaque_black(self):
        self.check('mask')

    def test_gouraud_ramps_and_per_vertex_uv(self):
        self.check('gradient')

    def test_short_records_do_not_read_vertices_or_change_page(self):
        self.check('short')

    def test_state_prefixed_packets_draw_their_primitive(self):
        self.check('prefixed')

    def test_packets_with_several_state_words_and_primitives(self):
        self.check('n')

    def test_texture_and_clut_cache_lifetime_with_warm_caches(self):
        self.check('k')

    def test_vertex_offset_11_bit_coordinates_and_oversized_polygons(self):
        self.check('v')

    def test_untextured_gouraud_colour_equal_on_both_backends(self):
        # The case renders on both backends itself and compares them.
        run([str(self.binary), '0', 'q'], timeout=60).require_ok()

    def test_untextured_fill_rule_and_shared_quad_edge(self):
        self.check('e')

    def test_untextured_polygon_stream_equal_on_both_backends(self):
        # Pseudo-random F3/F4/G3/G4 stream; compares every pixel.
        run([str(self.binary), '0', 'r'], timeout=60).require_ok()

    def test_textured_one_to_one_mapping(self):
        self.check('t')

    def test_save_menu_album_slots_subtract_once(self):
        self.check('s')

    def test_texture_and_clut_fetch_wrap_at_vram_right_edge(self):
        self.check('w')

    def test_textured_polygon_stream_equal_on_both_backends(self):
        # Pseudo-random FT3/FT4/GT3/GT4 stream; compares every pixel.
        run([str(self.binary), '0', 'u'], timeout=60).require_ok()
