/* SPDX-License-Identifier: GPL-3.0-or-later */
/* taiiku_plate_init -- PC override of src/overlay/TAIIKU/taiiku_plate_init.c.
 *
 * Statement for statement the matched function; the divergence is the
 * RECORD WIDTH (the src/main/pc-port/basyo_init.c doctrine).  The matched
 * Actor declares the class_w sprite slot's owner handle as `s32 *w52`:
 * 4 bytes on the console, 8 bytes (8-aligned) on LP64, where the record is
 * 80 bytes, so class_w[96 + i] / [100 + i] stride 80 and w52/w56/b67
 * move.  Here the record is the shared PS1-width ClassSlot
 * (pc/include/game/class_slot.h; matched bN/hN/wN is ClassSlot fN, x.w.hi
 * and y.w.hi are f38/f42) and the handle is the u32 PS1 address at +52.
 *
 * @DivergesFrom: src/overlay/TAIIKU/taiiku_plate_init.c -- record field
 * widths only.  Test: pc/tools/test_class_slot_width.py.
 */
#include "types.h"
#include "class_slot.h"

extern ClassSlot class_w[];
extern s32 taiiku_spr_img_13;
extern s32 taiiku_spr_anim_13;
extern s32 taiiku_spr_aux[];
extern s32 taiiku_tama_plate_tpg;
extern u8 taiiku_tama_rank[];
extern s32 taiiku_tama_girl;

void game_reset();

void taiiku_plate_init()
{
    s32 i;

    for (i = 0; i < 8; i++) {
        class_w[96 + i].f3 = 164;
        class_w[96 + i].f2 = 64;
        class_w[96 + i].f5 = 128;
        class_w[96 + i].f1 = 3;
        class_w[96 + i].f56 = 0x01000000;
        class_w[96 + i].f7 = 128;
        class_w[96 + i].f12 = taiiku_spr_img_13;
        class_w[96 + i].f16 = taiiku_spr_anim_13;
        class_w[96 + i].f52 = CLASS_SLOT_HANDLE(&taiiku_tama_plate_tpg);
        class_w[96 + i].f20 = (*(s16 *)&taiiku_spr_aux[13]);
        class_w[96 + i].f26 = 4096;
        class_w[96 + i].f28 = 4096;
        class_w[96 + i].f6 = 2;
    }
    for (i = 0; i < 4; i++) {
        class_w[96 + i].f38 = i * 64 - 120;
        class_w[96 + i].f42 = -8;
        switch ((s32)taiiku_tama_rank[i]) {
        case 0:
            class_w[96 + i].f24 = taiiku_tama_girl * 7;
            break;
        case 1:
            class_w[96 + i].f24 = 77;
            class_w[96 + i].f67 = 28;
            break;
        case 2:
            class_w[96 + i].f24 = 77;
            class_w[96 + i].f67 = 29;
            break;
        case 3:
            class_w[96 + i].f24 = 77;
            break;
        default:
            game_reset();
            break;
        }
    }
    for (i = 0; i < 4; i++) {
        class_w[100 + i].f38 = i * 64 - 120;
        class_w[100 + i].f42 = -8;
        class_w[100 + i].f24 = 88;
        switch ((s32)taiiku_tama_rank[i]) {
        case 0:
            break;
        case 1:
            class_w[100 + i].f67 = 28;
            break;
        case 2:
            class_w[100 + i].f67 = 29;
            break;
        case 3:
            class_w[100 + i].f67 = 30;
            break;
        }
    }
}
