/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tim.c — TIM texture format loader
 * Parses PS1 TIM files and loads them into VRAM via LoadImage.
 */
#include "hal.h"
#include "pc_data_root.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* TIM format: magic(4) + flags(4) + [clut_block] + pixel_block
 * Each block: length(4) + x(2) + y(2) + w(2) + h(2) + data
 */

s32 tim_load_file(const char *path) {
    FILE *f;
    u8 *data;
    s32 size;
    u32 magic, flags;
    s32 bpp, has_clut;
    s32 off;
    RECT rect;

    f = fopen(path, "rb");
    if (!f) {
        printf("[TIM] Cannot open: %s\n", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);

    data = (u8 *)malloc(size);
    if (!data) { fclose(f); return -1; }
    fread(data, 1, size, f);
    fclose(f);

    magic = *(u32 *)&data[0];
    flags = *(u32 *)&data[4];

    if (magic != 0x00000010) {
        printf("[TIM] Bad magic: 0x%08X in %s\n", magic, path);
        free(data);
        return -1;
    }

    bpp = flags & 0x7;
    has_clut = (flags >> 3) & 1;
    off = 8;

    /* Load CLUT if present — place at VRAM (0,481) and also copy to (160,481)
     * so both CLUT ID 0x7840 and 0x784A reference the same white/gray palette.
     *
     * IMPORTANT: TIM files may store 256-color CLUTs as 16x16 blocks, but PS1
     * clut_lookup expects them as 256x1 rows. Reformat to 256x1 if needed. */
    if (has_clut) {
        u32 clut_len = *(u32 *)&data[off];
        s16 orig_x = *(s16 *)&data[off + 4];
        s16 orig_y = *(s16 *)&data[off + 6];
        s16 tim_w = *(s16 *)&data[off + 8];
        s16 tim_h = *(s16 *)&data[off + 10];
        u16 *clut_data = (u16 *)&data[off + 12];

        printf("[TIM] CLUT: orig=(%d,%d) size=%dx%d\n", orig_x, orig_y, tim_w, tim_h);

        /* If CLUT is 16x16, reformat to 256x1 for PS1 clut_lookup compatibility */
        if (tim_w == 16 && tim_h == 16) {
            u16 linear_clut[256];
            s32 i;
            for (i = 0; i < 256; i++) {
                /* TIM 16x16: row = i/16, col = i%16 */
                linear_clut[i] = clut_data[(i / 16) * 16 + (i % 16)];
            }
            /* Load as 256x1 at (0, 481) for CLUT ID 0x7840 */
            rect.x = 0; rect.y = 481; rect.w = 256; rect.h = 1;
            printf("[TIM] CLUT: reformatted 16x16 -> 256x1 at (%d,%d)\n", rect.x, rect.y);
            LoadImage(&rect, (u32 *)linear_clut);

            /* Also copy to (160, 481) for CLUT ID 0x784A */
            rect.x = 160;
            printf("[TIM] CLUT: also at (%d,%d) for menu sprites\n", rect.x, rect.y);
            LoadImage(&rect, (u32 *)linear_clut);
        } else {
            /* Load as-is for 16x1 or other formats */
            rect.x = 0; rect.y = 481; rect.w = tim_w; rect.h = tim_h;
            printf("[TIM] CLUT: placed at (%d,%d) %dx%d\n", rect.x, rect.y, rect.w, rect.h);
            LoadImage(&rect, (u32 *)clut_data);

            rect.x = 160;
            printf("[TIM] CLUT: also copied to (%d,%d)\n", rect.x, rect.y);
            LoadImage(&rect, (u32 *)clut_data);
        }

        off += clut_len;
    }

    /* Load pixel data — place at VRAM (640,0) to avoid display area overlap */
    {
        u32 pix_len = *(u32 *)&data[off];
        s16 orig_x = *(s16 *)&data[off + 4];
        s16 orig_y = *(s16 *)&data[off + 6];
        rect.w = *(s16 *)&data[off + 8];
        rect.h = *(s16 *)&data[off + 10];
        /* Override position: place font texture at x=960 (texture page 15)
         * Display buffers are at X=640-959, so fonts go to X=960+ */
        rect.x = 960;
        rect.y = 0;

        printf("[TIM] Pixels: orig=(%d,%d) -> placed at (%d,%d) %dx%d (bpp=%d) from %s\n",
               orig_x, orig_y, rect.x, rect.y, rect.w, rect.h, bpp, path);
        LoadImage(&rect, (u32 *)&data[off + 12]);
    }

    free(data);
    return 0;
}

/* Load a TIM from the assets directory */
s32 tim_load_asset(const char *asset_name) {
    char path[512];
    /* Try multiple locations */
    snprintf(path, sizeof(path), "%s/textures/%s", pc_asset_dir(), asset_name);
    if (tim_load_file(path) == 0) return 0;
    snprintf(path, sizeof(path), "%s/raw/CDROM/%s", pc_asset_dir(), asset_name);
    if (tim_load_file(path) == 0) return 0;
    return -1;
}
