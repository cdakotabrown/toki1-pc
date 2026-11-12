/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Independent implementation from the contracts cited in gpu_vram.h. */
#include "gpu_vram.h"

void gpu_vram_fill(unsigned short *vram, unsigned int color,
                   unsigned int position, unsigned int size)
{
    unsigned int x, y, width, height, row, column;
    unsigned short pixel;
    size_t offset;

    x = position & 0x3F0U;
    y = (position >> 16) & 511U;
    width = ((size & 1023U) + 15U) & ~15U;
    height = (size >> 16) & 511U;
    pixel = (unsigned short)(((color >> 3) & 31U) |
                             ((color >> 6) & 0x3E0U) |
                             ((color >> 9) & 0x7C00U));
    for (row = 0; row < height; row++) {
        offset = (size_t)((y + row) & 511U) * GPU_VRAM_WIDTH;
        for (column = 0; column < width; column++) {
            vram[offset + ((x + column) & 1023U)] = pixel;
        }
    }
}

size_t gpu_vram_image_pixels(int width, int height,
                             unsigned int *copy_width,
                             unsigned int *copy_height)
{
    if (width < 0) {
        width = 0;
    } else if (width > GPU_VRAM_WIDTH) {
        width = GPU_VRAM_WIDTH;
    }
    if (height < 0) {
        height = 0;
    } else if (height > GPU_VRAM_HEIGHT) {
        height = GPU_VRAM_HEIGHT;
    }
    *copy_width = (unsigned int)width;
    *copy_height = (unsigned int)height;
    return (size_t)width * (size_t)height;
}

void gpu_vram_load(unsigned short *vram, int x, int y, int width, int height,
                   const unsigned short *pixels, int mask_set,
                   int mask_check)
{
    unsigned int start_x, start_y, copy_width, copy_height;
    unsigned int row, column;
    unsigned short set_bits, check_bits;
    size_t offset;

    start_x = (unsigned int)x & 1023U;
    start_y = (unsigned int)y & 511U;
    copy_width = (((unsigned int)width - 1U) & 1023U) + 1U;
    copy_height = (((unsigned int)height - 1U) & 511U) + 1U;
    set_bits = mask_set ? 0x8000U : 0;
    check_bits = mask_check ? 0x8000U : 0;
    for (row = 0; row < copy_height; row++) {
        offset = (size_t)((start_y + row) & 511U) * GPU_VRAM_WIDTH;
        for (column = 0; column < copy_width; column++) {
            if (!(vram[offset + ((start_x + column) & 1023U)] & check_bits)) {
                vram[offset + ((start_x + column) & 1023U)] =
                    (unsigned short)(*pixels | set_bits);
            }
            pixels++;
        }
    }
}

void gpu_vram_store(const unsigned short *vram, int x, int y,
                    int width, int height, unsigned short *pixels)
{
    unsigned int start_x, start_y, copy_width, copy_height;
    unsigned int row, column;
    size_t offset;

    start_x = (unsigned int)x & 1023U;
    start_y = (unsigned int)y & 511U;
    copy_width = (((unsigned int)width - 1U) & 1023U) + 1U;
    copy_height = (((unsigned int)height - 1U) & 511U) + 1U;
    for (row = 0; row < copy_height; row++) {
        offset = (size_t)((start_y + row) & 511U) * GPU_VRAM_WIDTH;
        for (column = 0; column < copy_width; column++) {
            *pixels++ = vram[offset + ((start_x + column) & 1023U)];
        }
    }
}
