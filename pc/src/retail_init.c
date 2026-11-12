/* SPDX-License-Identifier: GPL-3.0-or-later */
/* retail_init.c -- fill the generated globals' retail initial values from
 * the player's PSX.EXE at start-up (pc/include/pc_retail_init.h). */
#include "pc_retail_init.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int pc_retail_init_load(const char *exe_path) {
    FILE *f = fopen(exe_path, "rb");
    unsigned char hdr[0x800], *img;
    unsigned int load, size, i;
    int filled = 0, skipped = 0;
    if (!f) return -1;
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr || memcmp(hdr, "PS-X EXE", 8)) {
        fclose(f);
        return -1;
    }
    load = (unsigned)hdr[0x18] | (unsigned)hdr[0x19] << 8 | (unsigned)hdr[0x1A] << 16 | (unsigned)hdr[0x1B] << 24;
    size = (unsigned)hdr[0x1C] | (unsigned)hdr[0x1D] << 8 | (unsigned)hdr[0x1E] << 16 | (unsigned)hdr[0x1F] << 24;
    img = (unsigned char *)malloc(size ? size : 1);
    if (!img || fread(img, 1, size, f) != size) {
        free(img);
        fclose(f);
        return -1;
    }
    fclose(f);
    for (i = 0; i < pc_retail_init_count + pc_retail_init_manual_count; i++) {
        const PcRetailInit *e = i < pc_retail_init_count ? &pc_retail_init[i]
                                                         : &pc_retail_init_manual[i - pc_retail_init_count];
        if (!e->dst || e->addr < load || e->addr - load + e->size > size) { skipped++; continue; }
        memcpy(e->dst, img + (e->addr - load), e->size);
        filled++;
    }
    free(img);
    printf("[INIT] %d globals given their retail values from %s%s\n", filled, exe_path,
           skipped ? " (some outside the image)" : "");
    return filled;
}
