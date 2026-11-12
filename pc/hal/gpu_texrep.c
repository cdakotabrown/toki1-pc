/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gpu_texrep.c -- texture dumping and replacement packs (design and
 * invariants: gpu_texrep.h; player guide: docs/pc-port/PLAYER-GUIDE.md
 * "Texture packs").
 *
 * Settings ([graphics] in settings.ini; the environment wins):
 *   texture_dump = true       TOKI_TEXTURE_DUMP=1
 *   texture_pack = "NAME"     TOKI_TEXTURE_PACK=NAME   (a name, or a path)
 *   (root)                    TOKI_TEXTURE_DIR=DIR     (default <data dir>/textures)
 *   (dump floor)              TOKI_TEXTURE_DUMP_MIN=N  texels (default below)
 * Dumps go to <root>/dump/SLPS-00065/, packs are read from
 * <root>/packs/NAME/.  The profile switch GPU_TEXTURES gates all of it
 * (off under parity, on under enhanced).
 */
#include "hal.h"
#include "gpu_texrep.h"
#include "gpu_hires.h"
#include "gpu_scanout.h"
#include "profile.h"
#include "pc_settings.h"
#include "pc_async_io.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STB_IMAGE_IMPLEMENTATION
#include "../3rd/stb/stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../3rd/stb/stb_image_write.h"

#define TEXREP_GAME_ID "SLPS-00065"
/* Default dump floor, in texels: sources smaller than this are not dumped
 * (they are still replaced when a pack names them).  Measured on the three
 * playthroughs (ENHANCED-MODE.md): below it are the 16x12 text glyphs the
 * game streams in per character, palette rows and 1-line strips. */
#define TEXREP_DUMP_MIN_DEFAULT 256
#define MAX_SOURCES 65536          /* ids 1..65535 in owner[] */
#define MAX_REACH 32               /* distinct sources one primitive notes */

extern u16 g_vram[512][1024];
/* settings.c is linked into the game; harnesses go without it */
extern const PcSettings *pc_settings_get(void) __attribute__((weak));
extern int pc_settings_mkdirs(const char *path, int dir) __attribute__((weak));
extern u32 hal_clock_vblanks(void) __attribute__((weak));
extern void gpu_get_scanout_state(GpuScanoutState *state);
extern void gpu_get_draw_area(int *x, int *y, int *w, int *h) __attribute__((weak));
/* g_texrep_on / g_texrep_replace are defined in gpu.c (the harnesses link
 * it without this file) */

typedef struct {
    int x, y, w, h;               /* native rectangle; w == 0: free */
    unsigned long long hash;
    unsigned char changed;        /* content no longer hashes to `hash` */
    unsigned char dumped_write;   /* t16 "write" dump done or refused */
    /* replacement cache: valid while gen == g_pack_gen */
    unsigned gen;
    int rep_fmt;
    unsigned long long rep_clut;
    int rep;                      /* pack entry index, -1 none */
    unsigned rep_kx, rep_ky;      /* its pixels per texel, 16.16 */
} Source;

typedef struct {
    unsigned long long hash, clut;
    int fmt, tw, th;              /* fmt 0 = 4-bit, 1 = 8-bit, 2 = 15-bit */
    int w, h;                     /* the PNG's size (from its header) */
    char *path;
    unsigned int *px;             /* decoded 0xRRGGBBAA, NULL until used */
    unsigned short *px16;         /* the same as 15-bit / GPU_TEXREP_PX16_* */
    int failed;
} PackEntry;

static struct {
    int dump;
    long dump_min;
    char root[1024], dump_dir[1024], pack_dir[1024];
    FILE *index;
    int pack_scale;
} g_cfg;

static Source *g_src;
static unsigned short *g_owner, *g_oval;
static unsigned g_next_id = 1;
static unsigned g_last_id;         /* the latest upload (strip merging) */
static int g_last_strip_w;
/* frame-buffer uploads of this frame, dumped at its end (a picture sent
 * in columns is complete by then) */
#define MAX_WRITES 64
static unsigned g_writes[MAX_WRITES];
static int g_nwrites;

/* keys already dumped (this run and earlier ones, from index.csv) */
typedef struct { unsigned long long a, b; } Key2;
static Key2 *g_dumped;
static unsigned g_dumped_cap, g_dumped_n;

static PackEntry *g_pack;
static int g_pack_n, g_pack_cap;
static int *g_pack_slots;          /* open addressing over g_pack */
static unsigned g_pack_slot_cap;
static unsigned g_pack_gen = 1;

static unsigned long g_frames;
static GpuTexrepStats g_stats;

/* the primitive being drawn (prim_begin) */
static struct {
    int fmt, page_x, page_y;
    unsigned long long clut;
} g_prim;

/* ---------------------------------------------------------------- */
/* hashing                                                           */
/* ---------------------------------------------------------------- */

static unsigned long long fnv64(unsigned long long h, const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    while (n--) { h ^= *b++; h *= 1099511628211ull; }
    return h;
}
#define FNV64_INIT 14695981039346656037ull

static unsigned long long rect_hash(int x, int y, int w, int h)
{
    unsigned long long v = FNV64_INIT;
    int j;
    unsigned short dims[2];
    dims[0] = (unsigned short)w;
    dims[1] = (unsigned short)h;
    v = fnv64(v, dims, sizeof dims);
    for (j = 0; j < h; j++) v = fnv64(v, &g_vram[y + j][x], (size_t)w * 2u);
    return v;
}

/* The palette a 4/8-bit primitive uses: 16 / 256 entries from its CLUT
 * position (wrapping in X as the GPU's CLUT cache does). */
static unsigned long long clut_hash(unsigned short clut, int fmt)
{
    /* every textured primitive asks (text draws one sprite per glyph), so
     * the last palettes are kept with their entries: a compare is cheaper
     * than the hash, and a changed palette still hashes anew */
    static struct { unsigned short clut, fmt, valid, pal[256]; unsigned long long hash; } cache[16];
    unsigned short pal[256];
    int n = fmt ? 256 : 16, i, x = (clut & 63) * 16, y = (clut >> 6) & 511;
    unsigned slot = (clut ^ (clut >> 6) ^ (unsigned)fmt * 7u) & 15u;
    for (i = 0; i < n; i++) pal[i] = g_vram[y][(x + i) & 1023];
    if (cache[slot].valid && cache[slot].clut == clut && cache[slot].fmt == fmt &&
        !memcmp(cache[slot].pal, pal, (size_t)n * 2u))
        return cache[slot].hash;
    cache[slot].clut = clut;
    cache[slot].fmt = (unsigned short)fmt;
    cache[slot].valid = 1;
    memcpy(cache[slot].pal, pal, (size_t)n * 2u);
    cache[slot].hash = fnv64(FNV64_INIT, pal, (size_t)n * 2u);
    return cache[slot].hash;
}

static unsigned long long key_mix(unsigned long long hash, int fmt, unsigned long long clut)
{
    return hash ^ (clut * 0x9E3779B97F4A7C15ull) ^ ((unsigned long long)(fmt + 1) << 61);
}

/* ---------------------------------------------------------------- */
/* names                                                             */
/* ---------------------------------------------------------------- */

static int fmt_shift(int fmt) { return 2 - fmt; }   /* texels per pixel = 1 << shift */

static void key_name(char *out, size_t n, int fmt, unsigned long long hash,
                     unsigned long long clut, int tw, int th)
{
    if (fmt == 2)
        snprintf(out, n, "t16-%016llx-%dx%d", hash, tw, th);
    else
        snprintf(out, n, "t%d-%016llx-%016llx-%dx%d", fmt ? 8 : 4, hash, clut, tw, th);
}

/* Parse a pack file name (without directory); 1 on success. */
static int parse_name(const char *name, int *fmt, unsigned long long *hash,
                      unsigned long long *clut, int *tw, int *th)
{
    char ext[8];
    int bits, used = 0;
    *clut = 0;
    if (sscanf(name, "t16-%16llx-%dx%d.%4s%n", hash, tw, th, ext, &used) == 4 && !name[used]) {
        *fmt = 2;
    } else if (sscanf(name, "t%d-%16llx-%16llx-%dx%d.%4s%n", &bits, hash, clut, tw, th,
                      ext, &used) == 6 && !name[used] && (bits == 4 || bits == 8)) {
        *fmt = bits == 8 ? 1 : 0;
    } else {
        return 0;
    }
    if (strcmp(ext, "png") && strcmp(ext, "PNG")) return 0;
    return *tw > 0 && *th > 0 && *tw <= 4096 && *th <= 512;
}

/* ---------------------------------------------------------------- */
/* the dumped-key set                                                */
/* ---------------------------------------------------------------- */

static unsigned key2_slot(Key2 k, unsigned cap)
{
    unsigned long long h = k.a * 0x9E3779B97F4A7C15ull ^ k.b;
    return (unsigned)(h ^ (h >> 29)) & (cap - 1);
}

static int dumped_has(Key2 k)
{
    unsigned i;
    if (!g_dumped_cap) return 0;
    for (i = key2_slot(k, g_dumped_cap);; i = (i + 1) & (g_dumped_cap - 1)) {
        if (!g_dumped[i].a && !g_dumped[i].b) return 0;
        if (g_dumped[i].a == k.a && g_dumped[i].b == k.b) return 1;
    }
}

static void dumped_add(Key2 k)
{
    unsigned i;
    if ((g_dumped_n + 1) * 2 > g_dumped_cap) {
        Key2 *old = g_dumped;
        unsigned oc = g_dumped_cap, j;
        g_dumped_cap = oc ? oc * 2 : 4096;
        g_dumped = (Key2 *)calloc(g_dumped_cap, sizeof(Key2));
        g_dumped_n = 0;
        for (j = 0; j < oc; j++)
            if (old[j].a || old[j].b) dumped_add(old[j]);
        free(old);
    }
    for (i = key2_slot(k, g_dumped_cap);; i = (i + 1) & (g_dumped_cap - 1)) {
        if (!g_dumped[i].a && !g_dumped[i].b) { g_dumped[i] = k; g_dumped_n++; return; }
        if (g_dumped[i].a == k.a && g_dumped[i].b == k.b) return;
    }
}

/* Keys seen once, waiting for a second frame (a palette and a sheet that
 * meet for a single frame while the game swaps them are not dumped). */
typedef struct { Key2 k; unsigned long frame; } Pending;
static Pending *g_pend;
static unsigned g_pend_cap, g_pend_n;

/* 1 when k was already seen in an earlier frame; records it otherwise. */
static int pending_confirm(Key2 k)
{
    unsigned i;
    if ((g_pend_n + 1) * 2 > g_pend_cap) {
        Pending *old = g_pend;
        unsigned oc = g_pend_cap, j;
        g_pend_cap = oc ? oc * 2 : 4096;
        g_pend = (Pending *)calloc(g_pend_cap, sizeof(Pending));
        g_pend_n = 0;
        for (j = 0; j < oc; j++)
            if (old[j].k.a || old[j].k.b) {
                for (i = key2_slot(old[j].k, g_pend_cap); g_pend[i].k.a || g_pend[i].k.b;
                     i = (i + 1) & (g_pend_cap - 1)) { }
                g_pend[i] = old[j];
                g_pend_n++;
            }
        free(old);
    }
    for (i = key2_slot(k, g_pend_cap);; i = (i + 1) & (g_pend_cap - 1)) {
        if (!g_pend[i].k.a && !g_pend[i].k.b) {
            g_pend[i].k = k;
            g_pend[i].frame = g_frames;
            g_pend_n++;
            return 0;
        }
        if (g_pend[i].k.a == k.a && g_pend[i].k.b == k.b) return g_pend[i].frame != g_frames;
    }
}

static Key2 key2(int fmt, unsigned long long hash, unsigned long long clut, int tw, int th)
{
    Key2 k;
    k.a = key_mix(hash, fmt, clut);
    k.b = ((unsigned long long)(unsigned)tw << 32) | (unsigned)th | 1ull << 63;
    return k;
}

/* ---------------------------------------------------------------- */
/* dumping                                                           */
/* ---------------------------------------------------------------- */

static unsigned int rgba_of(unsigned short p)
{
    unsigned r = p & 31u, g = (p >> 5) & 31u, b = (p >> 10) & 31u;
    if (!p) return 0;                                    /* transparent */
    r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
    return (r << 24) | (g << 16) | (b << 8) | 255u;
}

static int g_scene = -1;           /* now_step at the last frame end */
static int scene_now(void) { return g_scene; }

/* A dump PNG, encoded and written on the writer thread (pc/hal/async_io.c;
 * weak: the texture harnesses link this file alone and write inline). */
PC_ASYNC_DECLARE_WEAK
typedef struct { char path[1200]; unsigned char *img; int w, h; } DumpJob;
static void dump_job(void *arg)
{
    DumpJob *j = (DumpJob *)arg;
    if (!stbi_write_png(j->path, j->w, j->h, 4, j->img, j->w * 4))
        fprintf(stderr, "[TEXTURES] cannot write %s\n", j->path);
    free(j->img);
    free(j);
}

/* Write source s as format fmt with palette `pal` (NULL for 15-bit). */
static void dump_source(const Source *s, int fmt, const unsigned short *pal,
                        unsigned long long clut, int clut_x, int clut_y, const char *kind)
{
    int sh = fmt_shift(fmt), tw = s->w << sh, th = s->h, x, y;
    char name[96], path[1200];
    unsigned char *img;
    Key2 k = key2(fmt, s->hash, clut, tw, th);
    if (dumped_has(k)) return;
    if ((long)tw * th < g_cfg.dump_min) { g_stats.dump_skipped_small++; dumped_add(k); return; }
    img = (unsigned char *)malloc((size_t)tw * th * 4u);
    if (!img) return;
    for (y = 0; y < th; y++) {
        for (x = 0; x < tw; x++) {
            unsigned short w = g_vram[s->y + y][s->x + (x >> sh)], p;
            unsigned int c;
            if (fmt == 2) p = w;
            else if (fmt == 1) p = pal[(w >> ((x & 1) * 8)) & 255];
            else p = pal[(w >> ((x & 3) * 4)) & 15];
            c = rgba_of(p);
            img[((size_t)y * tw + x) * 4 + 0] = (unsigned char)(c >> 24);
            img[((size_t)y * tw + x) * 4 + 1] = (unsigned char)(c >> 16);
            img[((size_t)y * tw + x) * 4 + 2] = (unsigned char)(c >> 8);
            img[((size_t)y * tw + x) * 4 + 3] = (unsigned char)c;
        }
    }
    key_name(name, sizeof name, fmt, s->hash, clut, tw, th);
    snprintf(path, sizeof path, "%s/%s.png", g_cfg.dump_dir, name);
    if (pc_async_run) {
        /* encoded and written on the writer thread (pc/hal/async_io.c):
         * a dump burst no longer stalls the game thread on file I/O; the
         * worker reports a failed write itself */
        DumpJob *j = (DumpJob *)malloc(sizeof *j);
        if (!j) { free(img); return; }
        snprintf(j->path, sizeof j->path, "%s", path);
        j->img = img;
        j->w = tw;
        j->h = th;
        img = NULL;
        pc_async_run(dump_job, j, (size_t)tw * th * 4u);
    }
    if (!img || stbi_write_png(path, tw, th, 4, img, tw * 4)) {
        dumped_add(k);
        g_stats.dumped++;
        if (g_cfg.index)
            fprintf(g_cfg.index, "%s.png,%s,%d,%d,%d,%d,%d,%d,%d,%d,%lu,%lu,%d\n",
                    name, kind, tw, th, s->x, s->y, s->w, s->h, clut_x, clut_y,
                    g_frames, hal_clock_vblanks ? (unsigned long)hal_clock_vblanks() : 0ul,
                    scene_now());
        if (g_stats.dumped <= 16 || g_stats.dumped % 100 == 0) {
            printf("[TEXTURES] dumped %s.png (%s, %lu so far)\n", name, kind, g_stats.dumped);
            fflush(stdout);
        }
    } else {
        fprintf(stderr, "[TEXTURES] cannot write %s\n", path);
        dumped_add(k);      /* do not retry every frame */
    }
    free(img);
}

/* A source is dumped only while VRAM still holds exactly what was
 * uploaded (it may have been drawn over since). */
static int source_intact(Source *s)
{
    if (s->changed) return 0;
    if (rect_hash(s->x, s->y, s->w, s->h) != s->hash) {
        s->changed = 1;
        g_stats.dump_skipped_changed++;
        return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------- */
/* the pack                                                          */
/* ---------------------------------------------------------------- */

static void pack_free(void)
{
    int i;
    for (i = 0; i < g_pack_n; i++) {
        free(g_pack[i].path);
        free(g_pack[i].px);
        free(g_pack[i].px16);
    }
    free(g_pack);
    free(g_pack_slots);
    g_pack = NULL;
    g_pack_slots = NULL;
    g_pack_n = g_pack_cap = 0;
    g_pack_slot_cap = 0;
    g_pack_gen++;
}

static unsigned pack_slot(unsigned long long hash, int fmt, unsigned long long clut)
{
    unsigned long long h = key_mix(hash, fmt, clut);
    return (unsigned)(h ^ (h >> 31)) & (g_pack_slot_cap - 1);
}

static int pack_find(unsigned long long hash, int fmt, unsigned long long clut, int tw, int th)
{
    unsigned i;
    if (!g_pack_slot_cap) return -1;
    for (i = pack_slot(hash, fmt, clut);; i = (i + 1) & (g_pack_slot_cap - 1)) {
        int e = g_pack_slots[i];
        if (e < 0) return -1;
        if (g_pack[e].hash == hash && g_pack[e].fmt == fmt && g_pack[e].clut == clut &&
            g_pack[e].tw == tw && g_pack[e].th == th)
            return e;
    }
}

static const unsigned int *pack_pixels(int e);

static int pack_scan(void)
{
    DIR *d;
    struct dirent *de;
    unsigned i;
    int best = 1;
    pack_free();
    if (!g_cfg.pack_dir[0]) return 0;
    d = opendir(g_cfg.pack_dir);
    if (!d) {
        fprintf(stderr, "[TEXTURES] texture pack %s: %s\n", g_cfg.pack_dir, strerror(errno));
        return 0;
    }
    while ((de = readdir(d)) != NULL) {
        PackEntry e;
        char path[1400];
        int comp;
        memset(&e, 0, sizeof e);
        if (!parse_name(de->d_name, &e.fmt, &e.hash, &e.clut, &e.tw, &e.th)) continue;
        snprintf(path, sizeof path, "%s/%s", g_cfg.pack_dir, de->d_name);
        if (!stbi_info(path, &e.w, &e.h, &comp) || e.w <= 0 || e.h <= 0) {
            fprintf(stderr, "[TEXTURES] %s: not a readable PNG; ignored\n", path);
            continue;
        }
        if (pack_find(e.hash, e.fmt, e.clut, e.tw, e.th) >= 0) continue;
        e.path = strdup(path);
        if (g_pack_n == g_pack_cap) {
            g_pack_cap = g_pack_cap ? g_pack_cap * 2 : 256;
            g_pack = (PackEntry *)realloc(g_pack, (size_t)g_pack_cap * sizeof(PackEntry));
        }
        g_pack[g_pack_n++] = e;
        /* grow the slot table at half load */
        if ((unsigned)g_pack_n * 2u > g_pack_slot_cap) {
            unsigned cap = g_pack_slot_cap ? g_pack_slot_cap * 2u : 1024u;
            int j;
            free(g_pack_slots);
            g_pack_slots = (int *)malloc(cap * sizeof(int));
            g_pack_slot_cap = cap;
            for (i = 0; i < cap; i++) g_pack_slots[i] = -1;
            for (j = 0; j < g_pack_n; j++) {
                for (i = pack_slot(g_pack[j].hash, g_pack[j].fmt, g_pack[j].clut);
                     g_pack_slots[i] >= 0; i = (i + 1) & (cap - 1)) { }
                g_pack_slots[i] = j;
            }
        } else {
            for (i = pack_slot(e.hash, e.fmt, e.clut); g_pack_slots[i] >= 0;
                 i = (i + 1) & (g_pack_slot_cap - 1)) { }
            g_pack_slots[i] = g_pack_n - 1;
        }
        /* the pack's scale: the largest whole factor any file has */
        {
            int f = e.w / e.tw < e.h / e.th ? e.w / e.tw : e.h / e.th;
            if (f > best) best = f;
        }
    }
    closedir(d);
    /* replacements live only in the shadow, so even a 1x (recoloured)
     * pack needs one: 2x at least */
    if (best < 2) best = 2;
    if (best > GPU_HIRES_MAX_SCALE) best = GPU_HIRES_MAX_SCALE;
    g_cfg.pack_scale = best;
    g_stats.pack_files = (unsigned long)g_pack_n;
    g_stats.pack_loaded = 0;
    printf("[TEXTURES] texture pack %s: %d replacement(s), scale %dx\n",
           g_cfg.pack_dir, g_pack_n, best);
    /* TOKI_TEXTURE_PRELOAD=1: decode every PNG now (a longer start, more
     * memory) instead of on first use, where a scene that shows forty new
     * 4x replacements at once stalls a frame by most of a second */
    if (hal_setting_long("TOKI_TEXTURE_PRELOAD", 0) > 0) {
        int i;
        for (i = 0; i < g_pack_n; i++) pack_pixels(i);
        printf("[TEXTURES] preloaded %lu replacement(s)\n", g_stats.pack_loaded);
    }
    fflush(stdout);
    return g_pack_n;
}

static const unsigned int *pack_pixels(int e)
{
    PackEntry *p = &g_pack[e];
    unsigned char *data;
    int w, h, comp, i;
    if (p->px || p->failed) return p->px;
    data = stbi_load(p->path, &w, &h, &comp, 4);
    if (!data || w != p->w || h != p->h) {
        fprintf(stderr, "[TEXTURES] cannot decode %s; using the original\n", p->path);
        if (data) stbi_image_free(data);
        p->failed = 1;
        return NULL;
    }
    p->px = (unsigned int *)malloc((size_t)w * h * sizeof(unsigned int));
    p->px16 = (unsigned short *)malloc((size_t)w * h * sizeof(unsigned short));
    if (p->px && p->px16) {
        for (i = 0; i < w * h; i++) {
            unsigned a = data[i * 4 + 3];
            p->px[i] = ((unsigned)data[i * 4] << 24) | ((unsigned)data[i * 4 + 1] << 16) |
                       ((unsigned)data[i * 4 + 2] << 8) | a;
            p->px16[i] = a < GPU_TEXREP_ALPHA_CLEAR ? GPU_TEXREP_PX16_CLEAR :
                         a < GPU_TEXREP_ALPHA_SOLID ? GPU_TEXREP_PX16_MIX :
                         (unsigned short)((data[i * 4] >> 3) | ((data[i * 4 + 1] >> 3) << 5) |
                                          ((data[i * 4 + 2] >> 3) << 10));
        }
    } else {
        free(p->px);
        free(p->px16);
        p->px = NULL;
        p->px16 = NULL;
        p->failed = 1;
    }
    stbi_image_free(data);
    g_stats.pack_loaded++;
    return p->px;
}

/* The replacement entry of source s under the current primitive, cached
 * per source for one (format, palette) at a time. */
static int source_rep(Source *s, int fmt, unsigned long long clut)
{
    if (s->gen != g_pack_gen || s->rep_fmt != fmt || s->rep_clut != clut) {
        s->gen = g_pack_gen;
        s->rep_fmt = fmt;
        s->rep_clut = clut;
        s->rep = pack_find(s->hash, fmt, clut, s->w << fmt_shift(fmt), s->h);
        if (s->rep >= 0) {
            s->rep_kx = (unsigned)(((unsigned long long)g_pack[s->rep].w << 16) /
                                   (unsigned)(s->w << fmt_shift(fmt)));
            s->rep_ky = (unsigned)(((unsigned long long)g_pack[s->rep].h << 16) / (unsigned)s->h);
        }
    }
    return s->rep;
}

/* ---------------------------------------------------------------- */
/* set-up                                                            */
/* ---------------------------------------------------------------- */

static int env_bool(const char *name, int *out)
{
    const char *v = getenv(name);
    if (!v || !*v) return 0;
    *out = !(v[0] == '0' || !strcasecmp(v, "false") || !strcasecmp(v, "off") ||
             !strcasecmp(v, "no"));
    return 1;
}

static int mkdirs(const char *path)
{
    if (pc_settings_mkdirs) return pc_settings_mkdirs(path, 1);
    {
        char tmp[1024], *p;
        snprintf(tmp, sizeof tmp, "%s", path);
        for (p = tmp + 1; *p; p++)
            if (PC_MKDIR_SPLIT(p)) { char c = *p; *p = 0; mkdir(tmp, 0755); *p = c; }
        return mkdir(tmp, 0755) && errno != EEXIST ? -1 : 0;
    }
}

static void index_open(void)
{
    char path[1200], line[512];
    FILE *f;
    snprintf(path, sizeof path, "%s/index.csv", g_cfg.dump_dir);
    /* earlier sessions' dumps: never rewritten */
    f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            int fmt, tw, th;
            unsigned long long hash, clut;
            char *comma = strchr(line, ',');
            if (!comma) continue;
            *comma = 0;
            if (parse_name(line, &fmt, &hash, &clut, &tw, &th))
                dumped_add(key2(fmt, hash, clut, tw, th));
        }
        fclose(f);
    }
    g_cfg.index = fopen(path, "a");
    if (!g_cfg.index) {
        fprintf(stderr, "[TEXTURES] cannot write %s\n", path);
        return;
    }
    if (ftell(g_cfg.index) == 0)
        fprintf(g_cfg.index, "file,kind,width,height,vram_x,vram_y,vram_w,vram_h,"
                             "clut_x,clut_y,frame,vblank,scene\n");
    fflush(g_cfg.index);
}

static void texrep_exit(void)
{
    if (g_cfg.index) fclose(g_cfg.index);
    g_cfg.index = NULL;
    if (g_cfg.dump)
        printf("[TEXTURES] dump: %lu file(s) written to %s; %lu uploads, %lu small "
               "skipped, %lu changed before use\n", g_stats.dumped, g_cfg.dump_dir,
               g_stats.uploads, g_stats.dump_skipped_small, g_stats.dump_skipped_changed);
    if (g_pack_n || g_cfg.pack_dir[0])
        printf("[TEXTURES] pack: %lu of %lu replacement(s) used\n",
               g_stats.pack_loaded, g_stats.pack_files);
    fflush(stdout);
}

/* <data dir>/textures, or TOKI_TEXTURE_DIR */
static const char *texrep_root(void)
{
    const PcSettings *ps;
    const char *v;
    if (g_cfg.root[0]) return g_cfg.root;
    ps = pc_settings_get ? pc_settings_get() : NULL;
    if ((v = getenv("TOKI_TEXTURE_DIR")) && *v)
        snprintf(g_cfg.root, sizeof g_cfg.root, "%s", v);
    else
        snprintf(g_cfg.root, sizeof g_cfg.root, "%s/textures",
                 ps && ps->data_dir[0] ? ps->data_dir : ".");
    return g_cfg.root;
}

const char *gpu_texrep_packs_dir(void)
{
    static char dir[1100];
    snprintf(dir, sizeof dir, "%s/packs", texrep_root());
    return dir;
}

/* The source registry, once (the first dump or pack). */
static int core_init(void)
{
    if (g_src) return 1;
    if (!hal_feature_on("GPU_TEXTURES")) return 0;
    texrep_root();
    g_cfg.dump_min = hal_setting_long("TOKI_TEXTURE_DUMP_MIN", TEXREP_DUMP_MIN_DEFAULT);
    g_src = (Source *)calloc(MAX_SOURCES, sizeof(Source));
    g_owner = (unsigned short *)calloc(1024 * 512, sizeof(unsigned short));
    g_oval = (unsigned short *)calloc(1024 * 512, sizeof(unsigned short));
    if (!g_src || !g_owner || !g_oval) {
        fprintf(stderr, "[TEXTURES] out of memory; texture dumping and packs are off\n");
        free(g_src); free(g_owner); free(g_oval);
        g_src = NULL; g_owner = g_oval = NULL;
        return 0;
    }
    g_cfg.pack_scale = 1;
    g_texrep_on = 1;
    atexit(texrep_exit);
    return 1;
}

int gpu_texrep_set_dump(int on)
{
    if (!on) {
        if (g_cfg.dump) printf("[TEXTURES] dumping off\n");
        g_cfg.dump = 0;
        g_stats.dump_dir[0] = 0;
        return 0;
    }
    if (g_cfg.dump) return 1;
    if (!core_init()) return -1;
    snprintf(g_cfg.dump_dir, sizeof g_cfg.dump_dir, "%s/dump/%s", g_cfg.root, TEXREP_GAME_ID);
    if (mkdirs(g_cfg.dump_dir)) {
        fprintf(stderr, "[TEXTURES] cannot create %s; dumping is off\n", g_cfg.dump_dir);
        return -1;
    }
    if (!g_cfg.index) index_open();
    g_cfg.dump = 1;
    snprintf(g_stats.dump_dir, sizeof g_stats.dump_dir, "%s", g_cfg.dump_dir);
    printf("[TEXTURES] dumping textures to %s (sources of %ld+ texels)\n",
           g_cfg.dump_dir, g_cfg.dump_min);
    fflush(stdout);
    return 1;
}

int gpu_texrep_set_pack(const char *pack)
{
    if (!pack || !*pack) {
        if (g_texrep_replace) printf("[TEXTURES] texture pack off\n");
        pack_free();
        g_cfg.pack_dir[0] = 0;
        g_stats.pack_dir[0] = 0;
        g_stats.pack_files = 0;
        g_texrep_replace = 0;
        return 0;
    }
    if (!core_init()) return -1;
    if (strchr(pack, '/'))
        snprintf(g_cfg.pack_dir, sizeof g_cfg.pack_dir, "%s", pack);
    else
        snprintf(g_cfg.pack_dir, sizeof g_cfg.pack_dir, "%s/packs/%s", g_cfg.root, pack);
    snprintf(g_stats.pack_dir, sizeof g_stats.pack_dir, "%s", g_cfg.pack_dir);
    g_texrep_replace = 1;
    return pack_scan();
}

/* 1 when the loaded pack cannot be shown at its scale until a restart
 * (the shadow is sized at start-up). */
int gpu_texrep_needs_restart(void)
{
    return g_texrep_replace && g_pack_n && gpu_hires_running_scale() < 2;
}

void gpu_texrep_init(void)
{
    const PcSettings *ps = pc_settings_get ? pc_settings_get() : NULL;
    const char *v, *pack = NULL;
    int dump = 0;
    if (g_texrep_on || !hal_feature_on("GPU_TEXTURES")) return;
    if (!env_bool("TOKI_TEXTURE_DUMP", &dump) && ps) dump = ps->texture_dump;
    v = getenv("TOKI_TEXTURE_PACK");
    if (v) pack = v;
    else if (ps) pack = ps->texture_pack;
    if (!dump && !(pack && *pack)) return;
    if (dump) gpu_texrep_set_dump(1);
    if (pack && *pack) gpu_texrep_set_pack(pack);
    fflush(stdout);
}

int gpu_texrep_pack_scale(void)
{
    return g_texrep_replace && g_pack_n ? g_cfg.pack_scale : 1;
}

int gpu_texrep_reload(void)
{
    int n;
    if (!g_texrep_replace) return 0;
    n = pack_scan();
    if (g_cfg.pack_scale > gpu_hires_running_scale())
        printf("[TEXTURES] the pack is %dx, the shadow %dx: restart to draw it at %dx\n",
               g_cfg.pack_scale, gpu_hires_running_scale(), g_cfg.pack_scale);
    return n;
}

void gpu_texrep_stats(GpuTexrepStats *out)
{
    *out = g_stats;
}

/* ---------------------------------------------------------------- */
/* sources                                                           */
/* ---------------------------------------------------------------- */

static void source_release(unsigned id)
{
    Source *s = &g_src[id];
    int i, j;
    if (!s->w) return;
    for (j = 0; j < s->h; j++)
        for (i = 0; i < s->w; i++)
            if (g_owner[(s->y + j) * 1024 + s->x + i] == id)
                g_owner[(s->y + j) * 1024 + s->x + i] = 0;
    s->w = 0;
    g_stats.sources_live--;
}

static int rects_meet(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh)
{
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

/* An upload into the picture the GPU shows or draws: a frame-buffer
 * write (a full-screen picture or part of one), not a texture. */
static int in_frame_buffer(const Source *s)
{
    GpuScanoutState d;
    int x, y, w, h;
    gpu_get_scanout_state(&d);
    if (d.enabled && !d.rgb24 &&
        rects_meet(s->x, s->y, s->w, s->h, d.x, d.y,
                   d.width < GPU_SCANOUT_WIDTH ? d.width : GPU_SCANOUT_WIDTH,
                   d.height < GPU_SCANOUT_HEIGHT ? d.height : GPU_SCANOUT_HEIGHT))
        return 1;
    if (gpu_get_draw_area) {
        gpu_get_draw_area(&x, &y, &w, &h);
        if (w > 1 && h > 1 && rects_meet(s->x, s->y, s->w, s->h, x, y, w, h)) return 1;
    }
    return 0;
}

/* Paint a t16 replacement into the shadow blocks of source s. */
static void paint_write(const Source *s, int e)
{
    const unsigned int *px = pack_pixels(e);
    const PackEntry *p = &g_pack[e];
    int sc = g_hires.scale, X, Y, W = s->w * sc, H = s->h * sc;
    if (!px || sc <= 1) return;
    for (Y = 0; Y < H; Y++) {
        int iy = (int)(((long long)(2 * Y + 1) * p->h) / (2LL * H));
        unsigned short *row = g_hires.vram + (size_t)(s->y * sc + Y) * g_hires.stride +
                              (size_t)s->x * sc;
        for (X = 0; X < W; X++) {
            int ix = (int)(((long long)(2 * X + 1) * p->w) / (2LL * W));
            unsigned int c = px[(size_t)iy * p->w + ix], a = c & 255u;
            unsigned short v;
            if (a < GPU_TEXREP_ALPHA_CLEAR) continue;   /* the 1x picture shows */
            /* bit 15 stays the native pixel's */
            v = (unsigned short)(((c >> 27) & 31u) | (((c >> 19) & 31u) << 5) |
                                 (((c >> 11) & 31u) << 10) |
                                 (g_vram[s->y + Y / sc][s->x + X / sc] & 0x8000u));
            row[X] = a < GPU_TEXREP_ALPHA_SOLID ? gpu_texrep_alpha_mix(row[X], v, a) : v;
        }
    }
    g_stats.samples_replaced++;
}

void gpu_texrep_upload(int x, int y, int w, int h)
{
    unsigned id;
    Source *s;
    int i, j;
    if (!g_texrep_on || w <= 0 || h <= 0) return;
    g_stats.uploads++;
    x &= 1023;
    y &= 511;
    if (x + w > 1024 || y + h > 512) {
        /* a wrapping upload: not a texture source (never seen here) */
        return;
    }
    /* A picture decoded by the MDEC arrives as one upload per 16-pixel
     * macroblock column (320x240 = 20 uploads of 16x240): a column that
     * continues the previous one -- same width, same rows, next to it --
     * joins its source, so the picture is one texture.  Narrow and tall
     * only: sheets side by side (64x128 each) and text glyphs (4x12) stay
     * separate. */
    s = g_last_id ? &g_src[g_last_id] : NULL;
    if (s && s->w && w <= 32 && h >= 64 && w == g_last_strip_w && s->y == y &&
        s->h == h && s->x + s->w == x && !s->changed &&
        rect_hash(s->x, s->y, s->w, s->h) == s->hash) {
        id = g_last_id;
        s->w += w;
        s->hash = rect_hash(s->x, s->y, s->w, s->h);
        s->gen = 0;                        /* replacement cache: new key */
        s->dumped_write = 0;
    } else {
        id = g_next_id;
        g_next_id = g_next_id + 1 >= MAX_SOURCES ? 1 : g_next_id + 1;
        source_release(id);
        s = &g_src[id];
        memset(s, 0, sizeof *s);
        s->x = x; s->y = y; s->w = w; s->h = h;
        s->rep = -1;
        s->hash = rect_hash(x, y, w, h);
        g_stats.sources_live++;
    }
    g_last_id = id;
    g_last_strip_w = w <= 32 ? w : 0;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) {
            g_owner[(y + j) * 1024 + x + i] = (unsigned short)id;
            g_oval[(y + j) * 1024 + x + i] = g_vram[y + j][x + i];
        }
    if (in_frame_buffer(s)) {
        if (g_texrep_replace && g_hires.scale > 1) {
            int e = source_rep(s, 2, 0);
            if (e >= 0) paint_write(s, e);
        }
        if (g_cfg.dump && !s->dumped_write && g_nwrites < MAX_WRITES) {
            s->dumped_write = 1;
            g_writes[g_nwrites++] = id;
        }
    }
}

static unsigned owner_at(int x, int y);

void gpu_texrep_move(int sx, int sy, int w, int h, int dx, int dy)
{
    unsigned id;
    int i, j;
    if (!g_texrep_on || w <= 0 || h <= 0 || dx < 0 || dy < 0 || dx + w > 1024 ||
        dy + h > 512 || sx < 0 || sy < 0 || sx + w > 1024 || sy + h > 512)
        return;
    /* the copy of a whole, unchanged upload keys as that upload */
    id = owner_at(sx, sy);
    if (id && g_src[id].x == sx && g_src[id].y == sy && g_src[id].w == w &&
        g_src[id].h == h && rect_hash(dx, dy, w, h) == g_src[id].hash) {
        g_last_id = 0;
        gpu_texrep_upload(dx, dy, w, h);
        return;
    }
    /* anything else (a captured frame, part of a sheet) is not a source */
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) g_owner[(dy + j) * 1024 + dx + i] = 0;
}

/* ---------------------------------------------------------------- */
/* draws                                                             */
/* ---------------------------------------------------------------- */

static unsigned owner_at(int x, int y)
{
    unsigned at = (unsigned)y * 1024u + (unsigned)x, id = g_owner[at];
    if (id && g_oval[at] != g_vram[y][x]) return 0;   /* overwritten since */
    return id;
}

void gpu_texrep_note_draw(unsigned short clut, unsigned short tpage,
                          int u0, int v0, int u1, int v1)
{
    unsigned found[MAX_REACH];
    int nfound = 0, fmt, sh, px0, px1, py0, py1, x, y, k;
    unsigned short pal[256];
    unsigned long long ch = 0;
    int have_pal = 0;
    if (!g_texrep_on || !g_cfg.dump) return;
    g_stats.draws++;
    fmt = (tpage >> 7) & 3;
    if (fmt == 3) return;
    sh = fmt_shift(fmt);
    if (u0 < 0) u0 = 0;
    if (v0 < 0) v0 = 0;
    if (u1 > 255) u1 = 255;
    if (v1 > 255) v1 = 255;
    px0 = (tpage & 15) * 64 + (u0 >> sh);
    px1 = (tpage & 15) * 64 + (u1 >> sh);
    py0 = ((tpage >> 4) & 1) * 256 + v0;
    py1 = ((tpage >> 4) & 1) * 256 + v1;
    if (px1 > 1023) px1 = 1023;
    /* every second pixel and the last row / column: a source is at least
     * one pixel wide, and those under 2 x 2 are below any dump floor */
    for (y = py0; nfound < MAX_REACH; y += 2) {
        if (y > py1) y = py1;
        for (x = px0; nfound < MAX_REACH; x += 2) {
            unsigned id;
            if (x > px1) x = px1;
            id = owner_at(x, y & 511);
            if (id) {
                for (k = 0; k < nfound && found[k] != id; k++) { }
                if (k == nfound) found[nfound++] = id;
            }
            if (x == px1) break;
        }
        if (y == py1) break;
    }
    for (k = 0; k < nfound; k++) {
        Source *s = &g_src[found[k]];
        Key2 key;
        if (fmt < 2 && !have_pal) {
            int n = fmt ? 256 : 16, i, cx = (clut & 63) * 16, cy = (clut >> 6) & 511;
            for (i = 0; i < n; i++) pal[i] = g_vram[cy][(cx + i) & 1023];
            ch = fnv64(FNV64_INIT, pal, (size_t)n * 2u);
            have_pal = 1;
        }
        key = key2(fmt, s->hash, fmt < 2 ? ch : 0, s->w << sh, s->h);
        if (dumped_has(key) || !pending_confirm(key) || !source_intact(s)) continue;
        dump_source(s, fmt, fmt < 2 ? pal : NULL, fmt < 2 ? ch : 0,
                    fmt < 2 ? (clut & 63) * 16 : -1, fmt < 2 ? (clut >> 6) & 511 : -1,
                    "texture");
    }
}

void gpu_texrep_frame_end(int scene)
{
    int i;
    if (!g_texrep_on) return;
    g_scene = scene;
    for (i = 0; i < g_nwrites; i++) {
        Source *s = &g_src[g_writes[i]];
        if (s->w && source_intact(s)) dump_source(s, 2, NULL, 0, -1, -1, "write");
    }
    g_nwrites = 0;
    g_frames++;
    if (g_cfg.index) fflush(g_cfg.index);
}

/* ---------------------------------------------------------------- */
/* replacement sampling                                              */
/* ---------------------------------------------------------------- */

int gpu_texrep_prim_begin(unsigned short clut, unsigned short tpage)
{
    if (!g_texrep_replace || !g_pack_n) return 0;
    g_prim.fmt = (tpage >> 7) & 3;
    if (g_prim.fmt == 3) return 0;
    g_prim.page_x = (tpage & 15) * 64;
    g_prim.page_y = ((tpage >> 4) & 1) * 256;
    g_prim.clut = g_prim.fmt < 2 ? clut_hash(clut, g_prim.fmt) : 0;
    return 1;
}

int gpu_texrep_texel(unsigned u, unsigned v, GpuTexrepTexel *t)
{
    int sh = fmt_shift(g_prim.fmt), vx, vy, e;
    unsigned id;
    Source *s;
    const unsigned int *px;
    u &= 255u;
    v &= 255u;
    vx = (g_prim.page_x + (int)(u >> sh)) & 1023;
    vy = g_prim.page_y + (int)v;
    id = owner_at(vx, vy);
    if (!id) return 0;
    s = &g_src[id];
    e = source_rep(s, g_prim.fmt, g_prim.clut);
    if (e < 0) return 0;
    px = pack_pixels(e);
    if (!px) return 0;
    t->px = px;
    t->px16 = g_pack[e].px16;
    t->w = g_pack[e].w;
    t->h = g_pack[e].h;
    t->tx = (unsigned)(((vx - s->x) << sh) | (int)(u & ((1u << sh) - 1u)));
    t->ty = (unsigned)(vy - s->y);
    t->kx = s->rep_kx;
    t->ky = s->rep_ky;
    g_stats.samples_replaced++;
    return 1;
}

int gpu_texrep_sample(unsigned u, unsigned v, unsigned fu, unsigned fv,
                      unsigned int *rgba)
{
    GpuTexrepTexel t;
    unsigned int c;
    if (!gpu_texrep_texel(u, v, &t)) return 0;
    c = gpu_texrep_texel_at(&t, fu, fv);
    if ((c & 255u) < GPU_TEXREP_ALPHA_CLEAR) return 1;
    *rgba = c;
    return 2;
}
