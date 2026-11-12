/* SPDX-License-Identifier: GPL-3.0-or-later */
/* taiiku_sprite_init -- PC override of src/overlay/TAIIKU/taiiku_sprite_init.c.
 *
 * Statement for statement the matched function; the divergence is the
 * RECORD WIDTH (the src/main/pc-port/basyo_init.c doctrine).  The matched
 * Rec68 declares the class_w sprite slot's tpage-set handle as `s32 *f52`:
 * 4 bytes on the console, 8 bytes (8-aligned) on LP64, where the record is
 * 80 bytes, so class_w[96 + i] / [107 + i] / [111 + i] / [112] stride 80
 * and f52/f56/f67 move.  Here the record is the shared PS1-width ClassSlot
 * (pc/include/game/class_slot.h; matched f0N is ClassSlot fN, fNN is fNN)
 * and the handle is the u32 PS1 address at +52.
 *
 * @DivergesFrom: src/overlay/TAIIKU/taiiku_sprite_init.c -- record field
 * widths only.  Test: pc/tools/test_class_slot_width.py.
 */
#include "types.h"
#include "class_slot.h"

typedef struct { u16 h[5]; } Blk10;
typedef struct { s32 w[5]; } Blk20;

extern ClassSlot class_w[];
extern Blk10 taiiku_sprite_init_tbl0;
extern Blk20 taiiku_sprite_init_tbl1;
extern Blk20 taiiku_sprite_init_tbl2;
extern Blk20 taiiku_sprite_init_tbl3;
extern s32 taiiku_spr_img_2;
extern s32 taiiku_spr_img_3;
extern s32 taiiku_spr_anim_2;
extern s32 taiiku_spr_anim_3;
extern s32 taiiku_spr_aux[];
extern s32 taiiku_kari_runner_tpg[];
extern s32 taiiku_kari_sprite_tpg[];

extern get_taiiku_rank_frame();
extern get_taiiku_rank_x();
extern class_clear_n();

taiiku_sprite_init()
{
    s32 i;
    s32 k;
    Blk10 b1;
    Blk20 b2;
    Blk20 b3;
    Blk20 b4;
    u8  *p1;
    s32 y;

    b1 = taiiku_sprite_init_tbl0;
    b2 = taiiku_sprite_init_tbl1;
    b3 = taiiku_sprite_init_tbl2;
    b4 = taiiku_sprite_init_tbl3;

    for (i = 0, y = 40, k = 2; i < 4; y -= 24, i++, k += 2) {
        class_w[96 + i].f56 = 0x01000000;
        class_w[96 + i].f1 = (u8)k;
        class_w[96 + i].f4 = 0;
        class_w[96 + i].f5 = 128;
        class_w[96 + i].f2 = 64;
        class_w[96 + i].f3 = 164;
        class_w[96 + i].f6 = 2;
        class_w[96 + i].f8 = 0;
        class_w[96 + i].f12 = taiiku_spr_img_3;
        class_w[96 + i].f16 = taiiku_spr_anim_3;
        class_w[96 + i].f52 = CLASS_SLOT_HANDLE(taiiku_kari_sprite_tpg);
        class_w[96 + i].f20 = (*(s16 *)&taiiku_spr_aux[3]);
        class_w[96 + i].f22 = 0;
        class_w[96 + i].f24 = (s16)get_taiiku_rank_frame(i, 1);
        class_w[96 + i].f26 = 4096;
        class_w[96 + i].f28 = 4096;
        class_w[96 + i].f7 = 128;
        class_w[96 + i].f38 = (s16)get_taiiku_rank_x(i);
        class_w[96 + i].f42 = (s16)y;
    }

    for (i = 0, y = 40, k = 3; i < 4; y -= 24, i++, k += 2) {
        class_w[107 + i].f56 = 0x01000000;
        class_w[107 + i].f1 = (u8)k;
        class_w[107 + i].f4 = 0;
        class_w[107 + i].f5 = 128;
        class_w[107 + i].f2 = 64;
        class_w[107 + i].f3 = 164;
        class_w[107 + i].f6 = 2;
        class_w[107 + i].f8 = 0;
        class_w[107 + i].f12 = taiiku_spr_img_3;
        class_w[107 + i].f16 = taiiku_spr_anim_3;
        class_w[107 + i].f52 = CLASS_SLOT_HANDLE(taiiku_kari_sprite_tpg);
        class_w[107 + i].f20 = (*(s16 *)&taiiku_spr_aux[3]);
        class_w[107 + i].f22 = 0;
        class_w[107 + i].f24 = (s16)get_taiiku_rank_frame(i, 0);
        class_w[107 + i].f26 = 4096;
        class_w[107 + i].f28 = 4096;
        class_w[107 + i].f7 = 128;
        class_w[107 + i].f38 = (s16)get_taiiku_rank_x(i);
        class_w[107 + i].f42 = (s16)y;
    }

    for (i = 0, p1 = (u8 *)&b1; i != 10; p1++, i++) {
        class_clear_n(i + 111);
        class_w[111 + i].f1 = *p1;
        class_w[111 + i].f56 = 0x01000000;
        class_w[111 + i].f4 = 0;
        class_w[111 + i].f5 = 128;
        class_w[111 + i].f2 = 1;
        class_w[111 + i].f3 = 164;
        class_w[111 + i].f6 = 2;
        class_w[111 + i].f8 = 0;
        class_w[111 + i].f12 = taiiku_spr_img_3;
        class_w[111 + i].f16 = taiiku_spr_anim_3;
        class_w[111 + i].f52 = CLASS_SLOT_HANDLE(taiiku_kari_sprite_tpg);
        class_w[111 + i].f20 = (*(s16 *)&taiiku_spr_aux[3]);
        class_w[111 + i].f22 = ((s16 *)&b2)[i];
        class_w[111 + i].f38 = ((s16 *)&b3)[i];
        class_w[111 + i].f42 = ((s16 *)&b4)[i];
        class_w[111 + i].f24 = 0;
        class_w[111 + i].f26 = 4096;
        class_w[111 + i].f28 = 4096;
        class_w[111 + i].f67 = 0;
        class_w[111 + i].f7 = 128;
    }

    class_clear_n(112);
    class_w[112].f56 = 0x01000000;
    class_w[112].f1 = 1;
    class_w[112].f4 = 0;
    class_w[112].f5 = 128;
    class_w[112].f2 = 64;
    class_w[112].f3 = 164;
    class_w[112].f6 = 2;
    class_w[112].f8 = 0;
    class_w[112].f12 = taiiku_spr_img_2;
    class_w[112].f16 = taiiku_spr_anim_2;
    class_w[112].f52 = CLASS_SLOT_HANDLE(taiiku_kari_runner_tpg);
    class_w[112].f20 = (*(s16 *)&taiiku_spr_aux[2]);
    class_w[112].f22 = 0;
    class_w[112].f24 = 160;
    class_w[112].f26 = 8192;
    class_w[112].f28 = 8192;
    class_w[112].f67 = 0;
    class_w[112].f7 = 128;
    class_w[112].f38 = -128;
    class_w[112].f42 = -248;
}
