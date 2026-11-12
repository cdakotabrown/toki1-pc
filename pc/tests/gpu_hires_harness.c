/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The internal resolution scale (pc/hal/gpu_hires.h) through the real
 * native GPU: gpu.c (included, as gpu_ot_harness.c does) and gpu_hires.c /
 * profile.c linked beside it.  GP0 records are literal PS1 words handed to
 * DrawOTag; LoadImage / StoreImage / MoveImage are the SDK entry points.
 * No assets.  Run by pc/tools/test_gpu_hires.py, one mode per process:
 *
 *   stream     a seeded mix of every primitive family, fills, uploads,
 *              moves and reads; prints a hash of the native VRAM and of
 *              everything StoreImage returned (equal at every scale)
 *   texcache   a stale texture-cache line that the shadow pass must not
 *              evict; prints the native pixels it draws (equal at every scale)
 *   config     prints the scale gpu_hires_init chose
 *   coverage   random flat and alpha-tested textured triangles; prints how
 *              many native pixels a polygon drew outside every block its
 *              shadow pass claimed (0: none fall back to 1x)
 *   sprite / poly1to1 / edges / fill / move / resync: assertions at the
 *              scale in TOKI_RES_SCALE (exit 0 = pass)
 */
#define main ot_fixture_main
#include OT_FIXTURE
#undef main
#include "gpu_hires.h"

extern void gpu_hires_init(void);
extern unsigned long gpu_hires_resync_rect(int x, int y, int w, int h);

/* LoadImage's trace hook (pc/hal/nbn_trace.c): not under test here. */
void nbn_trace_load_image(const void *data, u32 bytes, s32 x, s32 y, s32 w, s32 h)
{
    (void)data; (void)bytes; (void)x; (void)y; (void)w; (void)h;
}
void nbn_trace_move_image(s32 x, s32 y, s32 w, s32 h, s32 dx, s32 dy)
{
    (void)x; (void)y; (void)w; (void)h; (void)dx; (void)dy;
}

#define FAIL(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)

static u32 rng = 12345u;
static u32 rnd(u32 n) { rng = rng * 1103515245u + 12345u; return (rng >> 8) % n; }

static u32 xy(int x, int y) { return ((u32)(y & 0xffff) << 16) | (u32)(x & 0xffff); }

/* One DMA packet: tag (len, terminator) then the GP0 words. */
static void gp0(const u32 *words, int n)
{
    u32 packet[260];
    packet[0] = ((u32)n << 24) | 0xffffffu;
    memcpy(packet + 1, words, (size_t)n * 4);
    DrawOTag(packet);
}

static void base_state(void)
{
    gpu_draw_command(0xe1000000u);
    gpu_draw_command(0xe3000000u);
    gpu_draw_command(0xe4000000u | 1023u | (511u << 10));
    gpu_draw_command(0xe5000000u);
    gpu_draw_command(0xe6000000u);
}

static u16 shadow_at(int sx, int sy)
{
    return g_hires.vram[(size_t)sy * g_hires.stride + sx];
}

/* 1: every sub-pixel of native (x, y)'s block is the native pixel. */
static int block_is_native(int x, int y)
{
    int s = g_hires.scale, i, j;
    for (j = 0; j < s; j++)
        for (i = 0; i < s; i++)
            if (shadow_at(x * s + i, y * s + j) != g_vram[y][x]) return 0;
    return 1;
}

static int block_uniform(int x, int y)
{
    int s = g_hires.scale, i, j;
    u16 first = shadow_at(x * s, y * s);
    for (j = 0; j < s; j++)
        for (i = 0; i < s; i++)
            if (shadow_at(x * s + i, y * s + j) != first) return 0;
    return 1;
}

static u32 fnv(u32 h, const void *data, size_t n)
{
    const unsigned char *p = (const unsigned char *)data;
    while (n--) { h ^= *p++; h *= 16777619u; }
    return h;
}

static void init(void)
{
    int x, y;
    for (y = 0; y < 512; y++)
        for (x = 0; x < 1024; x++)
            g_vram[y][x] = (u16)(rnd(0x10000u));
    for (y = 0; y < 256; y++)            /* draw area: dark grey */
        for (x = 0; x < 512; x++)
            g_vram[y][x] = 0x0842;
    gpu_cache_clear();
    gpu_hires_init();       /* allocates and mirrors the seeded VRAM */
    base_state();
}

static void init_hires(void)
{
    init();
    if (g_hires.scale <= 1) FAIL("this mode needs GPU_HIRES=1 and TOKI_RES_SCALE > 1");
}

/* A slanted red triangle over the draw area: mixed blocks on its edges. */
static void red_triangle(int ox, int oy)
{
    u32 w[4];
    w[0] = 0x200000ffu;
    w[1] = xy(ox + 3, oy + 2);
    w[2] = xy(ox + 41, oy + 9);
    w[3] = xy(ox + 12, oy + 37);
    gp0(w, 4);
}

static int mixed_blocks(int x0, int y0, int w, int h)
{
    int x, y, n = 0;
    for (y = y0; y < y0 + h; y++)
        for (x = x0; x < x0 + w; x++)
            n += !block_uniform(x, y);
    return n;
}

/* ---------------------------------------------------------------- */

static void random_primitive(void)
{
    u32 w[16];
    u32 page = 8u + rnd(4u) + ((u32)rnd(4u) << 5) + ((u32)rnd(3u) << 7);
    u32 clut = (480u + rnd(16u)) << 6 | rnd(64u);
    u32 code = rnd(4u) * 0x01000000u;           /* raw / semi bits */
    int x = (int)rnd(300u), y = (int)rnd(220u), i;
    switch (rnd(10u)) {
    case 0:   /* F3 / F4 */
        w[0] = 0x20000000u | code | rnd(0x1000000u);
        for (i = 1; i <= 4; i++) w[i] = xy(x + (int)rnd(60u) - 10, y + (int)rnd(50u) - 10);
        if (rnd(2u)) { w[0] |= 0x08000000u; gp0(w, 5); } else gp0(w, 4);
        break;
    case 1:   /* G3 */
        for (i = 0; i < 3; i++) {
            w[i * 2] = (i ? 0 : (0x30000000u | code)) | rnd(0x1000000u);
            w[i * 2 + 1] = xy(x + (int)rnd(60u) - 10, y + (int)rnd(50u) - 10);
        }
        gp0(w, 6);
        break;
    case 2:   /* FT3 */
    case 3:   /* FT4 */
        w[0] = 0x24000000u | code | rnd(0x1000000u);
        w[1] = xy(x, y);
        w[2] = clut << 16 | rnd(0x10000u);
        w[3] = xy(x + (int)rnd(80u), y + (int)rnd(10u));
        w[4] = page << 16 | rnd(0x10000u);
        w[5] = xy(x + (int)rnd(10u), y + (int)rnd(70u));
        w[6] = rnd(0x10000u);
        if (rnd(2u)) {
            w[0] |= 0x08000000u;
            w[7] = xy(x + (int)rnd(80u), y + (int)rnd(70u));
            w[8] = rnd(0x10000u);
            gp0(w, 9);
        } else gp0(w, 7);
        break;
    case 4:   /* GT3 */
        w[0] = 0x34000000u | code | rnd(0x1000000u);
        w[1] = xy(x, y);
        w[2] = clut << 16 | rnd(0x10000u);
        w[3] = rnd(0x1000000u);
        w[4] = xy(x + (int)rnd(80u), y + (int)rnd(10u));
        w[5] = page << 16 | rnd(0x10000u);
        w[6] = rnd(0x1000000u);
        w[7] = xy(x + (int)rnd(10u), y + (int)rnd(70u));
        w[8] = rnd(0x10000u);
        gp0(w, 9);
        break;
    case 5:   /* SPRT, after an E1 */
        gpu_draw_command(0xe1000000u | page | (rnd(2u) << 9));
        w[0] = 0x64000000u | code | rnd(0x1000000u);
        w[1] = xy(x, y);
        w[2] = clut << 16 | rnd(0x10000u);
        w[3] = xy(1 + (int)rnd(40u), 1 + (int)rnd(40u));
        gp0(w, 4);
        break;
    case 6:   /* TILE */
        w[0] = 0x60000000u | (code & 0x02000000u) | rnd(0x1000000u);
        w[1] = xy(x, y);
        w[2] = xy(1 + (int)rnd(30u), 1 + (int)rnd(30u));
        gp0(w, 3);
        break;
    case 7:   /* LINE_G2 */
        w[0] = 0x50000000u | (code & 0x02000000u) | rnd(0x1000000u);
        w[1] = xy(x, y);
        w[2] = rnd(0x1000000u);
        w[3] = xy(x + (int)rnd(60u), y + (int)rnd(60u));
        gp0(w, 4);
        break;
    case 8:   /* fill / mask state / dither */
        if (rnd(2u)) {
            w[0] = 0x02000000u | rnd(0x1000000u);
            w[1] = xy((int)rnd(400u), (int)rnd(240u));
            w[2] = xy(1 + (int)rnd(60u), 1 + (int)rnd(40u));
            gp0(w, 3);
        } else {
            gpu_draw_command(0xe6000000u | rnd(4u));
            gpu_draw_command(0xe1000000u | (rnd(2u) << 9));
        }
        break;
    default:  /* an A0 upload (texture or into the draw area) */
        {
            int uw = 1 + (int)rnd(12u), uh = 1 + (int)rnd(12u), n = (uw * uh + 1) / 2;
            w[0] = 0xa0000000u;
            w[1] = xy(rnd(2u) ? (int)rnd(500u) : 512 + (int)rnd(256u), (int)rnd(500u));
            w[2] = xy(uw, uh);
            {
                u32 packet[260];
                int k;
                packet[0] = ((u32)(3 + n) << 24) | 0xffffffu;
                memcpy(packet + 1, w, 12);
                for (k = 0; k < n; k++) packet[4 + k] = rnd(0x10000u) | rnd(0x10000u) << 16;
                DrawOTag(packet);
            }
        }
        break;
    }
}

static int mode_stream(void)
{
    static u16 buf[64 * 64];
    u32 hs = 2166136261u;
    int i;
    init();
    for (i = 0; i < 6000; i++) {
        random_primitive();
        if (i % 97 == 0) {      /* SDK transfers */
            RECT r;
            r.x = (s16)rnd(400u); r.y = (s16)rnd(400u);
            r.w = (s16)(1 + rnd(64u)); r.h = (s16)(1 + rnd(64u));
            if (rnd(2u)) {
                MoveImage(&r, (s32)rnd(900u), (s32)rnd(440u));
            } else if (rnd(2u)) {
                StoreImage(&r, (u32 *)buf);
                hs = fnv(hs, buf, (size_t)r.w * r.h * 2);
            } else {
                int k;
                for (k = 0; k < r.w * r.h; k++) buf[k] = (u16)rnd(0x10000u);
                LoadImage(&r, (u32 *)buf);
            }
        }
    }
    printf("vram=%08x store=%08x\n", (unsigned)fnv(2166136261u, g_vram, sizeof g_vram),
           (unsigned)hs);
    return 0;
}

/* Page 8 (x 512), 15-bit.  A line of 4 texels is cached at slot
 * (u >> 2) | ((v & 31) << 3), so rows v and v + 32 share a slot. */
static int mode_texcache(void)
{
    u32 w[9];
    int x;
    init();
    for (x = 0; x < 4; x++) {
        g_vram[0][512 + x] = 0x001f;      /* v 0: red */
        g_vram[32][512 + x] = 0x7c00;     /* v 32: blue */
    }
    gpu_cache_clear();
    /* A: 1:1, v 0 only -> caches the v 0 line */
    w[0] = 0x2c808080u;
    w[1] = xy(100, 100); w[2] = 0;
    w[3] = xy(104, 100); w[4] = (8u | 2u << 7) << 16 | 4u;
    w[5] = xy(100, 101); w[6] = 0;
    w[7] = xy(104, 101); w[8] = 4u;
    gp0(w, 9);
    /* the texture's v 0 line becomes green: a fill does not flush the cache */
    w[0] = 0x0200ff00u; w[1] = xy(512, 0); w[2] = xy(16, 1);
    gp0(w, 3);
    /* B: one native row, v 0 -> 64 per row: at 2x its second sub-row
     * samples v 32, whose line shares the v 0 line's slot */
    w[0] = 0x2c808080u;
    w[1] = xy(100, 110); w[2] = 0;
    w[3] = xy(104, 110); w[4] = (8u | 2u << 7) << 16 | 4u;
    w[5] = xy(100, 111); w[6] = 64u << 8;
    w[7] = xy(104, 111); w[8] = 64u << 8 | 4u;
    gp0(w, 9);
    printf("%04x %04x %04x %04x\n", g_vram[110][100], g_vram[110][101],
           g_vram[110][102], g_vram[110][103]);
    return 0;
}

/* Sprites, tiles and lines look exactly as at 1x; under a sprite's
 * transparent texels the polygon's sub-pixel edge stays. */
static int mode_sprite(void)
{
    static u16 before[4 * 64 * 4 * 64];
    u32 w[4];
    int x, y, s;
    init_hires();
    s = g_hires.scale;
    red_triangle(40, 40);
    if (!mixed_blocks(40, 40, 50, 50)) FAIL("no sub-pixel edge to keep");
    for (y = 0; y < 64 * s; y++)
        for (x = 0; x < 64 * s; x++)
            before[y * 64 * s + x] = shadow_at(40 * s + x, 40 * s + y);
    /* texture on page 8, 15-bit: checker of transparent / opaque texels */
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++)
            g_vram[y][512 + x] = ((x ^ y) & 4) ? 0 : (u16)(0x03e0 | (x & 31));
    gpu_cache_clear();
    gpu_draw_command(0xe1000000u | 8u | 2u << 7);
    w[0] = 0x64808080u; w[1] = xy(40, 40); w[2] = 0; w[3] = xy(64, 64);
    gp0(w, 4);
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++) {
            int sx, sy, opaque = !(((x ^ y) & 4));
            if (opaque) {
                if (!block_is_native(40 + x, 40 + y))
                    FAIL("sprite pixel %d,%d is not the 1x pixel", x, y);
                continue;
            }
            for (sy = 0; sy < s; sy++)
                for (sx = 0; sx < s; sx++)
                    if (shadow_at((40 + x) * s + sx, (40 + y) * s + sy) !=
                        before[(y * s + sy) * 64 * s + x * s + sx])
                        FAIL("transparent texel %d,%d changed the shadow", x, y);
        }
    /* a semi-transparent tile and a line over uniform blocks: 1x pixels */
    w[0] = 0x620080ffu; w[1] = xy(200, 40); w[2] = xy(20, 20);
    gp0(w, 3);
    w[0] = 0x5000ff00u; w[1] = xy(200, 100); w[2] = 0x00ff0000u; w[3] = xy(260, 130);
    gp0(w, 4);
    for (y = 40; y < 140; y++)
        for (x = 200; x < 270; x++)
            if (!block_is_native(x, y)) FAIL("tile/line pixel %d,%d is not 1x", x, y);
    return 0;
}

/* A textured quad mapped 1:1 samples one texel per native pixel block. */
static int mode_poly1to1(void)
{
    u32 w[9];
    int x, y;
    init_hires();
    for (y = 0; y < 40; y++)
        for (x = 0; x < 40; x++)
            g_vram[y][512 + x] = (u16)(0x8000 | (x * 3 + y * 97));
    gpu_cache_clear();
    w[0] = 0x2d000000u;   /* FT4, raw */
    w[1] = xy(60, 60); w[2] = 0;
    w[3] = xy(100, 60); w[4] = (8u | 2u << 7) << 16 | 40u;
    w[5] = xy(60, 100); w[6] = 40u << 8;
    w[7] = xy(100, 100); w[8] = 40u << 8 | 40u;
    gp0(w, 9);
    for (y = 60; y < 100; y++)
        for (x = 60; x < 100; x++)
            if (!block_is_native(x, y))
                FAIL("1:1 texel at %d,%d: shadow is not the 1x pixel", x, y);
    return 0;
}

/* Polygon edges gain resolution; interiors are the 1x colour. */
static int mode_edges(void)
{
    int n;
    init_hires();
    red_triangle(100, 100);
    n = mixed_blocks(100, 100, 50, 50);
    if (n < 20) FAIL("only %d edge blocks have sub-pixel coverage", n);
    if (!block_is_native(115, 115)) FAIL("the interior is not the 1x colour");
    printf("edge blocks %d\n", n);
    return 0;
}

/* Writes that replace pixels replace their sub-pixel edges too. */
static int mode_fill(void)
{
    static u16 black[64 * 64];
    u32 w[3], words[3];
    RECT r;
    init_hires();
    red_triangle(40, 40);
    w[0] = 0x02000000u; w[1] = xy(32, 32); w[2] = xy(64, 64);
    gp0(w, 3);
    if (mixed_blocks(32, 32, 64, 64)) FAIL("GP0(02h) fill left sub-pixel edges");
    red_triangle(140, 40);
    words[0] = 0x02000000u; words[1] = xy(128, 32); words[2] = xy(64, 64);
    gpu_execute_clear_words(words, 3);
    if (mixed_blocks(128, 32, 64, 64)) FAIL("the SDK clear left sub-pixel edges");
    red_triangle(240, 40);
    r.x = 232; r.y = 32; r.w = 64; r.h = 64;
    memset(black, 0, sizeof black);
    LoadImage(&r, (u32 *)black);
    if (mixed_blocks(232, 32, 64, 64)) FAIL("LoadImage left sub-pixel edges");
    red_triangle(340, 40);
    gpu_clear_tile(0x60000000u, xy(332, 32), xy(64, 64));
    if (mixed_blocks(332, 32, 64, 64)) FAIL("the clear tile left sub-pixel edges");
    return 0;
}

/* MoveImage carries valid high-resolution blocks; an overlapping move
 * takes the 1x result. */
static int mode_move(void)
{
    RECT r;
    int x, y, s;
    init_hires();
    s = g_hires.scale;
    red_triangle(40, 40);
    r.x = 40; r.y = 40; r.w = 50; r.h = 50;
    MoveImage(&r, 200, 100);
    for (y = 0; y < 50 * s; y++)
        for (x = 0; x < 50 * s; x++)
            if (shadow_at(200 * s + x, 100 * s + y) != shadow_at(40 * s + x, 40 * s + y))
                FAIL("moved shadow differs at %d,%d", x, y);
    MoveImage(&r, 60, 50);   /* overlaps its source */
    for (y = 50; y < 100; y++)
        for (x = 60; x < 110; x++)
            if (!block_is_native(x, y)) FAIL("overlapping move: %d,%d not 1x", x, y);
    return 0;
}

/* A native write the shadow did not see shows at 1x when presented. */
static int mode_resync(void)
{
    const unsigned int *px;
    int w, h, x, y, s;
    init_hires();
    s = g_hires.scale;
    red_triangle(40, 40);
    for (y = 40; y < 90; y++)
        for (x = 40; x < 90; x++)
            g_vram[y][x] = 0x7c00;      /* behind the HAL's back */
    g_disp_x = 0; g_disp_y = 0; g_disp_w = 320; g_disp_h = 240; g_disp_mask = 1;
    px = gpu_hires_pack_display(&w, &h);
    if (!px || w != 320 * s || h != 240 * s) FAIL("no high-resolution display");
    for (y = 40 * s; y < 90 * s; y++)
        for (x = 40 * s; x < 90 * s; x++)
            if (px[y * w + x] != 0x0000F8FFu) FAIL("stale shadow shown at %d,%d", x, y);
    return 0;
}

/* Every native pixel a polygon draws lies in a block its shadow pass drew
 * into (the stamp): random flat triangles over a uniform area, each native
 * pixel that changes checked against the stamp.  Prints the misses. */
static int mode_coverage(void)
{
    static u16 before[256][320];
    u32 w[9];
    long misses = 0, drawn = 0;
    int i, x, y, k;
    init_hires();
    /* page 8, 15-bit: 4x4 checker of transparent (0) and opaque texels */
    for (y = 0; y < 256; y++)
        for (x = 0; x < 64; x++)
            g_vram[y][512 + x] = ((x ^ y) & 4) ? 0 : (u16)(0x4210 + x + y);
    gpu_hires_mirror_rect(512, 0, 64, 256);
    gpu_cache_clear();
    for (i = 0; i < 3000; i++) {
        for (y = 0; y < 256; y++)
            for (x = 0; x < 320; x++) {
                g_vram[y][x] = 0;
                before[y][x] = 0;
            }
        gpu_hires_mirror_rect(0, 0, 320, 256);
        if (i & 1) {    /* textured: minified, magnified, transparent texels */
            w[0] = 0x25000000u;
            for (k = 0; k < 3; k++) {
                w[1 + k * 2] = xy((int)rnd(300u) - 10, (int)rnd(240u) - 10);
                w[2 + k * 2] = rnd(64u) | rnd(256u) << 8;
            }
            w[4] |= (8u | 2u << 7) << 16;
            gp0(w, 7);
        } else {
            w[0] = 0x20000000u | (1u + rnd(0xfffffeu));
            for (k = 1; k <= 3; k++)
                w[k] = xy((int)rnd(300u) - 10, (int)rnd(240u) - 10);
            gp0(w, 4);
        }
        for (y = 0; y < 240; y++)
            for (x = 0; x < 320; x++)
                if (g_vram[y][x] != before[y][x]) {
                    drawn++;
                    if (g_hires.stamp[y * 1024 + x] != g_hires.prim) misses++;
                }
    }
    printf("drawn %ld misses %ld\n", drawn, misses);
    return 0;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";
    if (!strcmp(mode, "coverage")) return mode_coverage();
    if (!strcmp(mode, "config")) {
        gpu_hires_init();
        printf("scale=%d\n", g_hires.scale);
        return 0;
    }
    if (!strcmp(mode, "stream")) return mode_stream();
    if (!strcmp(mode, "texcache")) return mode_texcache();
    if (!strcmp(mode, "sprite")) return mode_sprite();
    if (!strcmp(mode, "poly1to1")) return mode_poly1to1();
    if (!strcmp(mode, "edges")) return mode_edges();
    if (!strcmp(mode, "fill")) return mode_fill();
    if (!strcmp(mode, "move")) return mode_move();
    if (!strcmp(mode, "resync")) return mode_resync();
    FAIL("unknown mode %s", mode);
    return 2;
}
