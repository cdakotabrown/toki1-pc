/* SPDX-License-Identifier: GPL-3.0-or-later */
/* extras_gallery.c -- the gallery's eyes (pc/hal/extras.h): which pictures
 * the player has been shown, captured from the native VRAM at 1x.
 *
 * Two kinds, each measured on this game (docs/pc-port/ENHANCED-MODE.md
 * "Extras", the logs of date_save.txt, spring_events.txt and the seeded
 * GEKO 93 start):
 *
 *  - STILLS: every background, date spot and close-up is a BS file of the
 *    disc's BS/ folder, read to 0x80162000 and decoded by the game's own
 *    decdct (libpress DecDCTvlc on the bitstream, pc/hal/libpress.c), then
 *    uploaded one 16-pixel macroblock column at a time: 20 LoadImages of
 *    16x160 (16x240, 16x96) side by side, e.g. D_09_02.BS (the park of
 *    date_save's date) into (320,256)..(624,256).  DecDCTvlc names the
 *    file (the bitstream's address lies in the BS read, extras_state.c);
 *    the 20 joined columns are the picture.  It enters the gallery once a
 *    15-bit sprite or polygon samples that rectangle -- the picture was put
 *    on screen, not just loaded -- from a copy taken when the last column
 *    landed (so a later overwrite cannot change it).
 *
 *  - EVENT SCENES: a character event's own animation is an EVENT/ .BIN
 *    file read to 0x801B0000 (geko_ev_as01_cd_load.c: EV_AS01 at LBA 32150 ...)
 *    whose 64x128 8-bit sheets are uploaded straight from the file
 *    (0x801B2000, +0x4000, ...) beside a BS background and the bustup.  The
 *    scene exists only as the composed screen, so the gallery keeps the
 *    DISPLAYED frame (the scanout of the native VRAM, gpu_scanout_pack):
 *    over the first 240 frames in which a primitive samples one of the
 *    event's sheets, the brightest one (the scene faded in, not mid-fade).
 *
 * One picture per disc source; the PNG is named by the FNV-1a 64 hash of
 * its pixels.  Read-only: copies of VRAM, nothing written back; the GPU
 * hooks (pc/hal/gpu.c, g_extras_gpu) are not-taken branches unless the
 * extras are on, and the draw hook only while a picture waits to be shown.
 * Statics are the session's (dbg_state.c KEEP_FILES); a state load resets
 * them (extras_gallery_reset). */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "extras.h"
#include "gpu_display.h"
#include "gpu_scanout.h"
#include "types.h"

extern u16 g_vram[512][1024];
extern int g_extras_gpu;        /* pc/hal/gpu.c: 1 uploads, 2 draws */

#define COLS 20                 /* a 320-wide still: 20 macroblock columns */
#define EVENT_WINDOW 240        /* drawn frames an event scene is watched */
#define EVENT_IDLE 120          /* frames without its sheets: the scene is over */
#define STILL_WAIT 600          /* frames a loaded still may wait to be shown */
#define EVENT_WAIT 3600         /* ... an event's sheets (GEKO 93 talks first:
                                 * EV_FU01's were drawn ~700 frames after
                                 * their upload in the seeded run) */
#define SHEETS 8

enum { ST_NONE, ST_COLLECT, ST_SHOWN_WAIT };

static struct {
    int st;
    char src[EX_SOURCE_MAX];
    int x0, y0, h, cols;
    int drawn, wait;
    u16 pix[320 * 256];
} g_still;

typedef struct {
    int on;
    char src[EX_SOURCE_MAX];
    int girl;
    int nrect;
    int rx[SHEETS], ry[SHEETS], rw[SHEETS], rh[SHEETS];
    int drawn;                  /* a sheet was sampled this frame */
    int frames, idle;
    long best;
    int have;
    int step, scene_girl;
    int unseen;                 /* frames since its sheets were uploaded, none drawn */
    unsigned char rgb[320 * 240 * 3];
} EvState;
static EvState g_ev;

static int active(void) { return extras_active(); }

static unsigned long long fnv(const void *p, size_t n, unsigned long long h) {
    const unsigned char *b = p;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}

static void watch_draws(void) {
    if (g_still.st == ST_SHOWN_WAIT || g_ev.on) g_extras_gpu |= 2;
    else g_extras_gpu &= ~2;
}

/* libpress.c DecDCTvlc on a still's bitstream (a host pointer to PS1 RAM) */
void extras_gallery_still(const void *bs) {
    char rel[64];
    if (!active()) return;
    g_still.st = ST_NONE;
    if (ex_file_at((unsigned)(unsigned long)bs, rel, sizeof rel, NULL) && !strncmp(rel, "BS/", 3) &&
        strlen(rel) < EX_SOURCE_MAX && !ex_picture_known(rel)) {
        snprintf(g_still.src, sizeof g_still.src, "%s", rel);
        g_still.st = ST_COLLECT;
        g_still.cols = 0;
    }
    watch_draws();
}

/* gpu.c after an upload's native write (data: the host source of a
 * LoadImage, NULL for a GP0 A0h stream) */
void extras_gallery_upload(int x, int y, int w, int h, const void *data) {
    char rel[64];
    unsigned off;
    if (g_still.st == ST_COLLECT && w == 16 && h >= 64 && h <= 256) {
        /* a macroblock column (other uploads -- a glyph -- pass by); one
         * that does not continue the run ends it */
        if (!g_still.cols) {
            g_still.x0 = x;
            g_still.y0 = y;
            g_still.h = h;
            g_still.cols = 1;
        } else if (x == g_still.x0 + 16 * g_still.cols && y == g_still.y0 && h == g_still.h) {
            g_still.cols++;
        } else {
            g_still.st = ST_NONE;
        }
        if (g_still.st == ST_COLLECT && g_still.cols == COLS) {
            int r;
            if (g_still.x0 + 320 > 1024 || g_still.y0 + g_still.h > 512) {
                g_still.st = ST_NONE;
            } else {
                for (r = 0; r < g_still.h; r++)
                    memcpy(&g_still.pix[r * 320], &g_vram[g_still.y0 + r][g_still.x0], 320 * 2);
                g_still.st = ST_SHOWN_WAIT;
                g_still.drawn = 0;
                g_still.wait = 0;
            }
        }
    }
    /* an event's sheet: a 64x128 upload straight out of an EVENT/ file */
    if (data && w == 64 && h == 128 &&
        ex_file_at((unsigned)(unsigned long)data, rel, sizeof rel, &off) &&
        !strncmp(rel, "EVENT/", 6) && strlen(rel) < EX_SOURCE_MAX) {
        if (!g_ev.on || strcmp(g_ev.src, rel)) {
            if (ex_picture_known(rel)) {
                watch_draws();
                return;
            }
            memset(&g_ev, 0, offsetof(EvState, rgb));
            g_ev.on = 1;
            snprintf(g_ev.src, sizeof g_ev.src, "%s", rel);
            {
                const char *b = strrchr(rel, '/');
                g_ev.girl = (b && !strncmp(b + 1, "EV_", 3)) ? ex_girl_of_code(b + 4) : -1;
            }
        }
        if (g_ev.nrect < SHEETS) {
            g_ev.rx[g_ev.nrect] = x;
            g_ev.ry[g_ev.nrect] = y;
            g_ev.rw[g_ev.nrect] = w;
            g_ev.rh[g_ev.nrect] = h;
            g_ev.nrect++;
        }
    }
    watch_draws();
}

static int overlaps(int ax0, int ay0, int ax1, int ay1, int bx, int by, int bw, int bh) {
    return ax0 < bx + bw && ax1 >= bx && ay0 < by + bh && ay1 >= by;
}

/* gpu.c: a textured primitive about to sample (tpage as the primitive
 * gives it, the inclusive UV box) */
void extras_gallery_draw(unsigned tpage, int u0, int v0, int u1, int v1) {
    int fmt = (int)(tpage >> 7) & 3, sh, px0, px1, py0, py1, i;
    if (fmt == 3) return;
    sh = fmt == 0 ? 2 : fmt == 1 ? 1 : 0;
    if (u0 < 0) u0 = 0;
    if (v0 < 0) v0 = 0;
    if (u1 > 255) u1 = 255;
    if (v1 > 255) v1 = 255;
    px0 = (int)(tpage & 15) * 64 + (u0 >> sh);
    px1 = (int)(tpage & 15) * 64 + (u1 >> sh);
    py0 = (int)((tpage >> 4) & 1) * 256 + v0;
    py1 = (int)((tpage >> 4) & 1) * 256 + v1;
    if (g_still.st == ST_SHOWN_WAIT && fmt == 2 &&
        overlaps(px0, py0, px1, py1, g_still.x0, g_still.y0, 320, g_still.h))
        g_still.drawn = 1;
    if (g_ev.on && fmt != 2)
        for (i = 0; i < g_ev.nrect; i++)
            if (overlaps(px0, py0, px1, py1, g_ev.rx[i], g_ev.ry[i], g_ev.rw[i], g_ev.rh[i])) {
                g_ev.drawn = 1;
                break;
            }
}

static void picture_meta(ExPicture *p, const ExView *v, int kind, const char *src, int w, int h,
                         int girl) {
    memset(p, 0, sizeof *p);
    p->kind = kind;
    p->w = w;
    p->h = h;
    p->unix_time = ex_now();
    p->step = v->step;
    p->girl = girl;
    p->year = v->year;
    p->month = v->month;
    p->day = v->day;
    snprintf(p->source, sizeof p->source, "%s", src);
}

static void still_done(const ExView *v) {
    static unsigned char rgb[320 * 256 * 3];
    ExPicture p;
    unsigned long long hsh = fnv("still", 5, 14695981039346656037ull);
    int i, n = 320 * g_still.h;
    u16 w16[2] = { 320, (u16)g_still.h };
    hsh = fnv(w16, sizeof w16, hsh);
    for (i = 0; i < n; i++) {
        u16 c = g_still.pix[i] & 0x7FFF;       /* the STP bit is the decode mode's */
        unsigned r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
        hsh = fnv(&c, 2, hsh);
        rgb[i * 3] = (unsigned char)((r << 3) | (r >> 2));
        rgb[i * 3 + 1] = (unsigned char)((g << 3) | (g >> 2));
        rgb[i * 3 + 2] = (unsigned char)((b << 3) | (b >> 2));
    }
    picture_meta(&p, v, EX_PIC_STILL, g_still.src, 320, g_still.h, v->scene_girl);
    snprintf(p.hash, sizeof p.hash, "%016llx", hsh);
    ex_picture_add(&p, rgb);
}

static void event_sample(void) {
    static unsigned int px[GPU_SCANOUT_PIXELS];
    GpuScanoutState d;
    int w = 0, h = 0, i;
    long sum = 0;
    gpu_get_scanout_state(&d);
    if (!gpu_scanout_pack(&d, &g_vram[0][0], px, GPU_SCANOUT_PIXELS, &w, &h) || w != 320 ||
        h != 240 || !d.enabled)
        return;
    for (i = 0; i < GPU_SCANOUT_PIXELS; i++)
        sum += ((px[i] >> 24) & 255) + ((px[i] >> 16) & 255) + ((px[i] >> 8) & 255);
    if (g_ev.have && sum <= g_ev.best) return;
    g_ev.best = sum;
    g_ev.have = 1;
    for (i = 0; i < GPU_SCANOUT_PIXELS; i++) {
        g_ev.rgb[i * 3] = (unsigned char)(px[i] >> 24);
        g_ev.rgb[i * 3 + 1] = (unsigned char)(px[i] >> 16);
        g_ev.rgb[i * 3 + 2] = (unsigned char)(px[i] >> 8);
    }
}

static void event_done(const ExView *v) {
    ExPicture p;
    if (g_ev.have && g_ev.best > 0) {
        unsigned long long hsh = fnv("event", 5, 14695981039346656037ull);
        hsh = fnv(g_ev.rgb, sizeof g_ev.rgb, hsh);
        picture_meta(&p, v, EX_PIC_EVENT, g_ev.src, 320, 240,
                     g_ev.girl >= 0 ? g_ev.girl : g_ev.scene_girl);
        p.step = g_ev.step;
        snprintf(p.hash, sizeof p.hash, "%016llx", hsh);
        ex_picture_add(&p, g_ev.rgb);
    }
    g_ev.on = 0;
}

void extras_gallery_frame_end(const ExView *v) {
    g_extras_gpu |= 1;
    if (g_still.st == ST_SHOWN_WAIT) {
        if (g_still.drawn) {
            still_done(v);
            g_still.st = ST_NONE;
        } else if (++g_still.wait > STILL_WAIT) {
            printf("[EXTRAS] gallery: %s loaded but not shown within %d frames\n", g_still.src,
                   STILL_WAIT);
            g_still.st = ST_NONE;
        }
    }
    if (g_ev.on) {
        if (g_ev.drawn) {
            if (!g_ev.frames) {
                g_ev.step = v->step;
                g_ev.scene_girl = v->scene_girl;
            }
            g_ev.frames++;
            g_ev.idle = 0;
            event_sample();
            if (g_ev.frames >= EVENT_WINDOW) event_done(v);
        } else if (g_ev.frames && ++g_ev.idle >= EVENT_IDLE) {
            event_done(v);
        } else if (!g_ev.frames && ++g_ev.unseen > EVENT_WAIT) {
            printf("[EXTRAS] gallery: %s uploaded but never drawn\n", g_ev.src);
            g_ev.on = 0;
        }
        g_ev.drawn = 0;
    }
    watch_draws();
}

void extras_gallery_reset(void) {
    g_still.st = ST_NONE;
    g_ev.on = 0;
    g_extras_gpu = extras_active() ? 1 : 0;
}
