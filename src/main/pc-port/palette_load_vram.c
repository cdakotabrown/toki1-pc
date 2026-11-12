/* palette_load_vram: retain the retail 8-byte CLUT record on x64.
 * @DivergesFrom: src/main/matched/palette_load_vram.c -- only the stored
 * pointer representation changes: a 32-bit address is widened at use.
 * Palette modes, loop bounds, flags and load_image_rect calls are identical.
 * PS1 screen_w+4904 holds 32 eight-byte records; count is at +5160.
 */
#include "types.h"

typedef struct {
    u32 clut_addr;
    u8 pal;
    u8 pad;
    u8 mode;
    u8 row;
} ClutEnt;

typedef struct {
    u8 pad[4904];
    ClutEnt ent[32];
    u8 count;
} Save;

extern u8 palette_queue_n;
extern Save screen_w;

extern void load_image_rect(u16, u16, u16, s32, u32 *);

#define CLUT(i) ((u32 *)(unsigned long)sv->ent[i].clut_addr)

void palette_load_vram(s32 arg0)
{
    Save *sv;
    u16 x;
    u16 y;
    u16 w;
    s32 i;
    s32 j;
    s32 start;
    s32 unused[2];

    sv = &screen_w;
    i = 0;
    if (palette_queue_n != 0) {
        do {
            y = (sv->ent[i].pal & 0x1F) + 480;
            start = sv->ent[i].row << 7;
            if (sv->ent[i].pal < 32) {
                w = 256;
                x = 0;
                switch (sv->ent[i].mode) {
                case 0:
                    for (j = start; j < start + 128; j++) {
                        if (CLUT(i)[j] & 0x80000000) {
                            CLUT(i)[j] &= 0x7FFF7FFF;
                        }
                        if (CLUT(i)[j] & 0x8000) {
                            CLUT(i)[j] &= 0x7FFF7FFF;
                        }
                    }
                    break;
                case 1:
                default:
                    break;
                case 2:
                    for (j = start; j < start + 120; j++) {
                        if (CLUT(i)[j] & 0x80000000) {
                            CLUT(i)[j] &= 0x7FFF7FFF;
                        }
                        if (CLUT(i)[j] & 0x8000) {
                            CLUT(i)[j] &= 0x7FFF7FFF;
                        }
                    }
                    for (j = start + 120; j < start + 128; j++) {
                        if (CLUT(i)[j] & 0x7FFF0000) {
                            CLUT(i)[j] |= 0x80000000;
                        }
                        if (CLUT(i)[j] & 0x7FFF) {
                            CLUT(i)[j] |= 0x8000;
                        }
                    }
                    break;
                case 3:
                    for (j = start; j < start + 128; j++) {
                        if (CLUT(i)[j] & 0x7FFF0000) {
                            CLUT(i)[j] |= 0x80000000;
                        }
                        if (CLUT(i)[j] & 0x7FFF) {
                            CLUT(i)[j] |= 0x8000;
                        }
                    }
                    break;
                case 4:
                    for (j = start; j < start + 112; j++) {
                        if (CLUT(i)[j] & 0x80000000) {
                            CLUT(i)[j] &= 0x7FFF7FFF;
                        }
                        if (CLUT(i)[j] & 0x8000) {
                            CLUT(i)[j] &= 0x7FFF7FFF;
                        }
                    }
                    for (j = start + 112; j < start + 128; j++) {
                        if (CLUT(i)[j] & 0x7FFF0000) {
                            CLUT(i)[j] |= 0x80000000;
                        }
                        if (CLUT(i)[j] & 0x7FFF) {
                            CLUT(i)[j] |= 0x8000;
                        }
                    }
                    break;
                }
            } else {
                w = 32;
                if (sv->ent[i].pal < 64) {
                    x = 256;
                } else {
                    x = 288;
                }
            }
            load_image_rect(x, y, w, 1, &CLUT(i)[start]);
            i++;
        } while (i < sv->count);
    }
    palette_queue_n = 0;
}
