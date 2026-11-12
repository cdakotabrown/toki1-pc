/* SPDX-License-Identifier: GPL-3.0-or-later */
/* date_spring_init -- PC override of src/overlay/DATE/date_spring_init.c.
 *
 * Statement for statement the matched function; the divergence is the
 * RECORD WIDTH (the src/main/pc-port/basyo_init.c doctrine).  The matched
 * DateObj declares the class_w sprite slot's handles as `s32 *unk0C,
 * *unk10, *unk34`: 4 bytes on the console, 8 bytes (8-aligned) on LP64,
 * where the record is 80 bytes, so class_w[i + 98] strides 80 and every
 * field after +12 moves (unk24, the point mirrored into date_spring_pos, is
 * read back from the host offset +28).  Here the record is the shared
 * PS1-width ClassSlot (pc/include/game/class_slot.h; matched unkNN is
 * ClassSlot f<NN decimal>, unk24's halves are f36/f38) and the handles are
 * u32 PS1 addresses at +12/+16/+52.  The validated pointer cells
 * date_spring_res..date_spring_res_pal1 are 32-bit PS1 words (as the module's generator
 * already reads them) and are copied into the slot as such.
 *
 * @DivergesFrom: src/overlay/DATE/date_spring_init.c -- record field
 * widths only.  Test: pc/tools/test_class_slot_width.py.
 */
#include "types.h"
#include "class_slot.h"

typedef union {
    s32 word;
    struct {
        s16 x;
        s16 y;
    } h;
} DatePoint;

extern u32        date_spring_res;
extern u32        date_spring_res_hdr1;
extern u32        date_spring_res_img0;
extern u32        date_spring_res_img1;
extern u32        date_spring_res_anim_tbl0;
extern u32        date_spring_res_anim_tbl1;
extern u32        date_spring_res_pal0;
extern u32        date_spring_res_pal1;
extern DatePoint  date_spring_pos[];
extern u8         date_pattern;
extern u8         now_sub_sub_step;
extern ClassSlot  class_w[];

extern s32  sub_sub_step_up(void);
extern void palette_upload_enqueue(s32 *a0, s32 a1, s32 a2, u8 a3, s32 a4);
extern void class_clear_n(s32 a0);
extern s32  rand(void);

/* the PS1 word at handle h, index i */
#define DATE_CELL(h, i) (((s32 *)(uintptr_t)(h))[i])

s32 date_spring_init()
{
    short i;
    short spd;
    s32 rndA;
    s32 rndB;
    u8 k; /* the held constant 2 */

    if (DATE_CELL(date_spring_res, 0) != 0x801E4000 ||
        DATE_CELL(date_spring_res_hdr1, 0) != 0x801E8000 ||
        DATE_CELL(date_spring_res_hdr1, 1) != 0x801EC000 ||
        DATE_CELL(date_spring_res_anim_tbl0, 0) != 0x801F0540 ||
        DATE_CELL(date_spring_res_anim_tbl0, 1) != 0x801F0558 ||
        DATE_CELL(date_spring_res_anim_tbl0, 2) != 0x801F0570 ||
        DATE_CELL(date_spring_res_anim_tbl0, 3) != 0x801F058C ||
        DATE_CELL(date_spring_res_anim_tbl1, 0) != 0x801F0B64 ||
        DATE_CELL(date_spring_res_anim_tbl1, 1) != 0x801F0B80) {
        now_sub_sub_step = now_sub_sub_step - 2;
        return 0;
    }
    k = 2;
    if (date_pattern == k) {
        palette_upload_enqueue((s32 *)(uintptr_t)date_spring_res_pal0, 18, 1, k, 0);
    } else {
        palette_upload_enqueue((s32 *)(uintptr_t)date_spring_res_pal1, 18, 1, k, 0);
    }

    for (i = 0; i < 20; i++) {
        class_clear_n(i + 98);
        class_w[i + 98].f56 = 0x01000000;
        class_w[i + 98].f1 = 13;
        class_w[i + 98].f2 = 1;
        class_w[i + 98].f3 = 4;
        if (date_pattern == k) {
            class_w[i + 98].f12 = date_spring_res_img0;
            class_w[i + 98].f16 = date_spring_res_anim_tbl0;
            class_w[i + 98].f52 = date_spring_res;
        } else {
            class_w[i + 98].f12 = date_spring_res_img1;
            class_w[i + 98].f16 = date_spring_res_anim_tbl1;
            class_w[i + 98].f52 = date_spring_res_hdr1;
        }
        class_w[i + 98].f22 = rand() % 8;
        class_w[i + 98].f67 = 18;
        spd = rand() % 2;
        class_w[i + 98].f26 = 4096 / (spd * 2 + 2);
        class_w[i + 98].f28 = 4096 / (spd * 2 + 2);
        class_w[i + 98].f7 = 128;
        class_w[i + 98].f38 = rand() % 300 - 150;
        date_spring_pos[i].word = *(s32 *)&class_w[i + 98].f36;
        rndA = rand();
        rndB = rand();
        date_spring_pos[i].h.y += ((rndB % 2 * 2 - 1) * (rndA % 8) + 8) / (spd + 1);
        class_w[i + 98].f42 = rand() % 160 - 120;
        class_w[i + 98].f48 =
            (((date_pattern != k) << 15) + 32768) / (spd + 1);
    }

    return sub_sub_step_up();
}
