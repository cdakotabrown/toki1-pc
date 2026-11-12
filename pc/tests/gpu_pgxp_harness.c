/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Sub-pixel vertex precision for the high-resolution shadow
 * (pc/hal/gpu_pgxp.h) through the real GTE model (pc/hal/gte.c) and the
 * real native GPU (gpu.c, included as gpu_ot_harness.c does; gpu_hires.c,
 * gpu_pgxp.c, profile.c linked beside it).  Polygons are built the way the
 * port's TMD builders build them: LDV, RTPT / RTPS, the SXY FIFO stored
 * into a GP0 packet, and the packet handed to DrawOTag.  No assets.  Run
 * by pc/tools/test_gpu_pgxp.py, one mode per process:
 *
 *   config   prints whether sub-pixel precision is active
 *   stream   a slowly moving 3D object, 120 frames; prints the hash of the
 *            native VRAM, then of the shadow, the precise-polygon count and
 *            the number of stale shadow blocks left (shown at 1x)
 *   jitter   the same object moving 0.15 native pixels a frame; prints the
 *            RMS of its left vertex (as the shadow shows it) around its
 *            straight-line path, in native pixels, and how many polygons
 *            used fractions
 *   rules    the table's rules (generations, ambiguity, all-or-nothing) and
 *            the GTE's fractions against an exact projection (exit 0 = pass)
 */
#include <math.h>
#define main ot_fixture_main
#include OT_FIXTURE
#undef main
#include "gpu_hires.h"
#include "gpu_pgxp.h"
#include "hal_gte_cop2.h"

extern void gpu_hires_init(void);
extern void gte_test_tables_init(void);
extern unsigned long gpu_hires_resync_rect(int x, int y, int w, int h);

void nbn_trace_load_image(const void *data, u32 bytes, s32 x, s32 y, s32 w, s32 h)
{
    (void)data; (void)bytes; (void)x; (void)y; (void)w; (void)h;
}
void nbn_trace_move_image(s32 x, s32 y, s32 w, s32 h, s32 dx, s32 dy)
{
    (void)x; (void)y; (void)w; (void)h; (void)dx; (void)dy;
}

#define FAIL(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)

static u32 xy(int x, int y) { return ((u32)(y & 0xffff) << 16) | (u32)(x & 0xffff); }

static u32 fnv(u32 h, const void *data, size_t n)
{
    const unsigned char *p = (const unsigned char *)data;
    while (n--) { h ^= *p++; h *= 16777619u; }
    return h;
}

/* One frame's OT: a single DMA packet of GP0 words (one DrawOTag, so one
 * generation of the fraction table). */
static u32 g_pkt[256];
static int g_n;

static void pkt_begin(void) { g_n = 1; }
static void pkt_word(u32 w) { if (g_n < 256) g_pkt[g_n++] = w; }
static void pkt_draw(void)
{
    g_pkt[0] = ((u32)(g_n - 1) << 24) | 0xffffffu;
    DrawOTag(g_pkt);
}

static void base_state(void)
{
    gpu_draw_command(0xe1000000u);
    gpu_draw_command(0xe3000000u);
    gpu_draw_command(0xe4000000u | 1023u | (511u << 10));
    gpu_draw_command(0xe5000000u);
    gpu_draw_command(0xe6000000u);
}

static void init(void)
{
    MATRIX m;
    int x, y;
    for (y = 0; y < 512; y++)
        for (x = 0; x < 1024; x++)
            g_vram[y][x] = 0x0842;
    gpu_cache_clear();
    gpu_hires_init();
    base_state();
    gte_test_tables_init();
    InitGeom();
    SetGeomOffset(160, 120);
    SetGeomScreen(300);
    memset(&m, 0, sizeof m);
    m.m[0][0] = m.m[1][1] = m.m[2][2] = 4096;
    SetRotMatrix(&m);
}

/* The object: a 6-triangle fan (a hexagon) of radius 60 world units at
 * depth 2000, centred on world x = tx16 / 16; plus one quad below it. */
static void object_frame(s32 tx16, u32 colour)
{
    static const s16 hex[7][2] = {
        { 0, 0 }, { 60, 0 }, { 30, 52 }, { -30, 52 }, { -60, 0 }, { -30, -52 }, { 30, -52 }
    };
    MATRIX m;
    SVECTOR v[3];
    int t;
    memset(&m, 0, sizeof m);
    m.m[0][0] = m.m[1][1] = m.m[2][2] = 4096;
    m.t[0] = tx16 / 16;
    m.t[1] = 0;
    m.t[2] = 2000;
    SetRotMatrix(&m);
    SetTransMatrix(&m);
    pkt_begin();
    /* clear the draw area (GP0 02h) */
    pkt_word(0x02000000u);
    pkt_word(xy(0, 0));
    pkt_word(xy(320, 240));
    for (t = 0; t < 6; t++) {
        const s16 *a = hex[0], *b = hex[1 + t], *c = hex[1 + (t + 1) % 6];
        memset(v, 0, sizeof v);
        v[0].vx = a[0]; v[0].vy = a[1];
        v[1].vx = b[0]; v[1].vy = b[1];
        v[2].vx = c[0]; v[2].vy = c[1];
        gte_cop2_ldv(0, &v[0]);
        gte_cop2_ldv(1, &v[1]);
        gte_cop2_ldv(2, &v[2]);
        gte_cop2_rtpt();
        pkt_word(0x20000000u | colour);        /* POLY_F3 */
        pkt_word(gte_cop2_sxy(0));
        pkt_word(gte_cop2_sxy(1));
        pkt_word(gte_cop2_sxy(2));
    }
    /* a quad below it: RTPT v0..v2, RTPS v3, as the quad builders do */
    memset(v, 0, sizeof v);
    v[0].vx = -50; v[0].vy = 80;
    v[1].vx = 50;  v[1].vy = 80;
    v[2].vx = -50; v[2].vy = 120;
    gte_cop2_ldv(0, &v[0]);
    gte_cop2_ldv(1, &v[1]);
    gte_cop2_ldv(2, &v[2]);
    gte_cop2_rtpt();
    pkt_word(0x28000000u | colour);            /* POLY_F4 */
    pkt_word(gte_cop2_sxy(0));
    pkt_word(gte_cop2_sxy(1));
    pkt_word(gte_cop2_sxy(2));
    v[0].vx = 50; v[0].vy = 120;
    gte_cop2_ldv(0, &v[0]);
    gte_cop2_rtps();
    pkt_word(gte_cop2_sxy(2));
    /* debris: small slanted triangles (2..12 native pixels) whose sharp
     * corners make the shadow's precise edges miss native edge blocks */
    for (t = 0; t < 40; t++) {
        s32 bx = (t % 8) * 90 - 340, by = (t / 8) * 70 - 330;
        s32 size = 15 + (t * 7) % 60;
        memset(v, 0, sizeof v);
        v[0].vx = (s16)bx;                v[0].vy = (s16)by;
        v[1].vx = (s16)(bx + size);       v[1].vy = (s16)(by + size / 3);
        v[2].vx = (s16)(bx + size / 4);   v[2].vy = (s16)(by + size);
        gte_cop2_ldv(0, &v[0]);
        gte_cop2_ldv(1, &v[1]);
        gte_cop2_ldv(2, &v[2]);
        gte_cop2_rtpt();
        pkt_word(0x20000000u | (colour ^ 0x00ff00u));
        pkt_word(gte_cop2_sxy(0));
        pkt_word(gte_cop2_sxy(1));
        pkt_word(gte_cop2_sxy(2));
    }
    pkt_draw();
}

/* The object's left vertex as the shadow shows it: the left edge of the
 * leftmost sub-pixel column holding `colour`, in native pixels. */
static double shadow_left_x(u16 colour)
{
    int s = g_hires.scale, x, y, best = -1;
    for (y = 0; y < 240 * s; y++) {
        const u16 *row = g_hires.vram + (size_t)y * g_hires.stride;
        for (x = 0; x < 320 * s && (best < 0 || x < best); x++)
            if (row[x] == colour) { best = x; break; }
    }
    return best < 0 ? -1.0 : (double)best / s;
}

static int mode_stream(void)
{
    u32 hv, hs;
    unsigned long stale = 0;
    int f, x, y;
    init();
    for (f = 0; f < 120; f++) {
        object_frame(f * 5, 0x0000ff);
        /* what the presenter would refill before showing the frame */
        if (g_hires.scale > 1) stale += gpu_hires_resync_rect(0, 0, 320, 240);
    }
    hv = fnv(2166136261u, g_vram, sizeof g_vram);
    hs = 2166136261u;
    if (g_hires.scale > 1)
        for (y = 0; y < 240 * g_hires.scale; y++)
            hs = fnv(hs, g_hires.vram + (size_t)y * g_hires.stride,
                     (size_t)320 * g_hires.scale * 2);
    (void)x;
    /* stale blocks: native polygon pixels whose shadow block was left
     * behind (they would be shown at 1x) */
    printf("%08x %08x %lu %lu\n", (unsigned)hv, (unsigned)hs,
           g_gpu_pgxp_stats.polys_precise, stale);
    return 0;
}

static int mode_jitter(void)
{
    enum { FRAMES = 160 };
    double cx[FRAMES], mx = 0, my = 0, sxx = 0, sxy = 0, slope, rms = 0;
    int f;
    init();
    if (g_hires.scale <= 1) FAIL("this mode needs GPU_HIRES=1 and TOKI_RES_SCALE > 1");
    for (f = 0; f < FRAMES; f++) {
        /* 1 world unit at depth 2000 with H = 300 is 0.15 native pixels */
        object_frame(f * 16, 0x0000ff);
        cx[f] = shadow_left_x(0x001f);
        if (cx[f] < 0) FAIL("frame %d: the object is not in the shadow", f);
        if (getenv("PGXP_JITTER_TRACE")) printf("f %d %.4f\n", f, cx[f]);
    }
    for (f = 0; f < FRAMES; f++) { mx += f; my += cx[f]; }
    mx /= FRAMES; my /= FRAMES;
    for (f = 0; f < FRAMES; f++) {
        sxx += (f - mx) * (f - mx);
        sxy += (f - mx) * (cx[f] - my);
    }
    slope = sxy / sxx;
    for (f = 0; f < FRAMES; f++) {
        double r = cx[f] - (my + slope * (f - mx));
        rms += r * r;
    }
    rms = sqrt(rms / FRAMES);
    printf("rms %.4f slope %.4f precise %lu\n", rms, slope,
           g_gpu_pgxp_stats.polys_precise);
    return 0;
}

static u32 shadow_hash(int x0, int y0, int w, int h)
{
    int s = g_hires.scale, y;
    u32 hh = 2166136261u;
    for (y = y0 * s; y < (y0 + h) * s; y++)
        hh = fnv(hh, g_hires.vram + (size_t)y * g_hires.stride + (size_t)x0 * s,
                 (size_t)w * s * 2);
    return hh;
}

static void clear_area(void)
{
    pkt_begin();
    pkt_word(0x02000000u);
    pkt_word(xy(0, 0));
    pkt_word(xy(320, 240));
    pkt_draw();
}

/* One projected triangle drawn as a POLY_F3 in its own OT (or, with
 * `bump`, after an unrelated DrawOTag closed the generation). */
static void projected_triangle(int bump)
{
    SVECTOR v[3];
    u32 w[3];
    MATRIX m;
    memset(&m, 0, sizeof m);
    m.m[0][0] = m.m[1][1] = m.m[2][2] = 4096;
    m.t[2] = 1700;
    SetRotMatrix(&m);
    SetTransMatrix(&m);
    memset(v, 0, sizeof v);
    v[0].vx = -101; v[0].vy = -77;
    v[1].vx = 133;  v[1].vy = -31;
    v[2].vx = 7;    v[2].vy = 119;
    gte_cop2_ldv(0, &v[0]);
    gte_cop2_ldv(1, &v[1]);
    gte_cop2_ldv(2, &v[2]);
    gte_cop2_rtpt();
    w[0] = gte_cop2_sxy(0); w[1] = gte_cop2_sxy(1); w[2] = gte_cop2_sxy(2);
    if (bump) clear_area();
    pkt_begin();
    if (!bump) {
        pkt_word(0x02000000u);
        pkt_word(xy(0, 0));
        pkt_word(xy(320, 240));
    }
    pkt_word(0x2000ff00u);
    pkt_word(w[0]); pkt_word(w[1]); pkt_word(w[2]);
    pkt_draw();
}

static int mode_rules(void)
{
    unsigned long before;
    unsigned int fx, fy, t[3] = { 0x00100020u, 0x00300040u, 0x00500060u };
    unsigned int tfx[3] = { 1, 2, 3 }, tfy[3] = { 4, 5, 6 }, ofx[3], ofy[3];
    u32 h_int, h_pgxp;
    int i;
    init();
    if (!g_gpu_pgxp_active) FAIL("sub-pixel precision is not active");

    /* 0. the GTE's fractions: floor(exact) is the SXY integer and the
     * fraction is the exact projection's to 1/256 pixel */
    for (i = 0; i < 400; i++) {
        SVECTOR v;
        MATRIX m;
        double ex, ey;
        s32 sx, sy;
        memset(&m, 0, sizeof m);
        m.m[0][0] = m.m[1][1] = m.m[2][2] = 4096;
        m.t[0] = (i * 37) % 300 - 150;
        m.t[1] = (i * 53) % 200 - 100;
        m.t[2] = 900 + (i * 71) % 3000;
        SetRotMatrix(&m);
        SetTransMatrix(&m);
        gpu_pgxp_list_done();   /* one vertex per generation: no ambiguity */
        memset(&v, 0, sizeof v);
        v.vx = (s16)((i * 13) % 101 - 50);
        v.vy = (s16)((i * 29) % 77 - 38);
        gte_cop2_ldv(0, &v);
        gte_cop2_rtps();
        sx = (s16)gte_cop2_sxy(2);
        sy = (s16)(gte_cop2_sxy(2) >> 16);
        ex = 160.0 + 300.0 * (m.t[0] + v.vx) / (double)m.t[2];
        ey = 120.0 + 300.0 * (m.t[1] + v.vy) / (double)m.t[2];
        if (gpu_pgxp_lookup(gte_cop2_sxy(2), &fx, &fy) != 1)
            FAIL("vertex %d: no fractions recorded", i);
        if (fabs(sx + fx / 65536.0 - ex) > 1.0 / 256 || fabs(sy + fy / 65536.0 - ey) > 1.0 / 256)
            FAIL("vertex %d: %d+%u/65536, %d+%u/65536 vs exact %.5f, %.5f",
                 i, sx, fx, sy, fy, ex, ey);
    }

    /* 1. generations: fractions recorded before a DrawOTag are not used by
     * the next one */
    before = g_gpu_pgxp_stats.polys_precise;
    projected_triangle(1);
    if (g_gpu_pgxp_stats.polys_precise != before)
        FAIL("rule 1: a previous generation's fractions were used");
    projected_triangle(0);
    if (g_gpu_pgxp_stats.polys_precise != before + 1)
        FAIL("rule 1 (control): this generation's fractions were not used");
    gpu_pgxp_list_done();
    if (gpu_pgxp_lookup(gte_cop2_sxy(2), &fx, &fy) != 0)
        FAIL("rule 1: a closed generation still answers");

    /* 2. ambiguity: one word, two fractions -> never returned */
    gpu_pgxp_list_done();
    gpu_pgxp_record(0x00440033u, 0x1000, 0x2000);
    if (gpu_pgxp_lookup(0x00440033u, &fx, &fy) != 1 || fx != 0x1000 || fy != 0x2000)
        FAIL("rule 2 (control): a single projection is not returned");
    gpu_pgxp_record(0x00440033u, 0x1000, 0x2000);       /* the same vertex again */
    if (gpu_pgxp_lookup(0x00440033u, &fx, &fy) != 1)
        FAIL("rule 2: a repeated identical projection made the word ambiguous");
    gpu_pgxp_record(0x00440033u, 0x9000, 0x2000);
    if (gpu_pgxp_lookup(0x00440033u, &fx, &fy) != -1)
        FAIL("rule 2: an ambiguous word is returned");
    gpu_pgxp_record_tri(t, tfx, tfy);
    if (gpu_pgxp_lookup_tri(t, ofx, ofy) != 1 || ofx[2] != 3 || ofy[0] != 4)
        FAIL("rule 2 (control): a single triple is not returned");
    tfx[1] = 9;
    gpu_pgxp_record_tri(t, tfx, tfy);
    if (gpu_pgxp_lookup_tri(t, ofx, ofy) != -1)
        FAIL("rule 2: an ambiguous triple is returned");

    /* 3. all-or-nothing: a 2D polygon with one corner equal to a recorded
     * word draws exactly as without sub-pixel precision */
    clear_area();
    gpu_pgxp_record(xy(40, 30), 0xC000, 0xC000);
    pkt_begin();
    pkt_word(0x20ff0000u);
    pkt_word(xy(40, 30)); pkt_word(xy(90, 41)); pkt_word(xy(52, 88));
    before = g_gpu_pgxp_stats.polys_precise;
    pkt_draw();
    if (g_gpu_pgxp_stats.polys_precise != before)
        FAIL("rule 3: a partly matched polygon used fractions");
    h_pgxp = shadow_hash(0, 0, 320, 240);
    gpu_pgxp_set_enabled(0);
    clear_area();
    pkt_begin();
    pkt_word(0x20ff0000u);
    pkt_word(xy(40, 30)); pkt_word(xy(90, 41)); pkt_word(xy(52, 88));
    pkt_draw();
    h_int = shadow_hash(0, 0, 320, 240);
    if (h_int != h_pgxp) FAIL("rule 3: the partly matched polygon moved in the shadow");
    printf("rules ok\n");
    return 0;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";
    if (!strcmp(mode, "config")) {
        gpu_hires_init();
        printf("scale=%d pgxp=%d\n", g_hires.scale, g_gpu_pgxp_active);
        return 0;
    }
    if (!strcmp(mode, "stream")) return mode_stream();
    if (!strcmp(mode, "jitter")) return mode_jitter();
    if (!strcmp(mode, "rules")) return mode_rules();
    FAIL("unknown mode %s", mode);
    return 2;
}
