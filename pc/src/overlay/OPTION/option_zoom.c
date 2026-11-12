/* SPDX-License-Identifier: GPL-3.0-or-later */
/* option_zoom: one contiguous retail 4x8 menu-row buffer.
 * @DivergesFrom: src/overlay/OPTION/option_zoom.c -- option_row_pos_set
 * fills all four adjacent rows. Native local-array order is unspecified.
 * Only the backing allocation changes; all calls and state writes remain.
 */
#include "types.h"
#include "tokimeki/class_w.h"

extern void icon_disp_switch();
extern void option_row_pos_set();
extern void menu_set();
extern void goto_sub_step();
extern void goto_sub_sub_step();
extern void option_row_slide();

extern u8 step_mode;
extern u8 class_w[];

void option_zoom(void)
{
    s16 rows[4][8];
    s32 i;

    if (step_mode == 0) {
        icon_disp_switch(0);
        ((CLASS_W *)class_w)[107].disp_flag = (u8)(((CLASS_W *)class_w)[107].disp_flag & ~0x80);
        for (i = 1; i < 9; i++) {
            class_w[i * 68 + 7279] =
                (u8)(class_w[i * 68 + 7279] & ~0x80);
        }

        ((CLASS_W *)class_w)[106].scale_x = (s16)((((CLASS_W *)class_w)[106].scale_x / 9) * 8);
        if (((CLASS_W *)class_w)[106].scale_x < 9) {
            step_mode = (u8)(step_mode + 1);
        }
    } else if (step_mode == 2) {
        icon_disp_switch(0);
        ((CLASS_W *)class_w)[107].disp_flag = (u8)(((CLASS_W *)class_w)[107].disp_flag & 0xFF7F);
        for (i = 1; i < 9; i++) {
            class_w[i * 68 + 7279] =
                (u8)(class_w[i * 68 + 7279] & ~0x80);
        }

        ((CLASS_W *)class_w)[106].scale_x = (s16)((((CLASS_W *)class_w)[106].scale_x * 9) / 8);
        ((CLASS_W *)class_w)[106].scale_y = (s16)((((CLASS_W *)class_w)[106].scale_y * 9) / 8);
        if (((CLASS_W *)class_w)[106].scale_x >= 16384) {
            step_mode = 1;
        }
    } else {
        option_row_pos_set(rows[0]);
        menu_set(0, 8, rows[0], rows[1], rows[2], rows[3]);
        goto_sub_step(0);
        goto_sub_sub_step(2);
    }

    option_row_slide();
    ((CLASS_W *)class_w)[106].disp_flag = 0;
}
