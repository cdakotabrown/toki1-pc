/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Extracted from gpu_display's normal 320x240 packing path.
 * No change to the rasterizer or the SDL presentation/debug paths.
 */
#include "gpu_scanout.h"

#include <limits.h>

#if USHRT_MAX != 65535U || UINT_MAX != 4294967295U
#error GPU scanout requires 16-bit unsigned short and 32-bit unsigned int
#endif

static unsigned int scanout_bgr555(unsigned short pixel)
{
    unsigned int r;
    unsigned int g;
    unsigned int b;

    r = (pixel & 0x001FU) << 3;
    g = ((pixel >> 5) & 0x1FU) << 3;
    b = ((pixel >> 10) & 0x1FU) << 3;
    return (r << 24) | (g << 16) | (b << 8) | 0xFFU;
}

int gpu_scanout_pack(const GpuScanoutState *state,
                     const unsigned short *vram, unsigned int *pixels,
                     size_t capacity, int *width, int *height)
{
    int i;
    int j;
    int vx;
    int vy;
    int byte_x;
    int channel;
    int pos;
    unsigned int color;
    unsigned int byte;

    if (!state || !vram || !pixels || !width || !height ||
        capacity < GPU_SCANOUT_PIXELS) {
        return 0;
    }
    for (j = 0; j < GPU_SCANOUT_HEIGHT; j++) {
        for (i = 0; i < GPU_SCANOUT_WIDTH; i++) {
            color = 0x000000FFU;
            /* Test before addition, including pathological signed origins.
             * The original loop's defined, in-range results are unchanged. */
            if (state->enabled && i < state->width && j < state->height &&
                state->x >= -i &&
                state->x < GPU_SCANOUT_VRAM_WIDTH - i &&
                state->y >= -j &&
                state->y < GPU_SCANOUT_VRAM_HEIGHT - j) {
                vx = state->x + i;
                vy = state->y + j;
                if (state->rgb24) {
                    byte_x = state->x * 2 + i * 3;
                    if (byte_x + 2 < GPU_SCANOUT_VRAM_WIDTH * 2) {
                        for (channel = 0; channel < 3; channel++) {
                            pos = byte_x + channel;
                            byte = (vram[vy * GPU_SCANOUT_VRAM_WIDTH +
                                         pos / 2] >> ((pos & 1) * 8)) & 0xFFU;
                            color |= byte << (24 - channel * 8);
                        }
                    }
                } else {
                    color = scanout_bgr555(
                        vram[vy * GPU_SCANOUT_VRAM_WIDTH + vx]);
                }
            }
            pixels[j * GPU_SCANOUT_WIDTH + i] = color;
        }
    }
    *width = GPU_SCANOUT_WIDTH;
    *height = GPU_SCANOUT_HEIGHT;
    return 1;
}
