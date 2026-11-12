/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gpu_hires.h -- internal resolution scaling for the native rasterizer
 * (the enhanced profile's GPU_HIRES).
 *
 * The 1024x512 native VRAM (g_vram) stays the only VRAM the game sees:
 * every draw is still rasterized into it exactly as at 1x, and every read
 * (StoreImage, MoveImage, texture and CLUT fetches, blending) reads it.
 * Beside it the HAL keeps a SHADOW of the whole VRAM at N times the
 * resolution, in which each native pixel P is an N x N block:
 *
 *   - a polygon is rasterized a second time into the shadow from its
 *     vertices times N (pc/hal/gpu.c gpu_raster_triangle), sampling its
 *     texture from the native VRAM;
 *   - every other write (sprites, tiles, lines, fills, LoadImage, GP0
 *     A0h, MoveImage) is mirrored into the shadow by nearest upscaling,
 *     so 2D looks exactly as at 1x;
 *   - ref[P] records the native value P had when its block was last made
 *     valid.  A block whose ref differs from g_vram[P] is stale (something
 *     wrote the native pixel without the shadow) and is refilled from the
 *     native pixel -- that region falls back to 1x -- before it is drawn
 *     on or presented.
 *
 * The presenter shows the shadow's display area.  With scale 1 (parity's
 * default, and enhanced's until the player raises it) none of this exists:
 * no buffer is allocated and every hook is a not-taken branch.
 */
#ifndef PC_GPU_HIRES_H
#define PC_GPU_HIRES_H

#include <stddef.h>

#define GPU_HIRES_MAX_SCALE 4

typedef struct {
    int scale;              /* 1 = off */
    int stride;             /* shadow row length: 1024 * scale */
    unsigned short *vram;   /* (512 * scale) rows of `stride` */
    unsigned short *ref;    /* 512 x 1024: native value each block is valid for */
    unsigned int *stamp;    /* 512 x 1024: last shadow triangle that touched it */
    unsigned int prim;      /* current shadow triangle */
    int in_poly;            /* the native pass of a polygon is running */
    unsigned long resyncs;  /* stale blocks refilled from native (diagnostic) */
} GpuHiresState;

extern GpuHiresState g_hires;   /* pc/hal/gpu.c */
int gpu_hires_running_scale(void);

/* Fill native pixel (px, py)'s block with `value` and make it valid. */
static __inline__ void gpu_hires_block_set(int px, int py, unsigned short value)
{
    int s = g_hires.scale, i, j;
    unsigned short *row = g_hires.vram + (size_t)py * s * g_hires.stride + (size_t)px * s;
    for (j = 0; j < s; j++, row += g_hires.stride)
        for (i = 0; i < s; i++) row[i] = value;
    g_hires.ref[py * 1024 + px] = value;
}

/* pc/hal/gpu_hires.c.  gpu.c reaches these through weak references and
 * only when g_hires.scale > 1, which only gpu_hires_init sets, so the GPU
 * harnesses that link gpu.c alone are unaffected. */
void gpu_hires_init(void);
/* Native rect -> shadow by nearest upscaling; x/y wrap at 1024/512. */
void gpu_hires_mirror_rect(int x, int y, int w, int h);
/* After the native MoveImage of the clipped rect (sx,sy,w,h) -> (dx,dy). */
void gpu_hires_move(int sx, int sy, int w, int h, int dx, int dy);
/* Refill every stale block of a native rect (the display before it is
 * shown); returns the number refilled. */
unsigned long gpu_hires_resync_rect(int x, int y, int w, int h);
/* The display area at N x: RGBA8888 (0xRRGGBBAA words, as gpu_scanout)
 * of 320N x 240N, after resyncing it.  NULL when off or not applicable
 * (scale 1, RGB24 movie scanout): the caller shows the native scanout. */
const unsigned int *gpu_hires_pack_display(int *width, int *height);
/* PC_FRAME_HASH's hires column (pc/hal/parity.c); 0 when off. */
int gpu_hires_frame_hash(unsigned int *hash);
/* <path> as a binary PPM of the high-resolution display; 0 when off. */
int gpu_hires_write_ppm(const char *path);

#endif
