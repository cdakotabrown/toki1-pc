/* SPDX-License-Identifier: GPL-3.0-or-later */
/* taiiku_sprite_init2 -- PC override of src/overlay/TAIIKU/taiiku_sprite_init2.c.
 *
 * Statement for statement the matched function; the divergence is the
 * RECORD WIDTH (the src/main/pc-port/basyo_init.c doctrine).  The matched
 * Rec declares the class_w sprite slot's tpage-set handle as `u8 *f52`:
 * 4 bytes on the console, 8 bytes (8-aligned) on LP64, where the record is
 * 80 bytes, so class_w[i + 96] strides 80 and f52/f56 move.  Here the
 * record is the shared PS1-width ClassSlot (pc/include/game/class_slot.h;
 * matched fN is ClassSlot fN) and the handle is the u32 PS1 address at
 * +52.  The cal_cw_* / cw97_* / D_80128Bxx scalars are class_w slot fields
 * already (layout_aliases.ld) and are unchanged.
 *
 * @DivergesFrom: src/overlay/TAIIKU/taiiku_sprite_init2.c -- record field
 * widths only.  Test: pc/tools/test_class_slot_width.py.
 */
#include "types.h"
#include "class_slot.h"
#include "tokimeki/class_w.h"

typedef struct {
    s16 v[9];
} Tbl9;

extern Tbl9 taiiku_sprite_init2_tbl0;
extern Tbl9 taiiku_sprite_init2_tbl1;
extern ClassSlot class_w[];
extern u8 taiiku_ninin_sprite_tpg[];

extern s32 taiiku_spr_img_5;
extern s32 taiiku_spr_anim_5;
extern s32 taiiku_spr_aux[];
extern u8 taiiku_ninin_girl1;

extern u8 cal_cw_prio;
extern u8 cal_cw_mode;
extern u8 cal_cw_disp;
extern s16 cal_cw_frame;
extern s16 cal_cw_x;
extern volatile s16 cal_cw_y;
extern u8 cw97_prio;
extern u8 cw97_mode;
extern u8 cw97_disp;
extern s16 cw97_x;
extern s16 cw98_x;
extern volatile s16 cw100_x;

extern void class_clear_n(s32);

void taiiku_sprite_init2(void)
{
    s32 i;
    Tbl9 a;
    Tbl9 b;

    a = taiiku_sprite_init2_tbl0;
    b = taiiku_sprite_init2_tbl1;
    for (i = 0; i < 9; i++) {
        class_clear_n(i + 96);
        class_w[i + 96].f56 = 0x01000000;
        class_w[i + 96].f7 = 128;
        class_w[i + 96].f12 = taiiku_spr_img_5;
        class_w[i + 96].f16 = taiiku_spr_anim_5;
        class_w[i + 96].f52 = CLASS_SLOT_HANDLE(taiiku_ninin_sprite_tpg);
        class_w[i + 96].f20 = (*(s16 *)&taiiku_spr_aux[5]);
        class_w[i + 96].f42 = a.v[i];
        class_w[i + 96].f24 = b.v[i];
        class_w[i + 96].f4 = 0;
        class_w[i + 96].f5 = 128;
        class_w[i + 96].f26 = 4096;
        class_w[i + 96].f28 = 4096;
        class_w[i + 96].f6 = 2;
    }

    if (taiiku_ninin_girl1 != 0) {
        cal_cw_frame = 14;
    }
    cal_cw_prio = 2;
    cal_cw_mode = 64;
    cal_cw_disp = 164;
    cw97_prio = 3;
    cw97_x = -152;
    cw97_mode = 64;
    cw97_disp = 164;
    ((CLASS_W *)class_w)[98].prio = 4;
    cw98_x = -144;
    ((CLASS_W *)class_w)[98].anim_flag = 64;
    ((CLASS_W *)class_w)[98].disp_flag = 164;
    ((CLASS_W *)class_w)[99].prio = 5;
    ((CLASS_W *)class_w)[99].x.half[1] = -136;
    ((CLASS_W *)class_w)[99].anim_flag = 64;
    ((CLASS_W *)class_w)[99].disp_flag = 164;
    ((CLASS_W *)class_w)[100].prio = 1;
    cw100_x = -160;
    ((CLASS_W *)class_w)[100].y.half[1] = cal_cw_y;
    ((CLASS_W *)class_w)[101].prio = 7;
    ((CLASS_W *)class_w)[102].prio = 5;
    ((CLASS_W *)class_w)[103].prio = 6;
    ((CLASS_W *)class_w)[104].prio = 7;
    ((CLASS_W *)class_w)[104].x.half[1] = -48;
    ((CLASS_W *)class_w)[104].anim_flag = 64;
    ((CLASS_W *)class_w)[104].disp_flag = 164;
    cal_cw_x = -160;
}
