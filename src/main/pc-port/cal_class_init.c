/*
 * cal_class_init -- PC override of src/main/matched/cal_class_init.c
 * (0x80067F60).
 *
 * Statement for statement the matched function; the divergence is the
 * handle WIDTH.  cal_cw_img / cal_cw_anim_tbl / cal_cw_hdr are the 4-byte
 * PS1 handles at +12/+16/+52 of class_w slot 96 (0x80128B3C / B40 / B64,
 * pc/ld/psx_globals.syms).  The matched C declares them `u32 *`, 8 bytes
 * on LP64, so each store's high half (0) also lands on the next PS1 word:
 * cal_cw_anim_tbl and cal_cw_attr, which the build's store order writes
 * again afterwards, and cal_cw_aux14 / cal_cw_anim, where cal_cw_anim
 * (+0x16, not assigned here) is left 0.  Latent today: class_clear_n(96)
 * has zeroed the slot already.  Here the handles are u32 and hold the
 * PS1 address, as on the console (pc/include/game/class_slot.h), so the
 * slot no longer depends on the store order.
 *
 * @DivergesFrom: matched cal_class_init -- handle store width only
 * (pc/tools/audit_ps1_wide_access.py).
 */
#include <stdint.h>
#include "types.h"

extern void class_clear_n(s32);
extern void schedule_mark_on(void);

extern u8 cal_cw_prio;
extern u8 cal_cw_mode;
extern u32 cal_cw_attr;
extern u8 cal_cw_disp;
extern u32 cal_cw_img;        /* PS1 handle */
extern u32 cal_cw_anim_tbl;   /* PS1 handle */
extern u32 cal_cw_hdr;        /* PS1 handle */
extern s16 cal_cw_aux14;
extern u8 cal_cw_flag4;
extern u8 cal_cw_sel;
extern s16 cal_cw_frame;
extern s16 cal_cw_x;
extern s16 cal_cw_y;

extern u32 basyo_init_tbl1;
extern u32 basyo_init_tbl2;
extern u32 _sys_default_tpage_set_tbl2;
extern s16 basyo_init_tbl3;

void cal_class_init(void)
{
    class_clear_n(96);

    cal_cw_prio = 4;
    cal_cw_mode = 64;
    cal_cw_attr = 0x40000000;
    cal_cw_disp = 132;
    cal_cw_img = (u32)(uintptr_t)&basyo_init_tbl1;
    cal_cw_anim_tbl = (u32)(uintptr_t)&basyo_init_tbl2;
    cal_cw_hdr = (u32)(uintptr_t)&_sys_default_tpage_set_tbl2;
    cal_cw_aux14 = basyo_init_tbl3;
    cal_cw_flag4 = 8;
    cal_cw_sel = 1;
    cal_cw_frame = 67;
    cal_cw_x = -16;
    cal_cw_y = -19;

    schedule_mark_on();
}
