/*
 * holiday_club_select -- PC override of src/main/matched/holiday_club_select.c (0x800786F8).
 *
 * Statement for statement the matched function; the divergence is the
 * table WIDTH.  album_info_disp_tbl1 (0x800B9DF4) is a table of 4-byte PS1 club-name
 * pointers; the matched C declares it `u32 *album_info_disp_tbl1[]`, so entry i is read 8
 * bytes wide at stride 8 (words 2i and 2i+1 joined: a non-canonical
 * source).  Here each entry is read as u32 and widened (PS1_PTR), as the
 * console reads it.  Found by pc/tools/audit_ps1_wide_access.py (WIDE-INDEX
 * through a lea'd base); not entered by the scripted playthroughs yet.
 *
 * @DivergesFrom: matched holiday_club_select -- table read width only.
 */
#include <stdint.h>
/* string/BIOS calls -> the BIOS trampolines, as in the matched TU (pc-port
 * objects do not get the force-include) */
#include "sony_bios_alias.h"
#include "types.h"

typedef struct {
    u8 pad[0x48];
} TempStruct;

extern s16 cursor_cw_y;
extern s16 cursor_cw_x;
extern u32 pad_trig;
extern s8 menu_csr;
extern u8 now_sub_sub_step;
extern u8 screen_w[];
extern u16 basyo_pack;
extern u32 album_info_disp_tbl1[];    /* PS1 club-name pointers */
#define PS1_PTR(t, i) ((void *)(uintptr_t)(t)[i])
extern u8 holiday_club_select_tbl0[];

extern void menu_check(s32, s16, s16);
extern void menu_bar_show(s32);
extern void menu_cursor_move(s32);
extern void goto_sub_step(s32);
extern void goto_sub_sub_step(u8);
extern void text_sys_init(void);
extern void strcpy(TempStruct *, u32 *);
extern void strcat(TempStruct *, u8 *);
extern void sndi(TempStruct *, s32, s32);
extern void sub_sub_step_up(void);
extern void k_disp_start(s32);

void holiday_club_select(void) {
    TempStruct temp;
    volatile u8 *p;

    menu_check(1, cursor_cw_x, cursor_cw_y);
    menu_bar_show(1);
    menu_cursor_move(1);

    if (pad_trig & 0x40) {
        goto_sub_step(0);
    }

    if (!(pad_trig & 0x20)) {
        return;
    }

    switch (menu_csr) {
    case 0:
        sub_sub_step_up();
        return;
    case 1:
        break;
    default:
        return;
    }

    goto_sub_sub_step((u8)(now_sub_sub_step + 2));
    text_sys_init();
    strcpy(&temp, PS1_PTR(album_info_disp_tbl1, (u32)basyo_pack >> 12));
    strcat(&temp, holiday_club_select_tbl0);
    sndi(&temp, 0, 255);

    p = screen_w;
    *(p + 1647 + ((u32)basyo_pack >> 12) * 4) |= 1;
    p[245] = (p[245] & 0xFF0F) | 0xB0;

    k_disp_start(1);
}
