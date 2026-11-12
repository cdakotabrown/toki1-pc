/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc/tests/test_nbn.c — tests for the NBN sprite atlas parser.
 *
 * Validates that we can locate TITLE.NBN and parse its TOC into the
 * expected sprite count + dimensions.
 */
#include "../include/game/types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TITLE_NBN "pc/assets/raw/CDROM/FUKUTEST/TITLE.NBN"
#define NBN_SECTOR 2048

static u8 *slurp(const char *path, u32 *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *buf = (u8 *)malloc((size_t)sz);
    fread(buf, 1, (size_t)sz, f);
    fclose(f);
    *out_size = (u32)sz;
    return buf;
}

int test_nbn_toc(void) {
    u32 sz;
    u8 *data = slurp(TITLE_NBN, &sz);
    if (!data) {
        printf("(missing %s) ", TITLE_NBN);
        return 1;
    }

    if (sz != 438272) {
        printf("(wrong size: %u, expected 438272) ", sz);
        free(data);
        return 2;
    }

    /* TOC at last sector. Each entry is 14 bytes ending in 0xCE. */
    u32 num_sectors = sz / NBN_SECTOR;
    u32 toc_off = (num_sectors - 1) * NBN_SECTOR;
    u32 entries = 0;
    for (u32 i = 0; i < NBN_SECTOR; i += 14) {
        if (data[toc_off + i + 13] != 0xCE) break;
        entries++;
    }
    if (entries != 14) {
        printf("(wrong TOC count: %u, expected 14) ", entries);
        free(data);
        return 3;
    }

    /* Spot-check sprite 4 (the LOGO atlas) — id=1397, 128x64. */
    u32 sprite4 = toc_off + 4 * 14;
    u32 px_w = data[sprite4 + 2] * 8;
    u32 height = (u32)data[sprite4 + 4] | ((u32)data[sprite4 + 5] << 8);
    u32 sprite_id = (u32)data[sprite4 + 10] | ((u32)data[sprite4 + 11] << 8);
    if (sprite_id != 1397 || px_w != 128 || height != 64) {
        printf("(sprite4 bad: id=%u %ux%u) ", sprite_id, px_w, height);
        free(data);
        return 4;
    }

    free(data);
    return 0;
}
