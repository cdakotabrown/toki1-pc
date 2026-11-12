/* SPDX-License-Identifier: GPL-3.0-or-later */
/* option_init0: preserve the retail four contiguous menu rows.
 * @DivergesFrom: src/overlay/OPTION/option_init0.c -- option_row_pos_set
 * writes 32 s16 values from its base pointer. The PS1 stack's sp60/sp76/
 * sp92/sp108 arrays are that one table, in ascending address order.
 * Independent native arrays do not preserve adjacency and overwrite the
 * stack canary. A single 4x8 array retains the data and every original call.
 */
#include "types.h"

extern void prim_init(void);
extern void sub_sub_step_up(void);
extern void draw2d3d(s32, s32);
extern void back_clear_switch(s32);
extern void cd_read_start2(s32, s32, s32);
extern void tpage_buf_clear(void);
extern void class_switch(s32);
extern void class_clear_f(void);
extern void white_light(s32);
extern void text_sys_init(void);
extern void set_kanji_string(s32, s32, s32, u8 *, s32);
extern void k_sub_reset_point_set(void);
extern void k_sub_disp_start(s32);
extern void menu_set(s32, s32, s16 *, s16 *, s16 *, s16 *);
extern void card_state_clear(void);
extern s32 card_check_main(void);
extern void card_step_init(void);
extern s32 card_file_read_main(void);
extern s32 card_file_check(s32);
extern s32 save_work_restore(s32);
extern void cursor_init(s32);
extern void printf(u8 *);
extern void option_row_pos_set(s16 *);

extern u8 _debug_number_show_str0;
extern u8 back_clear_r;
extern u8 back_clear_g;
extern u8 back_clear_b;
extern u8 screen_bri;
extern u8 now_icon;
extern u8 menu_csr;
extern s32 class_max;
extern u8 vblank_flag;
extern u8 step_mode;
extern u8 card_result;
extern u32 card_cnt;
extern u8 option_on;
extern s16 cal_cw_frame;
extern s16 cal_cw_x;
extern s16 cal_cw_y;
extern s32 work_fuku;
extern s32 option_bg_flag;

extern u8 option_init0_str0[];
extern u8 option_init0_str1[];
extern u8 option_init0_str2[];
extern u8 option_init0_str3[];
extern u8 option_init0_str4[];
extern u8 option_init0_str5[];
extern u8 option_init0_str6[];
extern u8 option_init0_str7[];
extern u8 option_init0_str8[];
extern u8 option_init0_str9[];
extern u8 option_init0_str10[];
extern u8 option_init0_str11[];
extern u8 option_init0_str12[];
extern u8 option_init0_str13[];
extern u8 option_init0_str14[];

void option_init0(void)
{
    s16 padA[10];
    s16 rows[4][8];
    s16 padB[6];
    u32 sel;
    u32 sub;

    sel = step_mode;

    switch (sel) {
    case 0:
        option_on = 1;
        draw2d3d(1, 0);
        class_switch(1);
        prim_init();
        class_clear_f();
        class_max = 160;
        back_clear_switch(1);
        back_clear_g = 0;
        back_clear_r = 0;
        back_clear_b = 0;
        tpage_buf_clear();
        vblank_flag = 0;
        text_sys_init();
        now_icon = 0;
        menu_csr = 0;
        cursor_init(0);
        screen_bri = 255;
        option_bg_flag = 0;
        _debug_number_show_str0 = 0;
        work_fuku = 0;
        option_row_pos_set(rows[0]);
        menu_set(0, 8, rows[0], rows[1], rows[2], rows[3]);
        set_kanji_string(rows[0][0], rows[1][0], 0, option_init0_str0, 0);
        set_kanji_string(rows[0][1], rows[1][1], 0, option_init0_str1, 0);
        set_kanji_string(rows[0][2], rows[1][2], 0, option_init0_str2, 0);
        set_kanji_string(rows[0][3], rows[1][3], 0, option_init0_str3, 0);
        set_kanji_string(rows[0][4], rows[1][4], 0, option_init0_str4, 0);
        set_kanji_string(rows[0][5], rows[1][5], 0, option_init0_str5, 0);
        set_kanji_string(rows[0][6], rows[1][6], 0, option_init0_str6, 0);
        set_kanji_string(rows[0][7], rows[1][7], 0, option_init0_str7, 0);
        k_sub_reset_point_set();
        k_sub_disp_start(0);
        cal_cw_frame = 0;
        cal_cw_x = -160;
        cal_cw_y = -120;
        step_mode = 1;
        card_step_init();
        break;
    case 1:
        if (card_check_main() != 0) {
            sel = card_result;
            if (sel == 1) {
                printf(option_init0_str8);
                sel = step_mode;
            step_mode = sel + 1;
            } else if (sel == 241) {
                printf(option_init0_str9);
                sel = step_mode;
            step_mode = sel + 1;
            } else if (sel == 255) {
                printf(option_init0_str10);
                sel = step_mode;
            step_mode = sel + 1;
            }
        } else if (card_cnt >= 513) {
            printf(option_init0_str11);
            sel = step_mode;
            step_mode = sel + 1;
        }
        break;
    case 2:
        card_state_clear();
        sel = card_result;
        if (sel == 1) {
            if (card_file_check(0) == 1) {
                sel = step_mode;
            step_mode = sel + 1;
                card_step_init();
            } else {
                sel = 255;
            step_mode = sel;
            }
            if (card_file_check(15) == 1) {
                printf(option_init0_str12);
                _debug_number_show_str0 = 1;
            } else {
                _debug_number_show_str0 = 0;
            }
        } else {
            sel = 255;
            step_mode = sel;
        }
        break;
    case 3:
        if (card_file_read_main() == 3) {
            card_state_clear();
            sel = step_mode;
            step_mode = sel + 1;
        } else if (card_cnt >= 513 || (sel = card_result) == 240) {
            card_state_clear();
            sel = 255;
            step_mode = sel;
        }
        break;
    case 4:
        if (save_work_restore(0) == -1) {
            printf(option_init0_str13);
            sel = 255;
            step_mode = sel;
        } else {
            printf(option_init0_str14);
            sel = 255;
            step_mode = sel;
        }
        break;
    case 255:
    default:
        cd_read_start2(61, 0x801B0000, 0x9EA4);
        sub_sub_step_up();
        break;
    }

    white_light(screen_bri);
}
