/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Texture dumping and replacement packs (pc/hal/gpu_texrep.h) through the
 * real native GPU: gpu.c (included, as gpu_ot_harness.c does) with
 * gpu_texrep.c, gpu_hires.c and profile.c linked beside it.  No assets.
 * Run by pc/tools/test_gpu_texrep.py, one mode per process:
 *
 *   native     a seeded stream over two uploaded sheets (4-bit with a
 *              palette, 15-bit), moves, overdraws and frame ends; prints a
 *              hash of the native VRAM and of every StoreImage (the same
 *              with dumping, with a pack and with neither)
 *   names      prints the keys the scenarios below use, computed here from
 *              the rule in gpu_texrep.h (the test builds packs from them)
 *   dump       the dump scenarios: a sheet drawn in two frames, a sheet
 *              drawn in one, a glyph under the floor, a picture sent in
 *              16-pixel columns, a frame-buffer upload, a moved sheet;
 *              the test reads index.csv and the PNGs
 *   replace    with a pack (TOKI_TEXTURE_PACK) at 2x: sprite and polygon
 *              sub-pixels from the replacement, see-through and
 *              semi-transparent texels, a palette swap and an overdrawn
 *              texel falling back to 1x, a frame-buffer picture replaced
 *              (exit 0 = pass); prints the native hash
 *   config     prints whether texturing is on and the shadow's scale
 */
#define main ot_fixture_main
#include OT_FIXTURE
#undef main
#include "gpu_hires.h"
#include "gpu_texrep.h"

extern void gpu_hires_init(void);

void nbn_trace_load_image(const void *data, u32 bytes, s32 x, s32 y, s32 w, s32 h)
{
    (void)data; (void)bytes; (void)x; (void)y; (void)w; (void)h;
}
void nbn_trace_move_image(s32 x, s32 y, s32 w, s32 h, s32 dx, s32 dy)
{
    (void)x; (void)y; (void)w; (void)h; (void)dx; (void)dy;
}

#define FAIL(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)

static u32 rng = 777u;
static u32 rnd(u32 n) { rng = rng * 1103515245u + 12345u; return (rng >> 8) % n; }
static u32 xy(int x, int y) { return ((u32)(y & 0xffff) << 16) | (u32)(x & 0xffff); }

static void gp0(const u32 *words, int n)
{
    u32 packet[260];
    packet[0] = ((u32)n << 24) | 0xffffffu;
    memcpy(packet + 1, words, (size_t)n * 4);
    DrawOTag(packet);
}

static u32 fnv(u32 h, const void *data, size_t n)
{
    const unsigned char *p = (const unsigned char *)data;
    while (n--) { h ^= *p++; h *= 16777619u; }
    return h;
}

/* The key rule of gpu_texrep.h, written again: FNV-1a 64 over the 16-bit
 * width and height, then the rectangle's words row by row; a palette is
 * FNV-1a 64 over its 16 / 256 entries. */
static unsigned long long fnv64(unsigned long long h, const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    while (n--) { h ^= *b++; h *= 1099511628211ull; }
    return h;
}
static unsigned long long src_hash(int x, int y, int w, int h)
{
    unsigned long long v = 14695981039346656037ull;
    unsigned short d[2];
    int j;
    d[0] = (unsigned short)w; d[1] = (unsigned short)h;
    v = fnv64(v, d, 4);
    for (j = 0; j < h; j++) v = fnv64(v, &g_vram[y + j][x], (size_t)w * 2);
    return v;
}
static unsigned long long pal_hash(int x, int y, int n)
{
    return fnv64(14695981039346656037ull, &g_vram[y][x], (size_t)n * 2);
}

static void base_state(void)
{
    gpu_draw_command(0xe1000000u);
    gpu_draw_command(0xe3000000u);
    gpu_draw_command(0xe4000000u | 319u | (239u << 10));
    gpu_draw_command(0xe5000000u);
    gpu_draw_command(0xe6000000u);
    g_draw_w = 320; g_draw_h = 240;
    g_disp_x = 0; g_disp_y = 0; g_disp_w = 320; g_disp_h = 240; g_disp_mask = 1;
}

/* the drawing area over the whole VRAM (to draw into a texture) */
static void draw_everywhere(void)
{
    gpu_draw_command(0xe3000000u);
    gpu_draw_command(0xe4000000u | 1023u | (511u << 10));
    g_draw_w = 1024; g_draw_h = 512;
}

static void init(void)
{
    int x, y;
    for (y = 0; y < 512; y++)
        for (x = 0; x < 1024; x++)
            g_vram[y][x] = (u16)rnd(0x10000u);
    for (y = 0; y < 240; y++)
        for (x = 0; x < 320; x++)
            g_vram[y][x] = 0x0842;
    gpu_cache_clear();
    gpu_texrep_init();      /* before the shadow: a pack sets its scale */
    gpu_hires_init();
    base_state();
}

static void load(int x, int y, int w, int h, const u16 *data)
{
    RECT r;
    r.x = (s16)x; r.y = (s16)y; r.w = (s16)w; r.h = (s16)h;
    LoadImage(&r, (u32 *)data);
}

/* Sheet A: 4-bit, 16x16 VRAM pixels (64x16 texels) at (512, 0); texel
 * (tx, ty) = (tx + ty) & 15.  Palette P at (0, 480): entry 0 = 0000h
 * (see-through), 1 = 801Fh (red, semi-transparent bit), the rest opaque. */
static u16 sheet_a[16 * 16], pal_p[16], pal_q[16];
static void make_sheet_a(void)
{
    int tx, ty, i;
    for (ty = 0; ty < 16; ty++)
        for (tx = 0; tx < 64; tx += 4) {
            u16 w = 0;
            for (i = 0; i < 4; i++) w |= (u16)(((tx + i + ty) & 15) << (i * 4));
            sheet_a[ty * 16 + tx / 4] = w;
        }
    for (i = 0; i < 16; i++) {
        pal_p[i] = i == 0 ? 0 : i == 1 ? 0x801f : (u16)(0x0400 * (i & 7) + 0x20 * i + 3);
        pal_q[i] = i == 0 ? 0 : (u16)(0x7fff - pal_p[i]);
    }
}
#define SHEET_X 512
#define SHEET_Y 0
#define PAL_X 0
#define PAL_Y 480
#define PAL_CLUT ((PAL_Y << 6) | (PAL_X / 16))
#define PALQ_X 16
#define PALQ_CLUT ((PAL_Y << 6) | (PALQ_X / 16))
/* tpage of sheet A: page 8 (x 512), 4-bit */
#define TPAGE_A (8u)

/* the sprites' colour: 808080h (unity) unless a mode tints them */
static u32 g_tint = 0x808080u;

static void sprite_a(int x, int y, int u, int v, int w, int h, u16 clut, int semi)
{
    u32 wd[4];
    gpu_draw_command(0xe1000000u | TPAGE_A);
    wd[0] = (semi ? 0x66000000u : 0x64000000u) | g_tint;
    wd[1] = xy(x, y);
    wd[2] = ((u32)clut << 16) | (u32)(v << 8 | u);
    wd[3] = xy(w, h);
    gp0(wd, 4);
}

static void print_names(void)
{
    printf("A %s t4-%016llx-%016llx-64x16\n", "sheet",
           src_hash(SHEET_X, SHEET_Y, 16, 16), pal_hash(PAL_X, PAL_Y, 16));
}

/* ---------------------------------------------------------------- */

static int mode_native(void)
{
    static u16 buf[64 * 64], pic[32 * 32];
    u32 hs = 2166136261u, wd[9];
    int i, k;
    init();
    make_sheet_a();
    load(SHEET_X, SHEET_Y, 16, 16, sheet_a);
    load(PAL_X, PAL_Y, 16, 1, pal_p);
    load(PALQ_X, PAL_Y, 16, 1, pal_q);
    for (k = 0; k < 32 * 32; k++) pic[k] = (u16)(k * 37 + 0x8001);
    load(576, 0, 32, 32, pic);
    for (i = 0; i < 4000; i++) {
        int x = (int)rnd(280u), y = (int)rnd(200u);
        switch (rnd(6u)) {
        case 0: case 1:
            /* half of them tinted: the shadow pass's general path */
            g_tint = rnd(2u) ? 0x808080u : rnd(0x1000000u);
            sprite_a(x, y, (int)rnd(64u), (int)rnd(16u), 1 + (int)rnd(40u), 1 + (int)rnd(20u),
                     rnd(2u) ? PAL_CLUT : PALQ_CLUT, (int)rnd(2u));
            break;
        case 2:     /* FT4 on sheet A */
            wd[0] = 0x2c808080u | (rnd(2u) ? 0x02000000u : 0) | (rnd(2u) ? 0x01000000u : 0);
            wd[1] = xy(x, y); wd[2] = (u32)PAL_CLUT << 16 | 0;
            wd[3] = xy(x + 20 + (int)rnd(40u), y); wd[4] = TPAGE_A << 16 | 63u;
            wd[5] = xy(x, y + 10 + (int)rnd(30u)); wd[6] = 15u << 8;
            wd[7] = xy(x + 20 + (int)rnd(40u), y + 10 + (int)rnd(30u)); wd[8] = 15u << 8 | 63u;
            gp0(wd, 9);
            break;
        case 3:     /* 15-bit picture, page 9 */
            gpu_draw_command(0xe1000000u | 9u | 2u << 7);
            wd[0] = 0x64808080u; wd[1] = xy(x, y); wd[2] = 0;
            wd[3] = xy(1 + (int)rnd(32u), 1 + (int)rnd(32u));
            gp0(wd, 4);
            break;
        case 4:     /* overdraw the sheet, move it, read back */
            if (rnd(3u) == 0) {
                wd[0] = 0x6000ff00u; wd[1] = xy(SHEET_X + (int)rnd(16u), (int)rnd(16u));
                wd[2] = xy(2, 2);
                draw_everywhere();
                gp0(wd, 3);
                base_state();
            } else if (rnd(2u)) {
                RECT r;
                r.x = SHEET_X; r.y = SHEET_Y; r.w = 16; r.h = 16;
                MoveImage(&r, 640, 0);
            } else {
                RECT r;
                r.x = (s16)rnd(300u); r.y = (s16)rnd(200u); r.w = 64; r.h = 64;
                StoreImage(&r, (u32 *)buf);
                hs = fnv(hs, buf, sizeof buf);
            }
            break;
        default:
            if (rnd(4u) == 0) load(SHEET_X, SHEET_Y, 16, 16, sheet_a);
            gpu_texrep_frame_end(7);
            break;
        }
    }
    printf("vram=%08x store=%08x\n", (unsigned)fnv(2166136261u, g_vram, sizeof g_vram),
           (unsigned)hs);
    fflush(stdout);
    {
        GpuTexrepStats st;
        gpu_texrep_stats(&st);
        fprintf(stderr, "dumped=%lu replaced=%lu\n", st.dumped, st.samples_replaced);
    }
    return 0;
}

static int mode_names(void)
{
    static u16 pic[32 * 32];
    int k;
    init();
    make_sheet_a();
    load(SHEET_X, SHEET_Y, 16, 16, sheet_a);
    load(PAL_X, PAL_Y, 16, 1, pal_p);
    print_names();
    for (k = 0; k < 32 * 32; k++) pic[k] = (u16)(0x7c00 | k);
    load(640, 256, 32, 32, pic);
    printf("W picture t16-%016llx-32x32\n", src_hash(640, 256, 32, 32));
    return 0;
}

/* ---------------------------------------------------------------- */

static int mode_dump(void)
{
    static u16 sheet_b[16 * 16], glyph[4 * 12], strip[16 * 64], frame[64 * 64];
    int i, k;
    init();
    make_sheet_a();
    load(SHEET_X, SHEET_Y, 16, 16, sheet_a);
    load(PAL_X, PAL_Y, 16, 1, pal_p);
    print_names();
    /* A: two frames */
    sprite_a(10, 10, 0, 0, 64, 16, PAL_CLUT, 0);
    gpu_texrep_frame_end(7);
    sprite_a(10, 10, 0, 0, 64, 16, PAL_CLUT, 0);
    gpu_texrep_frame_end(7);
    /* B at (528, 0): drawn in one frame only */
    for (k = 0; k < 16 * 16; k++) sheet_b[k] = (u16)(k * 4111u);
    load(SHEET_X + 16, SHEET_Y, 16, 16, sheet_b);
    sprite_a(10, 40, 64, 0, 64, 16, PAL_CLUT, 0);
    gpu_texrep_frame_end(7);
    printf("B one-frame t4-%016llx-%016llx-64x16\n", src_hash(SHEET_X + 16, 0, 16, 16),
           pal_hash(PAL_X, PAL_Y, 16));
    /* a 16x12 glyph at (512, 32): two frames, under the floor */
    for (k = 0; k < 4 * 12; k++) glyph[k] = (u16)(0x1111u * (k & 3));
    load(SHEET_X, 32, 4, 12, glyph);
    for (i = 0; i < 2; i++) {
        sprite_a(100, 100, 0, 32, 16, 12, PAL_CLUT, 0);
        gpu_texrep_frame_end(7);
    }
    printf("G small t4-%016llx-%016llx-16x12\n", src_hash(SHEET_X, 32, 4, 12),
           pal_hash(PAL_X, PAL_Y, 16));
    /* a 320x64 picture in twenty 16x64 columns at (320, 256), drawn as a
     * 15-bit sprite from page 5/1 in two frames */
    for (i = 0; i < 20; i++) {
        for (k = 0; k < 16 * 64; k++) strip[k] = (u16)(0x8000 | (i * 1601 + k * 7));
        load(320 + i * 16, 256, 16, 64, strip);
    }
    for (i = 0; i < 2; i++) {
        u32 wd[4];
        gpu_draw_command(0xe1000000u | 5u | 1u << 4 | 2u << 7);
        wd[0] = 0x64808080u; wd[1] = xy(0, 0); wd[2] = 0; wd[3] = xy(256, 64);
        gp0(wd, 4);
        gpu_texrep_frame_end(7);
    }
    printf("S strips t16-%016llx-320x64\n", src_hash(320, 256, 320, 64));
    /* a frame-buffer upload (into the display) */
    for (k = 0; k < 64 * 64; k++) frame[k] = (u16)(k | 0x0421);
    load(100, 100, 64, 64, frame);
    gpu_texrep_frame_end(7);
    printf("F write t16-%016llx-64x64\n", src_hash(100, 100, 64, 64));
    /* sheet A moved whole to (768, 0) and drawn from there (page 12): the
     * same key, so nothing new is written */
    {
        RECT r;
        r.x = SHEET_X; r.y = SHEET_Y; r.w = 16; r.h = 16;
        MoveImage(&r, 768, 0);
        for (i = 0; i < 2; i++) {
            u32 wd[4];
            gpu_draw_command(0xe1000000u | 12u);
            wd[0] = 0x64808080u; wd[1] = xy(10, 150); wd[2] = (u32)PAL_CLUT << 16;
            wd[3] = xy(64, 16);
            gp0(wd, 4);
            gpu_texrep_frame_end(7);
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- */

/* The test pack's pixel (X, Y) of sheet A's 2x replacement (the test
 * writes the same formula into the PNG): alpha 0 in texel column 3 (see-
 * through), 128 in texel row 5 (a soft edge, mixed), else opaque. */
static void rep_pixel(int X, int Y, unsigned *r, unsigned *g, unsigned *b, unsigned *a)
{
    *r = (unsigned)(X * 7) & 255u;
    *g = (unsigned)(Y * 13) & 255u;
    *b = (unsigned)((X + Y) * 5) & 255u;
    *a = (X / 2) == 3 ? 0u : (Y / 2) == 5 ? 128u : 255u;
}
/* What the shadow must hold for replacement pixel (X, Y) of texel (tx, ty)
 * drawn over `bg`: see-through below the clear alpha; else the colour with
 * the NATIVE texel's semi-transparency bit, blended when the primitive is
 * semi-transparent, then mixed with bg by its alpha. */
static u16 want_pixel(int X, int Y, int tx, int ty, int semi, u16 bg)
{
    unsigned r, g, b, a;
    u16 stp, p;
    rep_pixel(X, Y, &r, &g, &b, &a);
    if (a < GPU_TEXREP_ALPHA_CLEAR) return bg;
    stp = pal_p[(tx + ty) & 15] & 0x8000;
    p = (u16)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | stp);
    if (semi && stp) p = blend_pixel(bg, p, 0) | stp;
    if (a < GPU_TEXREP_ALPHA_SOLID) p = gpu_texrep_alpha_mix(bg, p, a);
    return p;
}

static u16 shadow_at(int sx, int sy)
{
    return g_hires.vram[(size_t)sy * g_hires.stride + sx];
}

/* sprite of the whole sheet at (ox, oy), 1:1, over the 0842h background */
static void check_replaced(int ox, int oy, const char *what, int semi)
{
    int tx, ty, i, j;
    for (ty = 0; ty < 16; ty++)
        for (tx = 0; tx < 64; tx++)
            for (j = 0; j < 2; j++)
                for (i = 0; i < 2; i++) {
                    int X = tx * 2 + i, Y = ty * 2 + j;
                    u16 got = shadow_at((ox + tx) * 2 + i, (oy + ty) * 2 + j);
                    u16 want = want_pixel(X, Y, tx, ty, semi, 0x0842);
                    if (got != want)
                        FAIL("%s: texel %d,%d sub %d,%d: shadow %04x, want %04x", what,
                             tx, ty, i, j, got, want);
                }
}

static int block_is_native(int x, int y)
{
    int i, j;
    for (j = 0; j < 2; j++)
        for (i = 0; i < 2; i++)
            if (shadow_at(x * 2 + i, y * 2 + j) != g_vram[y][x]) return 0;
    return 1;
}

static int mode_replace(void)
{
    static u16 pic[32 * 32];
    u32 wd[9];
    int x, y, k;
    GpuTexrepStats st;
    init();
    if (g_hires.scale != 2) FAIL("the pack should raise the shadow to 2x (it is %dx)", g_hires.scale);
    make_sheet_a();
    load(SHEET_X, SHEET_Y, 16, 16, sheet_a);
    load(PAL_X, PAL_Y, 16, 1, pal_p);
    load(PALQ_X, PAL_Y, 16, 1, pal_q);
    /* 1. sprite, opaque primitive */
    sprite_a(10, 10, 0, 0, 64, 16, PAL_CLUT, 0);
    check_replaced(10, 10, "sprite", 0);
    /* 2. sprite, semi-transparent primitive (abr 0): row 5 blends */
    sprite_a(10, 40, 0, 0, 64, 16, PAL_CLUT, 1);
    check_replaced(10, 40, "semi sprite", 1);
    /* 3. FT4 mapped 1:1, raw */
    wd[0] = 0x2d000000u;
    wd[1] = xy(10, 70); wd[2] = (u32)PAL_CLUT << 16 | 0;
    wd[3] = xy(74, 70); wd[4] = TPAGE_A << 16 | 64u;
    wd[5] = xy(10, 86); wd[6] = 16u << 8;
    wd[7] = xy(74, 86); wd[8] = 16u << 8 | 64u;
    gp0(wd, 9);
    check_replaced(10, 70, "polygon", 0);
    /* 4. the same sheet under another palette: no replacement, 1x */
    sprite_a(10, 100, 0, 0, 64, 16, PALQ_CLUT, 0);
    for (y = 100; y < 116; y++)
        for (x = 10; x < 74; x++)
            if (!block_is_native(x, y)) FAIL("palette swap: block %d,%d is not 1x", x, y);
    /* 5. overdraw texels (2..5, 0..1) of the sheet: those fall back to 1x */
    draw_everywhere();
    wd[0] = 0x60001234u; wd[1] = xy(SHEET_X, 0); wd[2] = xy(1, 2);
    gp0(wd, 3);
    base_state();
    gpu_cache_clear();
    sprite_a(10, 130, 0, 0, 64, 16, PAL_CLUT, 0);
    for (y = 130; y < 132; y++)
        for (x = 10; x < 14; x++)
            if (!block_is_native(x, y)) FAIL("overdrawn texel %d,%d still replaced", x - 10, y - 130);
    if (shadow_at(20 * 2, 140 * 2) != want_pixel(20, 20, 10, 10, 0, 0x0842))
        FAIL("untouched texels lost the replacement");
    /* 6. a frame-buffer picture with a t16 replacement: solid 2x colour */
    for (k = 0; k < 32 * 32; k++) pic[k] = (u16)(0x7c00 | k);
    load(200, 150, 32, 32, pic);
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++)
            if (shadow_at(400 + x, 300 + y) != ((x & 1) ? 0x03e0 : 0x001f))
                FAIL("frame-buffer picture: shadow %d,%d is %04x", x, y,
                     shadow_at(400 + x, 300 + y));
    gpu_texrep_stats(&st);
    printf("vram=%08x replaced=%lu\n", (unsigned)fnv(2166136261u, g_vram, sizeof g_vram),
           st.samples_replaced);
    return 0;
}

/* The same draws without checks: the native VRAM to compare with. */
static int mode_replace_native(void)
{
    static u16 pic[32 * 32];
    u32 wd[9];
    int k;
    init();
    make_sheet_a();
    load(SHEET_X, SHEET_Y, 16, 16, sheet_a);
    load(PAL_X, PAL_Y, 16, 1, pal_p);
    load(PALQ_X, PAL_Y, 16, 1, pal_q);
    sprite_a(10, 10, 0, 0, 64, 16, PAL_CLUT, 0);
    sprite_a(10, 40, 0, 0, 64, 16, PAL_CLUT, 1);
    wd[0] = 0x2d000000u;
    wd[1] = xy(10, 70); wd[2] = (u32)PAL_CLUT << 16 | 0;
    wd[3] = xy(74, 70); wd[4] = TPAGE_A << 16 | 64u;
    wd[5] = xy(10, 86); wd[6] = 16u << 8;
    wd[7] = xy(74, 86); wd[8] = 16u << 8 | 64u;
    gp0(wd, 9);
    sprite_a(10, 100, 0, 0, 64, 16, PALQ_CLUT, 0);
    draw_everywhere();
    wd[0] = 0x60001234u; wd[1] = xy(SHEET_X, 0); wd[2] = xy(1, 2);
    gp0(wd, 3);
    base_state();
    gpu_cache_clear();
    sprite_a(10, 130, 0, 0, 64, 16, PAL_CLUT, 0);
    for (k = 0; k < 32 * 32; k++) pic[k] = (u16)(0x7c00 | k);
    load(200, 150, 32, 32, pic);
    printf("vram=%08x\n", (unsigned)fnv(2166136261u, g_vram, sizeof g_vram));
    return 0;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";
    if (!strcmp(mode, "native")) return mode_native();
    if (!strcmp(mode, "names")) return mode_names();
    if (!strcmp(mode, "dump")) return mode_dump();
    if (!strcmp(mode, "replace")) return mode_replace();
    if (!strcmp(mode, "replace-native")) return mode_replace_native();
    if (!strcmp(mode, "config")) {
        gpu_texrep_init();
        gpu_hires_init();
        printf("textures=%d replace=%d scale=%d\n", g_texrep_on, g_texrep_replace, g_hires.scale);
        return 0;
    }
    FAIL("unknown mode %s", mode);
    return 2;
}
