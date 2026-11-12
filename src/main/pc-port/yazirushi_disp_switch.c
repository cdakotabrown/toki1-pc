/*
 * yazirushi_disp_switch -- PC override of src/main/matched/yazirushi_disp_switch.c
 * (0x8006C8F0).
 *
 * Statement for statement the matched function; the divergence is the
 * handle WIDTH.  yazirushi_cw_img / _anim_tbl / _hdr are the 4-byte PS1 handles at
 * +12/+16/+52 of class_w slot 2 (0x80127244 / 48 / 6C).  The
 * matched C declares them `void *`, 8 bytes on LP64, so each
 * store's high half (0) also lands on the next PS1 word: _anim_tbl
 * and _attr are written again afterwards, but slot +20
 * (0x8012724C, not assigned here) is left 0 -- latent today,
 * class_clear_n(2) has zeroed the slot already.  Here they are u32 and hold the PS1 address, as
 * on the console (pc/include/game/class_slot.h), so the slot no
 * longer depends on the store order.
 *
 * @DivergesFrom: matched yazirushi_disp_switch -- handle store width only
 * (pc/tools/audit_ps1_wide_access.py).
 */
#include <stdint.h>
#include "common.h"

/* External function declarations */
extern void class_clear_n(s32 arg0);

/* External data - addresses at 0x80127200 range (offset 29xxx) */
extern u8 yazirushi_cw_prio;     /* 29241 */
extern u8 yazirushi_cw_mode;     /* 29242 */
extern u8 yazirushi_cw_disp;     /* 29243 */
extern u8 yazirushi_cw_flag4;     /* 29244 */
extern u8 yazirushi_cw_sel;     /* 29245 */
extern u32 yazirushi_cw_img;   /* PS1 handle */  /* 29252 */
extern u32 yazirushi_cw_anim_tbl;   /* PS1 handle */  /* 29256 */
extern s16 yazirushi_cw_frame;    /* 29264 */
extern s16 yazirushi_cw_x;    /* 29278 */
extern s16 yazirushi_cw_y;    /* 29282 */
extern u32 yazirushi_cw_hdr;   /* PS1 handle */  /* 29292 */
extern s32 yazirushi_cw_attr;    /* 29296 */

/* Data pointers */
extern u8 basyo_init_tbl1;     /* 0x800d0000 - 932 */
extern u8 basyo_init_tbl2;     /* 0x800d0000 - 236 */
extern u8 _sys_default_tpage_set_tbl2;     /* 0x800d0000 - 976 */

void yazirushi_disp_switch(s32 arg0)
{
    class_clear_n(2);

    yazirushi_cw_prio = 2;
    yazirushi_cw_mode = 64;
    yazirushi_cw_attr = 0x40000000;

    yazirushi_cw_img = (u32)(uintptr_t)&basyo_init_tbl1;
    yazirushi_cw_anim_tbl = (u32)(uintptr_t)&basyo_init_tbl2;
    yazirushi_cw_hdr = (u32)(uintptr_t)&_sys_default_tpage_set_tbl2;

    yazirushi_cw_frame = 1;
    yazirushi_cw_flag4 = 8;
    yazirushi_cw_sel = 1;
    yazirushi_cw_x = 116;
    yazirushi_cw_y = 72;

    if (arg0 == 1) {
        yazirushi_cw_disp = yazirushi_cw_disp | 0x80;
    } else {
        yazirushi_cw_disp = yazirushi_cw_disp & 0x7F;
    }
}
