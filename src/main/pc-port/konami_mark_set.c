/*
 * konami_mark_set -- PC override of src/main/matched/konami_mark_set.c
 * (0x8006C808).
 *
 * Statement for statement the matched function; the divergence is the
 * handle WIDTH.  konami_mark_cw_img / _anim_tbl / _hdr are the 4-byte PS1 handles
 * at +12/+16/+52 of class_w slot 15 (0x801275B8 / BC / E0).  The
 * matched C declares them `void *`, 8 bytes on LP64, so each
 * store's high half (0) also lands on the next PS1 word (_anim_tbl,
 * _aux14 / _anim, _attr); the function assigns each of them
 * afterwards or assigns it 0, so nothing is lost today.  Here they are u32 and hold the PS1 address, as
 * on the console (pc/include/game/class_slot.h), so the slot no
 * longer depends on the store order.
 *
 * @DivergesFrom: matched konami_mark_set -- handle store width only
 * (pc/tools/audit_ps1_wide_access.py).
 */
#include <stdint.h>
#include "common.h"

/* External function declarations */
extern void class_clear_n(s32 arg0);

/* External data - addresses decoded from lui/addiu pairs */
extern u8 konami_mark_cw_prio;     /* 0x80127500 + 0x6D = 30125 decimal */
extern u8 konami_mark_cw_mode;     /* 30126 */
extern u8 konami_mark_cw_disp;     /* 30127 */
extern s32 konami_mark_cw_attr;    /* 30180 */
extern u32 konami_mark_cw_img;   /* PS1 handle */  /* 30136 */
extern u32 konami_mark_cw_anim_tbl;   /* PS1 handle */  /* 30140 */
extern u32 konami_mark_cw_hdr;   /* PS1 handle */  /* 30176 */
extern s16 konami_mark_cw_aux14;    /* 30144 */
extern u8 konami_mark_cw_flag4;     /* 30128 */
extern u8 konami_mark_cw_sel;     /* 30129 */
extern u8 konami_mark_cw_clut;     /* 30191 */
extern s16 konami_mark_cw_anim;    /* 30146 */
extern s16 konami_mark_cw_frame;    /* 30148 */
extern s16 konami_mark_cw_x;    /* 30162 */
extern s16 konami_mark_cw_y;    /* 30166 */

/* Data pointers */
extern u8 basyo_init_tbl1;     /* 0x800d0000 - 932 */
extern u8 basyo_init_tbl2;     /* 0x800d0000 - 236 */
extern u8 _sys_default_tpage_set_tbl2;     /* 0x800d0000 - 976 */
extern s16 basyo_init_tbl3;    /* 0x800d0000 - 172 */

void konami_mark_set(s32 arg0)
{
    class_clear_n(15);

    konami_mark_cw_prio = 0;
    konami_mark_cw_mode = 64;
    konami_mark_cw_attr = 0;

    if (arg0 != 0) {
        konami_mark_cw_disp = 140;
    } else {
        konami_mark_cw_disp = 0;
    }

    konami_mark_cw_img = (u32)(uintptr_t)&basyo_init_tbl1;
    konami_mark_cw_anim_tbl = (u32)(uintptr_t)&basyo_init_tbl2;
    konami_mark_cw_hdr = (u32)(uintptr_t)&_sys_default_tpage_set_tbl2;
    konami_mark_cw_aux14 = basyo_init_tbl3;

    konami_mark_cw_flag4 = 8;
    konami_mark_cw_sel = 1;
    konami_mark_cw_clut = 0;
    konami_mark_cw_anim = 0;
    konami_mark_cw_frame = 173;
    konami_mark_cw_x = -134;
    konami_mark_cw_y = -92;
}
