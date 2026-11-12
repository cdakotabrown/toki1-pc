/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Bounded, opt-in native parity artifacts. Offsets count completed hardware
 * VBlanks AFTER trace_anchor(), never game-loop iterations or wall time.
 * The native clock is 33868800 / 566204 Hz (about 59.8173), NOT 60 Hz.
 * Audio uses the clock's ticks/frame and a rational 44100 Hz accumulator.
 * PCM is stereo signed 16-bit little-endian, cumulative since the anchor.
 * Each checkpoint flushes PCM before its JSONL record (safe with _exit).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include "parity.h"
#include "gpu_display.h"
#include "gpu_hires.h"

#define MAX_FRAMES 1800
#define MAX_POINTS 16
#define MASTER_CLOCK 33868800u
#define SAMPLE_RATE 44100u

extern u16 g_vram[512][1024];
extern s32 frame_counter, step_frame_cnt, vblnk_counter, activeBuff;
extern s32 pad_data, pad_trig, pad_rel;
extern u8 now_step, now_sub_step, now_sub_sub_step, now_girl, step_mode;
extern u32 hal_clock_vblanks(void), hal_clock_ticks_per_frame(void);
/* pc/hal/gte.c (hal_gte_cop2.h); weak: the scanout tests link this file alone */
extern void gte_dump_regs(u32 out[64]) __attribute__((weak));
/* pc/hal/gpu_hires.c; weak for the same reason */
extern int gpu_hires_write_ppm(const char *path) __attribute__((weak));

static int g_initialized, g_enabled, g_anchored, g_done;
static int g_points[MAX_POINTS], g_count, g_next, g_offset;
static unsigned long g_remainder, g_samples;
static char g_dir[2048];
static FILE *g_pcm, *g_state;

static void fail(const char *message) {
    fprintf(stderr, "[PARITY] %s: %s\n", message, strerror(errno));
    fflush(NULL);
    _exit(2);
}

static void path_for(char *path, const char *name) {
    sprintf(path, "%s/%s", g_dir, name);
}

static FILE *open_artifact(const char *name, const char *mode) {
    char path[2200];
    FILE *file;
    path_for(path, name);
    file = fopen(path, mode);
    if (!file) fail("cannot open artifact");
    return file;
}

static void write_bytes(FILE *file, const void *data, size_t count) {
    if (fwrite(data, 1, count, file) != count) fail("artifact write failed");
}

static void flush_file(FILE *file) {
    if (fflush(file)) fail("artifact flush failed");
}

/* An absolute path: "/..." everywhere; on Windows also "X:\\..." / "X:/..."
 * and UNC "\\\\server\\..." (the Wine harness passes "/..." which Wine
 * resolves through drive Z:, a real Windows harness passes "C:\\..."). */
static int parity_absolute(const char *p) {
    if (p[0] == '/') return 1;
#ifdef _WIN32
    if (p[0] == '\\' && p[1] == '\\') return 1;
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' &&
        (p[2] == '\\' || p[2] == '/'))
        return 1;
#endif
    return 0;
}

void parity_init(void) {
    const char *dir, *spec, *p;
    struct stat info;
    int previous;
    if (g_initialized) return;
    g_initialized = 1;
    dir = getenv("PC_PARITY_DIR");
    if (!dir) return;
    errno = EINVAL;
    if (!parity_absolute(dir) || strlen(dir) >= sizeof(g_dir))
        fail("PC_PARITY_DIR must be an absolute path shorter than 2048 bytes");
    spec = getenv("PC_PARITY_FRAMES");
    if (!spec || !*spec) fail("PC_PARITY_FRAMES is required");
    p = spec;
    previous = 0;
    while (*p) {
        long value;
        char *end;
        if (*p < '0' || *p > '9' || g_count == MAX_POINTS)
            fail("expected at most 16 strictly increasing positive offsets");
        errno = 0;
        value = strtol(p, &end, 10);
        if (errno || value <= previous || value > MAX_FRAMES ||
            (*end && *end != ',')) {
            errno = EINVAL;
            fail("offsets must increase and be in 1..1800");
        }
        g_points[g_count++] = (int)value;
        previous = (int)value;
        p = end;
        if (*p == ',') {
            p++;
            if (!*p) { errno = EINVAL; fail("trailing comma in offsets"); }
        }
    }
    strcpy(g_dir, dir);
    if (mkdir(g_dir, 0700) && errno != EEXIST) fail("cannot create output directory");
    if (stat(g_dir, &info) || !S_ISDIR(info.st_mode)) fail("output is not a directory");
    g_enabled = 1;
    /* No implicit boot anchor: a missing SCENE_NAV/trace_anchor yields no
     * checkpoint rather than silently capturing a different timeline.
     * PC_PARITY_ANCHOR=boot asks for one explicitly: the capture starts at
     * power-on (the boot movie's XA audio and MDEC frames included;
     * pc/tools/windows_parity.py compares it across platforms). */
    {
        const char *a = getenv("PC_PARITY_ANCHOR");
        if (a && !strcmp(a, "boot")) parity_anchor();
    }
}

int parity_enabled(void) {
    parity_init();
    return g_enabled;
}

void parity_anchor(void) {
    if (!parity_enabled()) return;
    if (g_pcm) fclose(g_pcm);
    if (g_state) fclose(g_state);
    g_pcm = open_artifact("audio.s16le", "wb");
    g_state = open_artifact("state.jsonl", "w");
    g_remainder = g_samples = 0;
    g_next = g_offset = g_done = 0;
    g_anchored = 1;
}

/* The displayed image as SDL would present it (before any host HUD),
 * cropped to the declared display size and the fixed viewport cap. */
static void write_scanout_ppm(FILE *file, GpuScanoutState *out,
                              int *out_width, int *out_height) {
    int x, y, width, height;
    int packed_width, packed_height;
    GpuScanoutState display;
    static u32 pixels[GPU_SCANOUT_PIXELS];
    unsigned char row[GPU_SCANOUT_WIDTH * 3];

    gpu_get_scanout_state(&display);
    *out = display;
    width = display.width;
    height = display.height;
    if (width < 1 || height < 1) { errno = EINVAL; fail("invalid display dimensions"); }
    if (width > GPU_SCANOUT_WIDTH) width = GPU_SCANOUT_WIDTH;
    if (height > GPU_SCANOUT_HEIGHT) height = GPU_SCANOUT_HEIGHT;
    if (!gpu_scanout_pack(&display, &g_vram[0][0], pixels,
                          GPU_SCANOUT_PIXELS, &packed_width, &packed_height)) {
        errno = EINVAL;
        fail("invalid scanout buffers");
    }
    fprintf(file, "P6\n%d %d\n255\n", width, height);
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            u32 pixel;
            pixel = pixels[y * packed_width + x];
            row[x * 3] = (unsigned char)(pixel >> 24);
            row[x * 3 + 1] = (unsigned char)(pixel >> 16);
            row[x * 3 + 2] = (unsigned char)(pixel >> 8);
        }
        write_bytes(file, row, (size_t)width * 3);
    }
    *out_width = width;
    *out_height = height;
}

/* One screenshot outside the checkpoint schedule (PC_INPUT `shot`):
 * PC_SHOT_DIR/<label>.ppm.  Observation only.  Returns 0 when disabled. */
int parity_shot(const char *label) {
    const char *dir = getenv("PC_SHOT_DIR");
    char path[2200];
    FILE *file;
    GpuScanoutState display;
    int w, h;
    if (!dir || *dir != '/' || strlen(dir) + strlen(label) + 6 >= sizeof(path)) return 0;
    if (mkdir(dir, 0700) && errno != EEXIST) return 0;
    sprintf(path, "%s/%s.ppm", dir, label);
    file = fopen(path, "wb");
    if (!file) return 0;
    write_scanout_ppm(file, &display, &w, &h);
    if (fclose(file)) return 0;
    /* With the internal resolution raised, also <label>.hires.ppm: what the
     * presenter shows (pc/hal/gpu_hires.c); <label>.ppm stays the 1x
     * scanout of the native VRAM. */
    if (gpu_hires_write_ppm) {
        sprintf(path, "%s/%s.hires.ppm", dir, label);
        gpu_hires_write_ppm(path);
    }
    /* PC_SHOT_VRAM=1: also <label>.vram, the whole 1024x512 u16le VRAM
     * (the reference's -dumpvram format), and with PC_PARITY_RAM=1 also
     * <label>.ram, the 2 MB PS1 RAM block. */
    {
        const char *v = getenv("PC_SHOT_VRAM");
        const char *ram = getenv("PC_PARITY_RAM");
        unsigned char raw[1024 * 2];
        int x, y;
        if (v && *v && *v != '0') {
            sprintf(path, "%s/%s.vram", dir, label);
            file = fopen(path, "wb");
            if (!file) return 0;
            for (y = 0; y < 512; y++) {
                for (x = 0; x < 1024; x++) {
                    raw[x * 2] = (unsigned char)g_vram[y][x];
                    raw[x * 2 + 1] = (unsigned char)(g_vram[y][x] >> 8);
                }
                write_bytes(file, raw, sizeof(raw));
            }
            if (fclose(file)) return 0;
        }
        if (ram && *ram && *ram != '0') {
            u32 regs[64];
            unsigned char le[256];
            int i;
            sprintf(path, "%s/%s.ram", dir, label);
            file = fopen(path, "wb");
            if (!file) return 0;
            write_bytes(file, (const void *)(unsigned long)0x80000000u, 0x200000u);
            if (fclose(file)) return 0;
            /* <label>.gte: the 64 GTE registers, u32le, in the order a
             * retail save state stores them (lit_retail_check.py --gte) */
            if (!gte_dump_regs) return 1;
            gte_dump_regs(regs);
            for (i = 0; i < 64; i++) {
                le[4 * i] = (unsigned char)regs[i];
                le[4 * i + 1] = (unsigned char)(regs[i] >> 8);
                le[4 * i + 2] = (unsigned char)(regs[i] >> 16);
                le[4 * i + 3] = (unsigned char)(regs[i] >> 24);
            }
            sprintf(path, "%s/%s.gte", dir, label);
            file = fopen(path, "wb");
            if (!file) return 0;
            write_bytes(file, le, sizeof le);
            if (fclose(file)) return 0;
        }
    }
    return 1;
}

/* PC_FRAME_HASH=PATH: one line per presented frame (hal_frame_end), so two
 * builds or two settings can be compared frame by frame without storing
 * images: the frame's index, the VBlank, FNV-1a 32 of the whole native
 * VRAM and of the packed 320x240 scanout, and -- when the internal
 * resolution is raised (pc/hal/gpu_hires.c) -- of the high-resolution
 * picture.  Observation only; nothing is written without the variable. */
static u32 fnv1a(u32 h, const void *data, size_t n) {
    const unsigned char *p = (const unsigned char *)data;
    while (n--) { h ^= *p++; h *= 16777619u; }
    return h;
}

/* Host residue in the PS1 RAM block (PC_FRAME_HASH_RAM=game; the Python
 * twin is pc/tools/ps1_residue.py, and test_ps1_residue.py checks that the
 * two agree).  Matched code copies two buffers whose tails it never
 * writes, so those bytes hold whatever the host stack held there:
 *   - the current draw environment libgpu_cur_drawenv (DRAWENV, 92 bytes):
 *     PutDrawEnv builds its DR_ENV packet at +0x1C, and the words after
 *     the packet's length (tag bits 24-31) are the caller's uninitialised
 *     stack local;
 *   - the CD command ring cdq_que (48 records of 24 bytes): every command
 *     with data copies 16 bytes into data[0..14] + flag (cdq_command_enqueue);
 *     the reader (cdq_command_process) uses type, arg, data[0..3] (f8,
 *     whose first two bytes CdControl reads) and the flag, never data[4..14]
 *     (record +12..+22).  A read (type 6) copies the destination buffer's
 *     old contents, which may be host memory; a set-mode (type 14) copies
 *     cd_command_issue's 12-byte local, of which only data[0] is set, so
 *     its +9..+11 are residue too.
 * The game never reads them back (the traces, which carry its state, are
 * identical), but their values depend on which host code ran on the same
 * stack before -- on Windows a feature that changes the host's call depth
 * changes them.  RAM is the 2 MB block at 0x80000000; offsets are into it. */
#define RESIDUE_DRAWENV 0x0EBD70u
#define RESIDUE_CDQ_QUE 0x0E8DA8u
void parity_mask_host_residue(unsigned char *ram) {
    unsigned len, i;
    unsigned char *tag = ram + RESIDUE_DRAWENV + 0x1C;
    len = tag[3];
    for (i = 1 + len; i < (92 - 0x1C) / 4; i++)
        memset(tag + 4 * i, 0, 4);
    for (i = 0; i < 48; i++) {
        unsigned char *rec = ram + RESIDUE_CDQ_QUE + 24 * i;
        memset(rec + 12, 0, 11);                     /* data[4..14]: never read */
        if (rec[0] == 14) memset(rec + 9, 0, 3);     /* set-mode reads data[0] only */
    }
}

/* pc/hal/gpu_hires.c; weak: harnesses link this file alone */
extern int gpu_hires_frame_hash(u32 *hash) __attribute__((weak));
extern GpuHiresState g_hires __attribute__((weak));

void parity_frame_hash(void) {
    static int init;
    static FILE *out;
    static unsigned long frames;
    static u32 pixels[GPU_SCANOUT_PIXELS];
    GpuScanoutState display;
    int w, h;
    u32 hv, hs, hh;
    if (!init) {
        const char *path = getenv("PC_FRAME_HASH");
        init = 1;
        if (path && *path) {
            out = fopen(path, "w");
            if (!out) fail("cannot open PC_FRAME_HASH");
        }
    }
    if (!out) return;
    gpu_get_scanout_state(&display);
    hv = fnv1a(2166136261u, g_vram, sizeof g_vram);
    hs = 0;
    if (gpu_scanout_pack(&display, &g_vram[0][0], pixels, GPU_SCANOUT_PIXELS, &w, &h))
        hs = fnv1a(2166136261u, pixels, sizeof pixels);
    fprintf(out, "%lu vblank=%lu vram=%08x scan=%08x", frames++,
            (unsigned long)hal_clock_vblanks(), (unsigned)hv, (unsigned)hs);
    /* PC_FRAME_HASH_RAM=1: also the 2 MB PS1 RAM block (every game global).
     * Game code keeps a few stack addresses in globals, so compare runs in
     * the fixed layout (TOKI_SAVE_DIR set: no ASLR, the pinned game stack,
     * pc/src/main.c), or the hash differs from run to run.
     * PC_FRAME_HASH_RAM=cdq: the same without bytes 12..23 of the 48 CD
     * queue records (cdq_que, 0x800E8DA8): uninitialised host stack bytes
     * cd_read_start hands the queue, which on Windows hold host wall-clock
     * leftovers in every run (pc/tools/savestate_tool.py cdq_noise). */
    {
        /* PC_FRAME_HASH_RAM=game: the same block with the host residue
         * cleared (parity_mask_host_residue above) -- the setting for
         * comparisons of the game's state between runs whose host code
         * differs (a feature on and off) */
        static int ram = -1;
        static unsigned char *copy;
        if (ram < 0) {
            const char *e = getenv("PC_FRAME_HASH_RAM");
            ram = !e || !*e || *e == '0' ? 0 : !strcmp(e, "cdq") ? 2 : !strcmp(e, "game") ? 3 : 1;
            if (ram == 3 && !(copy = (unsigned char *)malloc(0x200000u)))
                fail("PC_FRAME_HASH_RAM=game: out of memory");
        }
        if (ram == 3) {
            memcpy(copy, (const void *)(unsigned long)0x80000000u, 0x200000u);
            parity_mask_host_residue(copy);
            fprintf(out, " ram=%08x", (unsigned)fnv1a(2166136261u, copy, 0x200000u));
        } else if (ram == 1)
            fprintf(out, " ram=%08x",
                    (unsigned)fnv1a(2166136261u, (const void *)(unsigned long)0x80000000u, 0x200000u));
        else if (ram == 2) {
            const unsigned long base = 0x80000000ul, que = 0x800E8DA8ul;
            unsigned long at = base;
            u32 h = 2166136261u;
            int i;
            for (i = 0; i < 48; i++) {
                unsigned long rec = que + 24ul * (unsigned long)i;
                h = fnv1a(h, (const void *)at, rec + 12 - at);
                at = rec + 24;
            }
            h = fnv1a(h, (const void *)at, base + 0x200000ul - at);
            fprintf(out, " ram=%08x", (unsigned)h);
        }
    }
    /* resync: blocks refilled from native so far (pc/hal/gpu_hires.h) */
    if (gpu_hires_frame_hash && gpu_hires_frame_hash(&hh))
        fprintf(out, " hires=%08x resync=%lu", (unsigned)hh,
                &g_hires ? g_hires.resyncs : 0ul);
    fputc('\n', out);
    fflush(out);
}

static void capture(void) {
    char name[80];
    FILE *file;
    int x, y, width, height, crop_x, crop_y;
    GpuScanoutState display;
    unsigned char raw[1024 * 2];

    /* Observe the same state and packing as SDL, before optional host HUD.
     * Keep the artifact's declared-size crop and fixed viewport cap. */
    sprintf(name, "frame_%06d.ppm", g_offset);
    file = open_artifact(name, "wb");
    write_scanout_ppm(file, &display, &width, &height);
    crop_x = display.x;
    crop_y = display.y;
    if (fclose(file)) fail("image close failed");
    sprintf(name, "frame_%06d.vram", g_offset);
    file = open_artifact(name, "wb");
    for (y = 0; y < 512; y++) {
        for (x = 0; x < 1024; x++) {
            raw[x * 2] = (unsigned char)g_vram[y][x];
            raw[x * 2 + 1] = (unsigned char)(g_vram[y][x] >> 8);
        }
        write_bytes(file, raw, sizeof(raw));
    }
    if (fclose(file)) fail("VRAM close failed");
    /* PC_PARITY_RAM=1: also snapshot the 2 MB PS1 RAM block (data symbols
     * resolve into it, see pc/src/main.c) so packets, OTs and object state
     * can be compared with the reference's -dumpram at the same VBlank. */
    {
        const char *ram = getenv("PC_PARITY_RAM");
        if (ram && *ram && *ram != '0') {
            sprintf(name, "frame_%06d.ram", g_offset);
            file = open_artifact(name, "wb");
            write_bytes(file, (const void *)(unsigned long)0x80000000u,
                        0x200000u);
            if (fclose(file)) fail("RAM close failed");
        }
    }
    flush_file(g_pcm);
    fprintf(g_state,
        "{\"schema\":1,\"anchor\":\"trace_anchor\",\"frame\":%d,\"vblank\":%lu,"
        "\"frame_counter\":%ld,\"step_frame_cnt\":%ld,\"vblnk_counter\":%ld,"
        "\"now_step\":%u,\"now_sub_step\":%u,\"now_sub_sub_step\":%u,\"now_girl\":%u,"
        "\"step_mode\":%u,\"disp_buf\":%ld,\"pad_data\":%ld,\"pad_trig\":%ld,\"pad_rel\":%ld,"
        "\"width\":%d,\"height\":%d,\"crop\":[%d,%d,%d,%d],\"display\":[%ld,%ld,%ld,%ld],"
        "\"image\":\"frame_%06d.ppm\",\"vram\":\"frame_%06d.vram\","
        "\"image_source\":\"gpu_scanout\",\"rgb_expansion\":\"%s\","
        "\"display_enabled\":%d,\"display_rgb24\":%d,"
        "\"vram_width\":1024,\"vram_height\":512,\"vram_format\":\"u16le\","
        "\"audio\":\"audio.s16le\",\"audio_format\":\"s16le\",\"audio_channels\":2,"
        "\"audio_rate\":44100,\"audio_samples\":%lu,\"audio_bytes\":%lu,"
        "\"clock_hz\":33868800,\"ticks_per_vblank\":%lu}\n",
        g_offset, (unsigned long)hal_clock_vblanks(), (long)frame_counter,
        (long)step_frame_cnt, (long)vblnk_counter, now_step, now_sub_step,
        now_sub_sub_step, now_girl, step_mode, (long)activeBuff, (long)pad_data,
        (long)pad_trig, (long)pad_rel, width, height, crop_x, crop_y, width, height,
        (long)display.x, (long)display.y,
        (long)display.width, (long)display.height,
        g_offset, g_offset, display.rgb24 ? "packed8" : "shift3",
        display.enabled != 0, display.rgb24 != 0,
        g_samples, g_samples * 4,
        (unsigned long)hal_clock_ticks_per_frame());
    flush_file(g_state);
}

void parity_vblank(void) {
    s16 samples[1024 * 2];
    unsigned char bytes[1024 * 4];
    unsigned long count, i;
    if (!parity_enabled() || g_done) return;
    /* 44100 * 566204 overflows 32-bit unsigned long. The master clock
     * is exactly 768 audio sample periods; keep arithmetic C89/32-bit safe. */
    g_remainder += hal_clock_ticks_per_frame();
    count = g_remainder / (MASTER_CLOCK / SAMPLE_RATE);
    g_remainder %= MASTER_CLOCK / SAMPLE_RATE;
    if (count > 1024) { errno = EINVAL; fail("unsupported VBlank audio rate"); }
    spu_parity_mix(samples, (int)count);
    if (!g_anchored) return;
    for (i = 0; i < count * 2; i++) {
        bytes[i * 2] = (unsigned char)samples[i];
        bytes[i * 2 + 1] = (unsigned char)((u16)samples[i] >> 8);
    }
    write_bytes(g_pcm, bytes, (size_t)count * 4);
    g_samples += count;
    g_offset++;
    if (g_offset == g_points[g_next]) {
        capture();
        g_next++;
        if (g_next == g_count) {
            if (fclose(g_pcm) || fclose(g_state)) fail("final close failed");
            g_pcm = g_state = NULL;
            g_done = 1;
        }
    }
}
