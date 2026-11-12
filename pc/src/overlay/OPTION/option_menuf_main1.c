/* SPDX-License-Identifier: GPL-3.0-or-later */
/* option_menuf_main1 -- PC override of src/overlay/OPTION/option_menuf_main1.c.
 *
 * Statement for statement the matched function; the divergence is the
 * TABLE WIDTH (the src/main/pc-port/basyo_init.c doctrine).  The matched C
 * walks the legend-string table with `u8 **p = &option_menuf_lbl_tbl[0]; ... p++`.
 * option_menuf_lbl_tbl is OPTION .data: sixteen 4-byte PS1 string handles
 * (0x8013D048, 0x8013D054, ... 0x8013D0CC in OPTION.EXN), so on LP64 the
 * native walk fuses handles 2k and 2k+1 into one 8-byte pointer and its
 * last eight steps run past the table.  generate_overlay_modules.py cannot
 * widen this form (the address of a cell, not an access; see
 * ps1_pointer_cells.py), so the table is walked here as u32 cells and each
 * handle is passed as the PS1 address it holds.
 *
 * @DivergesFrom: src/overlay/OPTION/option_menuf_main1.c -- table cell width only.
 * Test: pc/tools/test_ps1_handle_records.py.
 */
#include "types.h"

extern s16 cursor_cw_x;
extern s16 cursor_cw_y;
/* option_menuf_lbl_tbl: PS1 string handles */
extern u32 option_menuf_lbl_tbl[];
extern u8 work_miura[];
extern s32 pad_trig;
extern u8 now_icon;
extern u8 return_schedule_tbl0;
extern u16 basyo_pack;

extern void menu_check();
extern void menu_cursor_move();
extern void menu_bar_show();
extern void debug_string_show();
extern void bzero();
extern void goto_step();
extern void text_sys_init();
extern void class_clear();

/* the string at a PS1 handle */
#define OPTION_STR(h) ((u8 *)(unsigned long)(h))

void option_menuf_main1(void)
{
    s32 y;
    u32 *p;

    menu_check(0, cursor_cw_x, cursor_cw_y);
    menu_cursor_move(0);
    menu_bar_show(0);

    p = &option_menuf_lbl_tbl[0];
    y = -96;
    do {
        debug_string_show(OPTION_STR(*p), -32, y);
        y += 12;
        p++;
    } while (y != 96);

    bzero(work_miura, 256);

    if ((pad_trig & 0x90) != 0) {
        switch ((u32)now_icon) {
        case 0:
            return_schedule_tbl0 = 0;
            goto_step(52);
            break;
        case 1:
            goto_step(80);
            break;
        case 2:
            text_sys_init();
            class_clear(0);
            goto_step(93);
            break;
        case 3:
            goto_step(87);
            break;
        case 4:
            goto_step(86);
            break;
        case 5:
            goto_step(90);
            break;
        case 6:
            goto_step(114);
            break;
        case 7:
            goto_step(113);
            if (((u32)basyo_pack >> 12) < 6) {
                goto_step(112);
            }
            break;
        case 8:
            goto_step(81);
            break;
        case 9:
            goto_step(83);
            break;
        case 10:
            goto_step(85);
            break;
        case 11:
            goto_step(84);
            break;
        case 12:
            goto_step(88);
            break;
        case 13:
            goto_step(82);
            break;
        case 14:
            goto_step(94);
            break;
        case 15:
            goto_step(89);
            break;
        }
    }
}
