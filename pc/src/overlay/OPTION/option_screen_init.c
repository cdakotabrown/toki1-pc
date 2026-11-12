/* SPDX-License-Identifier: GPL-3.0-or-later */
/* option_screen_init -- PC override of src/overlay/OPTION/option_screen_init.c.
 *
 * Statement for statement the matched function; the divergence is the
 * RECORD WIDTH (the src/main/pc-port/basyo_init.c doctrine).  The matched
 * OptEnt declares the class_w sprite slot's handles as `void *unk0C,
 * *unk10, *unk34`: 4 bytes on the console, 8 bytes (8-aligned) on LP64,
 * where the record is 88 bytes, so class_w[i + 96] strides 88 and every
 * field after +12 moves.  Here the record is the shared PS1-width
 * ClassSlot (pc/include/game/class_slot.h; matched unkNN is ClassSlot
 * f<NN decimal>; unk30/unk32 are the two s16 halves of f48) and the
 * handles are u32 PS1 addresses at +12/+16/+52.
 *
 * @DivergesFrom: src/overlay/OPTION/option_screen_init.c -- record field
 * widths only.  Test: pc/tools/test_class_slot_width.py.
 */
#include "types.h"
#include "class_slot.h"
#include "tokimeki/libgs_types.h"

extern void pn_set();
extern void draw2d3d();
extern void class_switch();
extern void prim_init();
extern void class_clear_all();
extern void back_clear_switch();
extern void tpage_buf_clear();
extern void text_sys_init();
extern void basyo_init();
extern void k_disp_start();
extern void set_kanji_string();
extern void cursor_init();
extern void menu_set();
extern void menu_bar_color();
extern void class_clear_n();
extern s32 rand();
extern void sub_sub_step_up();

extern u8 vblank_flag;
extern u32 class_max;
extern u8 now_icon;
extern u8 menu_csr;
extern u8 menu_mode;
extern u8 screen_bri;
extern u8 option_main_vab_wk;
extern u8 option_sub_vab_wk;
extern u8 cursor_cw_disp;

extern u8 option_screen_init_str0;
extern u8 option_screen_init_str1;
extern u8 option_screen_init_str2;
extern u8 option_screen_init_str3;
extern u8 option_screen_init_str4;
extern u8 option_screen_init_str5;
extern u8 option_screen_init_str6;
extern u8 option_screen_init_str7;

extern s16 option_screen_x_tbl[];
extern s16 option_screen_y_tbl[];
extern s16 option_screen_w_tbl[];
extern s16 option_screen_h_tbl[];

extern u8 mascot_init_tbl0;
extern u8 mascot_init_tbl1;
extern u8 _sys_default_tpage_set_tbl0;
extern s16 mascot_init_tbl2;

extern ClassSlot class_w[];
extern LIBGS_BG Bg[];

void option_screen_init(void)
{
    s32 i;

    pn_set(4);
    draw2d3d(1, 0);
    class_switch(1);
    prim_init();
    class_clear_all(0);
    back_clear_switch(0);
    tpage_buf_clear();

    vblank_flag = 0;
    class_max = 160;

    text_sys_init();
    basyo_init(&option_screen_init_str0);
    k_disp_start(1);

    set_kanji_string(option_screen_x_tbl[0], option_screen_y_tbl[0], 0, &option_screen_init_str1, 0);
    set_kanji_string(option_screen_x_tbl[1], option_screen_y_tbl[1], 0, &option_screen_init_str2, 0);
    set_kanji_string(option_screen_x_tbl[2], option_screen_y_tbl[2], 0, &option_screen_init_str3, 0);
    set_kanji_string(option_screen_x_tbl[3], option_screen_y_tbl[3], 0, &option_screen_init_str4, 0);
    set_kanji_string(option_screen_x_tbl[4], option_screen_y_tbl[4], 0, &option_screen_init_str5, 0);
    set_kanji_string(option_screen_x_tbl[5], option_screen_y_tbl[5], 0, &option_screen_init_str6, 0);
    set_kanji_string(option_screen_x_tbl[6], option_screen_y_tbl[6], 3, &option_screen_init_str7, 0);

    now_icon = 0;
    menu_csr = 0;
    menu_mode = 0;

    cursor_init(0);

    cursor_cw_disp |= 1;

    option_main_vab_wk = 0;
    option_sub_vab_wk = 0;
    Bg[0].b = 0;
    Bg[0].g = 0;
    Bg[0].r = 0;
    screen_bri = 0;

    menu_set(0, 7, option_screen_x_tbl, option_screen_y_tbl, option_screen_w_tbl, option_screen_h_tbl);

    menu_bar_color(0, 0x005F7F3F, 9, 8);
    menu_bar_color(1, 0x007F7F7F, 2, 1);

    for (i = 0; i < 13; i++) {
        class_clear_n(i + 96);
        class_w[i + 96].f1 = 2;
        class_w[i + 96].f2 = 64;
        class_w[i + 96].f56 = 0x40000000;
        class_w[i + 96].f3 = 148;
        class_w[i + 96].f12 = CLASS_SLOT_HANDLE(&mascot_init_tbl0);
        class_w[i + 96].f16 = CLASS_SLOT_HANDLE(&mascot_init_tbl1);
        class_w[i + 96].f52 = CLASS_SLOT_HANDLE(&_sys_default_tpage_set_tbl0);
        class_w[i + 96].f20 = mascot_init_tbl2;
        class_w[i + 96].f4 = 8;
        class_w[i + 96].f5 = 1;
        class_w[i + 96].f22 = 0;
        class_w[i + 96].f24 = i;
        class_w[i + 96].f46 = 3 - (rand() & 7);
        ((s16 *)&class_w[i + 96].f48)[1] = 4 - (rand() & 7);
        class_w[i + 96].f44 = rand();
        ((s16 *)&class_w[i + 96].f48)[0] = rand();
        class_w[i + 96].f38 = 0;
        class_w[i + 96].f42 = 0;
    }

    sub_sub_step_up();
}
