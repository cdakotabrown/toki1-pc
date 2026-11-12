/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Pure native display packing; no SDL, allocation or device side effects. */
#ifndef GPU_SCANOUT_H
#define GPU_SCANOUT_H

#include <stddef.h>

#define GPU_SCANOUT_WIDTH 320
#define GPU_SCANOUT_HEIGHT 240
#define GPU_SCANOUT_PIXELS (GPU_SCANOUT_WIDTH * GPU_SCANOUT_HEIGHT)
#define GPU_SCANOUT_VRAM_WIDTH 1024
#define GPU_SCANOUT_VRAM_HEIGHT 512

typedef struct GpuScanoutState {
    int x;       /* VRAM halfword origin, including in RGB24 mode. */
    int y;       /* VRAM row origin. */
    int width;   /* Display rectangle width in output pixels. */
    int height;  /* Display rectangle height in rows. */
    int rgb24;   /* DISPENV isrgb24; zero selects BGR555. */
    int enabled; /* SetDispMask convention: zero blanks the display. */
} GpuScanoutState;

/* VRAM is 512 rows of 1024 unsigned 16-bit words, in row-major order.
 * Pixels are numeric 0xRRGGBBAA unsigned 32-bit words, NOT byte arrays.
 * Pass &g_vram[0][0]; capacity is in pixels, not bytes. Buffers must not
 * overlap each other, state, or the dimension outputs.
 *
 * Returns 1 and writes exactly GPU_SCANOUT_PIXELS pixels plus dimensions
 * 320x240. Nonzero enabled/rgb24 values mean enabled/packed RGB24.
 * Disabled output and all padding are opaque black. Bit 15 of BGR555 is
 * ignored, and each 5-bit channel is shifted by 3 (31 becomes 248).
 *
 * This preserves gpu_display's fixed viewport, NOT general PS1 scanout:
 * larger rectangles are cropped, zero/negative extents produce black;
 * out-of-VRAM coordinates and RGB24 triples crossing a row are black.
 * Negative origins clip rather than wrap. Interlace, display timing/ranges,
 * alternate output sizes and hardware row wrapping are not modeled.
 * Row-edge clipping is a legacy safety policy, not hardware evidence.
 *
 * Returns 0 on NULL arguments or insufficient capacity, with NO writes.
 * Inputs are never mutated. There is no retained state or allocation.
 */
int gpu_scanout_pack(const GpuScanoutState *state,
                     const unsigned short *vram, unsigned int *pixels,
                     size_t capacity, int *width, int *height);

#endif /* GPU_SCANOUT_H */
