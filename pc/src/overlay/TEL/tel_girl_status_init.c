/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tel_girl_status_init -- PC override of src/overlay/TEL/tel_girl_status_init.c.
 *
 * Statement for statement the matched function; the divergence is the
 * RECORD WIDTH (the src/main/pc-port/basyo_init.c doctrine).  The matched
 * Rec declares the class_w sprite slot's handles as `s32 *p12, *p16,
 * *p52`: 4 bytes on the console, 8 bytes (8-aligned) on LP64, where the
 * record is 88 bytes, so class_w[i + 96] / class_w[idx] stride 88 and
 * every field after +12 moves.  Here the record is the shared PS1-width
 * ClassSlot (pc/include/game/class_slot.h; matched fN is ClassSlot fN,
 * pN is fN, x/y are f38/f42) and the handles are u32 PS1 addresses at
 * +12/+16/+52.
 *
 * @DivergesFrom: src/overlay/TEL/tel_girl_status_init.c -- record field
 * widths only.  Test: pc/tools/test_class_slot_width.py.
 */
#include "types.h"
#include "class_slot.h"

extern s32 set_kanji_string(s16, s16, u8, u8 *, s32);
extern void class_clear_n(s32);

typedef struct {
    unsigned b0 : 1;
    unsigned met : 1;
    unsigned rest : 30;
} Flags;

typedef struct {
    u8  pad0[6];
    s16 days;
    Flags flags;
    u8  pad12[44];
} Chr;

typedef struct {
    u8  pad0[448];
    Chr chr[11];
    u8  pad1064[240];
    u8  level[11];
} Save;

extern ClassSlot class_w[];
extern Save screen_w;
extern s16 cal_cw_frame;
extern s16 cal_cw_x;
extern s16 cal_cw_y;
extern s16 basyo_init_tbl3;
extern s32 _sys_default_tpage_set_tbl2;
extern s32 basyo_init_tbl2;
extern s32 basyo_init_tbl1;
extern s32 work_fuku[];
extern u8  tel_girl_status_init_str0[];
extern u8  tel_girl_status_init_str1[];
extern s32 girl_full_name_tbl[];

void tel_girl_status_init()
{
    s32 i;
    s32 idx;
    s32 level;
    s32 xbase;
    s32 col;
    s32 ybase;
    u8 score[16];
    s16 rank[16];
    s32 days;

    for (i = 0; i < 34; i++) {
        class_clear_n(i + 96);
        class_w[i + 96].f1 = 4;
        class_w[i + 96].f2 = 64;
        class_w[i + 96].f56 = 0x40000000;
        class_w[i + 96].f3 = 132;
        class_w[i + 96].f12 = CLASS_SLOT_HANDLE(&basyo_init_tbl1);
        class_w[i + 96].f16 = CLASS_SLOT_HANDLE(&basyo_init_tbl2);
        class_w[i + 96].f52 = CLASS_SLOT_HANDLE(&_sys_default_tpage_set_tbl2);
        class_w[i + 96].f20 = basyo_init_tbl3;
        class_w[i + 96].f4 = 8;
        class_w[i + 96].f5 = 1;
        class_w[i + 96].f38 = 512;
    }
    cal_cw_frame = 66;
    cal_cw_x = -16;
    cal_cw_y = -27;
    work_fuku[1] = set_kanji_string(-128, -98, 4, tel_girl_status_init_str0, 0);
    idx = 97;
    get_grp(score, rank);
    for (i = 0; i < 11; i++) {
        screen_w.level[i] = 0;
        if (screen_w.chr[i].flags.met) {
            xbase = i / 6 * 130;
            col = i % 6 * 16;
            set_kanji_string(xbase - 130, col - 72, 3, girl_full_name_tbl[i], 0);
            ybase = col - 68;
            class_w[idx].f24 = 97; class_w[idx].f38 = xbase - 50; class_w[idx].f42 = ybase;
            idx++;
            if (rank[i] >= 500) {
                level = 0;
            } else if (score[i] < 6) {
                level = 6;
            } else if (score[i] < 11) {
                level = 5;
            } else if (score[i] < 21) {
                level = 4;
            } else if (score[i] < 41) {
                level = 3;
            } else if (score[i] < 61) {
                level = 2;
            } else {
                level = 1;
            }
            days = screen_w.chr[i].days;
            class_w[idx].f24 = level + 89; class_w[idx].f38 = xbase - 34; class_w[idx].f42 = ybase;
            idx++;
            if (days >= 60) {
                class_w[idx].f6 = -(abs(days - 60) / 10) * 2 + 8;
                if (class_w[idx].f6 == 0) {
                    class_w[idx].f6 = 1;
                }
                class_w[idx].f24 = 96;
                class_w[idx].f38 = xbase - 18;
                class_w[idx].f42 = ybase;
                idx++;
            }
            screen_w.level[i] = level + 1;
        }
    }
    work_fuku[1] = set_kanji_string(-128, -98, 4, tel_girl_status_init_str1, 0);
}
