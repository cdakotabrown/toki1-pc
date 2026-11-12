/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Literal native VRAM sampling. Producer metadata must not alter commands. */
#include "gpu_texture.h"

#include <limits.h>

#if UCHAR_MAX != 255 || USHRT_MAX != 65535U || UINT_MAX != 4294967295U
#error GPU texture sampling requires native unsigned 8/16/32-bit types
#endif

unsigned short gpu_texture_sample(const unsigned short *vram,
                                  unsigned char u, unsigned char v,
                                  unsigned short clut,
                                  unsigned short tpage)
{
    unsigned int x;
    unsigned int y;
    unsigned int depth;
    unsigned int index;
    unsigned int palette_x;
    unsigned int palette_y;
    unsigned short word;

    if (!vram) {
        return 0;
    }
    x = (tpage & 15U) * 64U;
    y = ((tpage >> 4) & 1U) * 256U + v;
    depth = (tpage >> 7) & 3U;
    if (depth == 3U) {
        return 0;
    }
    if (depth == 0U) {
        x += u / 4U;
    } else if (depth == 1U) {
        x += u / 2U;
    } else {
        x += u;
    }
    x &= 1023U;
    word = vram[y * 1024U + x];
    if (depth == 2U) {
        return word;
    }
    if (depth == 0U) {
        index = (word >> ((u & 3U) * 4U)) & 15U;
    } else {
        index = (word >> ((u & 1U) * 8U)) & 255U;
    }
    palette_x = ((clut & 63U) * 16U + index) & 1023U;
    palette_y = (clut >> 6) & 511U;
    return vram[palette_y * 1024U + palette_x];
}
