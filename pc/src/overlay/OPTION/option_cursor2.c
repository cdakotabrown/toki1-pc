/* SPDX-License-Identifier: GPL-3.0-or-later */
/* option_cursor2: reserve the complete output of option_row_pos_set.
 * @DivergesFrom: src/overlay/OPTION/option_cursor2.c -- that helper writes
 * four rows, although this caller reads only x/y. Keep the unused w/h rows
 * in the same buffer rather than allowing a native stack overwrite.
 */
#include "types.h"

extern s16 cursor_cw_x;
extern s16 cursor_cw_y;
extern u8 cw97_disp;
extern s16 cw97_x;
extern s16 cw97_y;
extern s8 now_icon;
extern s32 pad_trig;
extern s32 pad_data;
extern u8 pad_type;
extern volatile u8 _debug_number_show_str0;

extern void menu_check();
extern void menu_cursor_move();
extern void option_row_pos_set();
extern void device_cmd_send();
extern void goto_sub_step();
extern void goto_step();

void option_cursor2(void)
{
    s32 row;
    struct { s16 xs[8]; s16 ys[8]; s16 ws[8]; s16 hs[8]; } b;

    menu_check(0, cursor_cw_x, cursor_cw_y);
    menu_cursor_move(0);

    row = now_icon;
    if (row == -1) {
        cw97_disp = cw97_disp & ~0x80;
    } else {
        option_row_pos_set(&b);
        row = now_icon;
        cw97_disp = cw97_disp | 0x80;
        cw97_x = b.xs[row] + 48;
        cw97_y = *(s16 *)((u8 *)&b + 16 + (row << 1)) + 6;
    }

    if ((pad_trig & 0x20) != 0) {
        switch ((u32)row) {
        case 0:
            device_cmd_send(1281);
            goto_sub_step(1);
            break;
        case 1:
            device_cmd_send(1281);
            goto_sub_step(2);
            break;
        case 2:
            if (pad_type == 0) {
                device_cmd_send(1281);
                goto_sub_step(3);
            }
            break;
        case 3:
            if (pad_type == 1) {
                device_cmd_send(1281);
                goto_sub_step(4);
            }
            break;
        case 4:
            device_cmd_send(1281);
            goto_sub_step(5);
            break;
        case 5:
            device_cmd_send(1281);
            goto_sub_step(6);
            break;
        case 6:
            device_cmd_send(1281);
            goto_sub_step(7);
            break;
        case 7:
            device_cmd_send(1281);
            goto_sub_step(8);
            break;
        }
    }

    if (_debug_number_show_str0 == 1 && (pad_data & 0xF) == 15) {
        goto_step(33);
    }
    if (_debug_number_show_str0 == 1 && (pad_data & 0xF0000) == 0xF0000) {
        goto_step(34);
    }
    if ((pad_trig & 0x40) != 0) {
        now_icon = 7;
    }
}
