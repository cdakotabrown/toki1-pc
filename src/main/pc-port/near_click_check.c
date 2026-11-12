/*
 * near_click_check -- PC override of src/main/matched/near_click_check.c
 * (0x8005B438).
 *
 * Statement for statement the matched function except the set_XAsector
 * call.  set_XAsector(a0, a1, a2) takes the XA channel from a2 & 0xF; the
 * matched C passes two arguments.  The listing reloads a2 from the
 * volatile local holding the girl number (lw a2,44(sp) at 0x8005B4E8) and
 * nothing writes a2 again before the jal 0x80046560, so the console plays
 * the clicked girl's channel: set_XAsector(x, y, girl).  On x86-64 the
 * channel was whatever %edx held.  pc/tools/audit_ps1_call_args.py
 * SHORT-READ.
 *
 * @DivergesFrom: src/main/matched/near_click_check.c -- the argument the
 * console passes in $a2 (`ix`, the volatile a2's value) is written out.
 */
#include "types.h"

typedef struct {
    s32 x;
    s32 y;
} D_800BA088_Entry;

extern u8 mascot_cw_disp;
extern s16 mascot_cw_x;
extern s16 cursor_cw_x;
extern s16 mascot_cw_y;
extern s16 cursor_cw_y;
extern s32 pad_trig;
extern s32 frame_counter;
extern s16 near_click_check_tbl1;
extern u8 mascot_cw_frame;
extern u8 now_girl;
extern u8 near_click_check_tbl0[];

extern s32 get_g_zyotai_s(s32);
extern void set_XAsector(s32, s32, s32);
extern void device_cmd_send(s32);

s32 near_click_check(void)
{
    volatile s32 a2;
    u8 tmp;
    s32 arg;
    s32 ret;
    s32 sel;
    s32 ix;
    D_800BA088_Entry *entry;

    if (!(mascot_cw_disp & 0x80)) {
        goto fail;
    }

    if (mascot_cw_x + 12 <= cursor_cw_x) {
        goto fail;
    }
    if (mascot_cw_x - 12 >= cursor_cw_x) {
        goto fail;
    }

    if (mascot_cw_y + 48 <= cursor_cw_y) {
        goto fail;
    }
    if (mascot_cw_y >= cursor_cw_y) {
        goto fail;
    }

    if (!(pad_trig & 0x20)) {
        goto fail;
    }

    if ((u32)(frame_counter - near_click_check_tbl1) < 241) {
        goto fail;
    }

    tmp = mascot_cw_frame;
    now_girl = tmp;
    *(s32 *)&a2 = tmp;
    arg = tmp;
    ret = get_g_zyotai_s(arg);
    ix = a2;
    if (ret & 0x80) {
        sel = 1;
    } else {
        sel = 0;
    }
    entry = (D_800BA088_Entry *)(near_click_check_tbl0 + (ix * 16 + sel * 8));
    set_XAsector(entry->x, entry->y, ix);
    device_cmd_send(768);
    return 1;

fail:
    return 0;
}
