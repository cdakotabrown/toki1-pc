/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sweep_shot.c -- periodic, deduplicated screenshots for visual sweeps.
 * Observation only: it reads the
 * displayed picture at the end of a frame and writes PNG files; it never
 * touches game state, timing or the trace.
 *
 *   PC_SWEEP_DIR=/abs/dir    turn it on; each process writes into its own
 *                            sub-directory DIR/run-<pid>-<n>/
 *   PC_SWEEP_EVERY=N         a capture every N presented frames (default 30)
 *   PC_SWEEP_SETTLE=N        and one N frames after every change of the
 *                            step triple (now_step / now_sub_step /
 *                            now_sub_sub_step; default 12)
 *   PC_SWEEP_MAX=N           at most N files per process (default 4000)
 *
 * A capture whose packed 320x240 scanout hashes like an earlier capture of
 * the same process is skipped, so a static screen costs one file.
 * DIR/run-.../index.tsv: one line per written file
 *   frame vblank step sub sub_sub girl scan_hash file
 * and meta.txt names the route (PC_INPUT, SCENE, TOKI_LANG).
 * Without PC_SWEEP_DIR nothing is computed and nothing is written. */
#include "hal.h"
#include "gpu_scanout.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../3rd/stb/stb_image_write.h"

extern u16 g_vram[512][1024];
extern u8 now_step, now_sub_step, now_sub_sub_step, now_girl;
extern void gpu_get_scanout_state(GpuScanoutState *state);
extern u32 hal_clock_vblanks(void) __attribute__((weak));

#define SEEN_BITS 16
#define SEEN_SIZE (1u << SEEN_BITS)

static int g_state = -1;            /* -1 unread, 0 off, 1 on */
static char g_dir[2048];
static FILE *g_index;
static long g_every = 30, g_settle = 12, g_max = 4000, g_written;
static unsigned long g_frames, g_due = ~0ul;
static unsigned g_last_triple = ~0u;
static u32 g_seen[SEEN_SIZE];       /* open addressing, 0 = empty */
static unsigned g_nseen;

static long env_long(const char *name, long def, long lo) {
    const char *v = getenv(name);
    long n;
    if (!v || !*v) return def;
    n = strtol(v, NULL, 10);
    return n < lo ? def : n;
}

static u32 fnv1a(const void *data, size_t n) {
    const unsigned char *p = (const unsigned char *)data;
    u32 h = 2166136261u;
    while (n--) { h ^= *p++; h *= 16777619u; }
    return h ? h : 1u;
}

/* 1 when h is new (and records it), 0 when seen or the table is full */
static int seen_insert(u32 h) {
    unsigned i = h & (SEEN_SIZE - 1);
    if (g_nseen >= SEEN_SIZE / 2) return 0;
    while (g_seen[i]) {
        if (g_seen[i] == h) return 0;
        i = (i + 1) & (SEEN_SIZE - 1);
    }
    g_seen[i] = h;
    g_nseen++;
    return 1;
}

static void sweep_open(void) {
    const char *dir = getenv("PC_SWEEP_DIR");
    static int serial;
    char path[2200];
    FILE *meta;
    const char *names[] = {"PC_INPUT", "SCENE", "SCENE_NAV", "TOKI_LANG", "PC_SWEEP_ROUTE"};
    int i;
    g_state = 0;
    if (!dir || dir[0] != '/' || strlen(dir) > 1900) return;
    snprintf(g_dir, sizeof g_dir, "%s", dir);        /* mkdir -p */
    for (i = 1; g_dir[i]; i++) {
        if (g_dir[i] != '/') continue;
        g_dir[i] = 0;
        mkdir(g_dir, 0755);
        g_dir[i] = '/';
    }
    if (mkdir(dir, 0755) && errno != EEXIST) return;
    for (;;) {
        snprintf(g_dir, sizeof g_dir, "%s/run-%ld-%d", dir, (long)getpid(), serial++);
        if (!mkdir(g_dir, 0755)) break;
        if (errno != EEXIST || serial > 1000) return;
    }
    snprintf(path, sizeof path, "%s/index.tsv", g_dir);
    g_index = fopen(path, "w");
    if (!g_index) return;
    snprintf(path, sizeof path, "%s/meta.txt", g_dir);
    meta = fopen(path, "w");
    if (meta) {
        for (i = 0; i < (int)(sizeof names / sizeof names[0]); i++) {
            const char *v = getenv(names[i]);
            fprintf(meta, "%s=%s\n", names[i], v ? v : "");
        }
        fclose(meta);
    }
    g_every = env_long("PC_SWEEP_EVERY", 30, 1);
    g_settle = env_long("PC_SWEEP_SETTLE", 12, 0);
    g_max = env_long("PC_SWEEP_MAX", 4000, 1);
    g_state = 1;
}

static void sweep_capture(void) {
    static u32 pixels[GPU_SCANOUT_PIXELS];
    static unsigned char rgb[GPU_SCANOUT_PIXELS * 3];
    GpuScanoutState display;
    char name[64], path[2200];
    int w, h, i;
    u32 hash;
    gpu_get_scanout_state(&display);
    if (!display.enabled) return;
    if (!gpu_scanout_pack(&display, &g_vram[0][0], pixels, GPU_SCANOUT_PIXELS, &w, &h))
        return;
    hash = fnv1a(pixels, sizeof pixels);
    if (!seen_insert(hash)) return;
    for (i = 0; i < GPU_SCANOUT_PIXELS; i++) {
        rgb[i * 3] = (unsigned char)(pixels[i] >> 24);
        rgb[i * 3 + 1] = (unsigned char)(pixels[i] >> 16);
        rgb[i * 3 + 2] = (unsigned char)(pixels[i] >> 8);
    }
    snprintf(name, sizeof name, "f%07lu.png", g_frames);
    snprintf(path, sizeof path, "%s/%s", g_dir, name);
    if (!stbi_write_png(path, GPU_SCANOUT_WIDTH, GPU_SCANOUT_HEIGHT, 3, rgb,
                        GPU_SCANOUT_WIDTH * 3))
        return;
    fprintf(g_index, "%lu\t%lu\t%u\t%u\t%u\t%u\t%08x\t%s\n", g_frames,
            hal_clock_vblanks ? (unsigned long)hal_clock_vblanks() : 0ul,
            (unsigned)now_step, (unsigned)now_sub_step, (unsigned)now_sub_sub_step,
            (unsigned)now_girl, (unsigned)hash, name);
    fflush(g_index);
    g_written++;
}

void sweep_shot_frame_end(void) {
    unsigned triple;
    if (g_state < 0) sweep_open();
    if (g_state != 1 || g_written >= g_max) return;
    g_frames++;
    triple = (unsigned)now_step << 16 | (unsigned)now_sub_step << 8 | now_sub_sub_step;
    if (triple != g_last_triple) {
        g_last_triple = triple;
        g_due = g_frames + (unsigned long)g_settle;
    }
    if (g_frames == g_due || g_frames % (unsigned long)g_every == 0) sweep_capture();
}
