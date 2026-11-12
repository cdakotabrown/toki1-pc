/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gpu_hires.c -- the shadow VRAM of the internal resolution scale: its
 * configuration, the bulk mirrors, resync and the high-resolution picture.
 * Design and invariants: gpu_hires.h.  The per-pixel paths (shadow polygon
 * rasterization, the sprite / tile / line mirror) live in pc/hal/gpu.c
 * beside the native ones they follow.
 */
#include "hal.h"
#include "gpu_hires.h"
#include "gpu_scanout.h"
#include "profile.h"
#include "pc_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern u16 g_vram[512][1024];
extern void gpu_get_scanout_state(GpuScanoutState *state);
/* settings.c is linked into the game; harnesses go without it */
extern const PcSettings *pc_settings_get(void) __attribute__((weak));

/* pc/hal/gpu_texrep.c; weak: the hires harness links without it */
extern int gpu_texrep_pack_scale(void) __attribute__((weak));
/* pc/hal/gpu_pgxp.c (sub-pixel vertices in the shadow); weak likewise */
extern void gpu_pgxp_init(void) __attribute__((weak));

/* The scale asked for: GPU_HIRES (a profile switch: off under parity, on
 * under enhanced) gates it; the value is TOKI_RES_SCALE (environment or
 * toki.cfg) or else settings.ini's [graphics] resolution_scale.  A loaded
 * texture pack (gpu_texrep_init runs first) raises it to the pack's own
 * scale: replacements are drawn only into the shadow. */
static int hires_wanted_scale(void)
{
    long n, pack = gpu_texrep_pack_scale ? gpu_texrep_pack_scale() : 1;
    if (!hal_feature_on("GPU_HIRES")) {
        if (pack > 1)
            fprintf(stderr, "[HIRES] GPU_HIRES is off: the texture pack cannot be shown\n");
        return 1;
    }
    n = (pc_settings_get && pc_settings_get()) ? pc_settings_get()->resolution_scale : 1;
    n = hal_setting_long("TOKI_RES_SCALE", n);
    if (pack > n) {
        printf("[HIRES] texture pack: internal resolution raised to %ldx\n", pack);
        n = pack;
    }
    if (n < 1) n = 1;
    if (n > GPU_HIRES_MAX_SCALE) n = GPU_HIRES_MAX_SCALE;
    return (int)n;
}

/* The scale this run draws at (1 = off); the settings menu compares it
 * with the saved value to say a restart is needed. */
int gpu_hires_running_scale(void)
{
    return g_hires.scale > 1 ? g_hires.scale : 1;
}

void gpu_hires_init(void)
{
    int s = hires_wanted_scale();
    if (s <= 1 || g_hires.scale > 1) return;
    g_hires.vram = (unsigned short *)calloc((size_t)1024 * 512 * s * s, sizeof(u16));
    g_hires.ref = (unsigned short *)calloc((size_t)1024 * 512, sizeof(u16));
    g_hires.stamp = (unsigned int *)calloc((size_t)1024 * 512, sizeof(unsigned int));
    if (!g_hires.vram || !g_hires.ref || !g_hires.stamp) {
        fprintf(stderr, "[HIRES] cannot allocate the %dx shadow VRAM; staying at 1x\n", s);
        free(g_hires.vram); free(g_hires.ref); free(g_hires.stamp);
        memset(&g_hires, 0, sizeof g_hires);
        g_hires.scale = 1;
        return;
    }
    g_hires.stride = 1024 * s;
    g_hires.prim = 0;
    g_hires.in_poly = 0;
    g_hires.scale = s;
    gpu_hires_mirror_rect(0, 0, 1024, 512);
    printf("[HIRES] internal resolution %dx: polygons at %dx%d per display "
           "(shadow VRAM %dx%d)\n", s, 320 * s, 240 * s, 1024 * s, 512 * s);
    fflush(stdout);
    if (gpu_pgxp_init) gpu_pgxp_init();
}

void gpu_hires_mirror_rect(int x, int y, int w, int h)
{
    int i, j;
    if (g_hires.scale <= 1 || w <= 0 || h <= 0) return;
    if (w > 1024) w = 1024;
    if (h > 512) h = 512;
    for (j = 0; j < h; j++) {
        int py = (y + j) & 511;
        for (i = 0; i < w; i++) {
            int px = (x + i) & 1023;
            gpu_hires_block_set(px, py, g_vram[py][px]);
        }
    }
}

/* After a restore (pc/hal/dbg_state.c dbg_state_after_restore): the native
 * VRAM is the state's, the shadow (session heap, kept out of a state) is
 * still this session's picture -- every block whose native value changed
 * takes the restored one, at 1x, as any other stale block does. */
void gpu_hires_after_restore(void)
{
    unsigned long n;
    if (g_hires.scale <= 1) return;
    g_hires.in_poly = 0;
    n = gpu_hires_resync_rect(0, 0, 1024, 512);
    if (getenv("PC_RESTORE_LOG")) {
        printf("[HIRES] after a restore: %lu stale block(s) refilled\n", n);
        fflush(stdout);
    }
}

unsigned long gpu_hires_resync_rect(int x, int y, int w, int h)
{
    unsigned long n = 0;
    int i, j;
    if (g_hires.scale <= 1) return 0;
    for (j = 0; j < h; j++) {
        int py = y + j;
        if (py < 0 || py >= 512) continue;
        for (i = 0; i < w; i++) {
            int px = x + i;
            if (px < 0 || px >= 1024) continue;
            if (g_hires.ref[py * 1024 + px] != g_vram[py][px]) {
                gpu_hires_block_set(px, py, g_vram[py][px]);
                n++;
            }
        }
    }
    g_hires.resyncs += n;
    return n;
}

/* MoveImage copies native pixels inside VRAM (no wrap; out-of-range pixels
 * are skipped, as gpu.c does).  A source block that is valid carries its
 * high-resolution content along -- a captured frame stays sharp; anything
 * else, and an overlapping copy (whose order the native loop defines),
 * takes the native result at 1x. */
void gpu_hires_move(int sx, int sy, int w, int h, int dx, int dy)
{
    int s = g_hires.scale, x, y, j;
    int overlap;
    if (s <= 1 || w <= 0 || h <= 0) return;
    overlap = sx < dx + w && dx < sx + w && sy < dy + h && dy < sy + h;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int ax = sx + x, ay = sy + y, bx = dx + x, by = dy + y;
            if (ax < 0 || ax >= 1024 || ay < 0 || ay >= 512 ||
                bx < 0 || bx >= 1024 || by < 0 || by >= 512) continue;
            if (!overlap && g_hires.ref[ay * 1024 + ax] == g_vram[ay][ax]) {
                const unsigned short *src = g_hires.vram +
                    (size_t)ay * s * g_hires.stride + (size_t)ax * s;
                unsigned short *dst = g_hires.vram +
                    (size_t)by * s * g_hires.stride + (size_t)bx * s;
                for (j = 0; j < s; j++)
                    memcpy(dst + (size_t)j * g_hires.stride,
                           src + (size_t)j * g_hires.stride, (size_t)s * sizeof(u16));
                g_hires.ref[by * 1024 + bx] = g_vram[by][bx];
            } else {
                gpu_hires_block_set(bx, by, g_vram[by][bx]);
            }
        }
    }
}

static unsigned int hires_rgba(unsigned short p)
{
    unsigned int r = (p & 0x1Fu) << 3, g = ((p >> 5) & 0x1Fu) << 3, b = ((p >> 10) & 0x1Fu) << 3;
    return (r << 24) | (g << 16) | (b << 8) | 0xFFu;
}

/* gpu_scanout_pack's rules (fixed 320x240 viewport, black outside the
 * display rectangle and VRAM, no wrap), N times finer. */
const unsigned int *gpu_hires_pack_display(int *width, int *height)
{
    static unsigned int *pixels;
    static int pixels_scale;
    GpuScanoutState d;
    int s = g_hires.scale, i, j;
    if (s <= 1) return NULL;
    gpu_get_scanout_state(&d);
    if (d.rgb24 || !d.enabled) return NULL;
    if (pixels_scale != s) {
        free(pixels);
        pixels = (unsigned int *)malloc((size_t)GPU_SCANOUT_PIXELS * s * s * sizeof(unsigned int));
        pixels_scale = pixels ? s : 0;
        if (!pixels) return NULL;
    }
    gpu_hires_resync_rect(d.x, d.y, d.width < GPU_SCANOUT_WIDTH ? d.width : GPU_SCANOUT_WIDTH,
                          d.height < GPU_SCANOUT_HEIGHT ? d.height : GPU_SCANOUT_HEIGHT);
    for (j = 0; j < GPU_SCANOUT_HEIGHT * s; j++) {
        int nj = j / s, ni, k;
        unsigned int *out = pixels + (size_t)j * GPU_SCANOUT_WIDTH * s;
        int row_ok = nj < d.height && d.y >= -nj && d.y < 512 - nj;
        const unsigned short *src = row_ok ?
            g_hires.vram + (size_t)(d.y * s + j) * g_hires.stride : NULL;
        for (ni = 0, i = 0; ni < GPU_SCANOUT_WIDTH; ni++) {
            if (row_ok && ni < d.width && d.x >= -ni && d.x < 1024 - ni) {
                const unsigned short *p = src + (size_t)(d.x + ni) * s;
                for (k = 0; k < s; k++) out[i++] = hires_rgba(p[k]);
            } else {
                for (k = 0; k < s; k++) out[i++] = 0x000000FFu;
            }
        }
    }
    *width = GPU_SCANOUT_WIDTH * s;
    *height = GPU_SCANOUT_HEIGHT * s;
    return pixels;
}

int gpu_hires_frame_hash(unsigned int *hash)
{
    int w, h;
    size_t n;
    const unsigned char *p;
    unsigned int v = 2166136261u;
    const unsigned int *px = gpu_hires_pack_display(&w, &h);
    if (!px) return 0;
    p = (const unsigned char *)px;
    for (n = (size_t)w * h * 4; n; n--) { v ^= *p++; v *= 16777619u; }
    *hash = v;
    return 1;
}

int gpu_hires_write_ppm(const char *path)
{
    int w, h, i;
    FILE *f;
    unsigned char *row;
    const unsigned int *px = gpu_hires_pack_display(&w, &h);
    if (!px) return 0;
    f = fopen(path, "wb");
    if (!f) return 0;
    row = (unsigned char *)malloc((size_t)w * 3);
    if (!row) { fclose(f); return 0; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (; h > 0; h--, px += w) {
        for (i = 0; i < w; i++) {
            row[i * 3] = (unsigned char)(px[i] >> 24);
            row[i * 3 + 1] = (unsigned char)(px[i] >> 16);
            row[i * 3 + 2] = (unsigned char)(px[i] >> 8);
        }
        fwrite(row, 1, (size_t)w * 3, f);
    }
    free(row);
    return fclose(f) == 0;
}
