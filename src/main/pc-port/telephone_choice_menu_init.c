/*
 * telephone_choice_menu_init -- PC override of src/main/matched/telephone_choice_menu_init.c
 * (0x8007837C).
 *
 * Statement for statement the matched function; the divergence is the
 * handle WIDTH.  cal_cw_img / cal_cw_anim_tbl / cal_cw_hdr are the 4-byte PS1
 * handles at +12/+16/+52 of class_w slot 96 (0x80128B3C / B40 /
 * B64).  The matched C declares them `u32 *`, 8 bytes on LP64, so
 * each store's high half (0) also lands on the next PS1 word
 * (cal_cw_anim_tbl, cal_cw_aux14 / cal_cw_anim, cal_cw_attr); the
 * build's store order writes each of them again afterwards, so
 * nothing is lost today.  Here they are u32 and hold the PS1 address, as
 * on the console (pc/include/game/class_slot.h), so the slot no
 * longer depends on the store order.
 *
 * The three name tables are read at PS1 width too.  telephone_choice_menu_init_tbl0
 * (0x800B9A20) is 13 4-byte PS1 string pointers (the girls' names) and
 * tbl1 / tbl2 (0x800B9A54 / 58) one each.  The matched C declares tbl0
 * `u32 *[]` (8-byte stride: entry i read words 2i and 2i+1, i.e. two names
 * joined, and i >= 7 ran past the table) and tbl1/tbl2 `s32` cast to a
 * pointer, which sign-extends 0x800B9A00 to 0xFFFFFFFF800B9A00.  The first
 * set_kanji_string (i == 0, tbl2) then dereferenced that address: the
 * holiday screen's telephone icon hung the port in set_kanji_string
 * (watchdog SIGALRM every frame, scene 70 sub 16).
 *
 * @DivergesFrom: matched telephone_choice_menu_init -- handle store and name-table read
 * width only (pc/tools/audit_ps1_wide_access.py).
 */
#include <stdint.h>
#include "types.h"

extern u32 telephone_choice_menu_init_tbl2;    /* PS1 string pointer */
extern u32 telephone_choice_menu_init_tbl1;    /* PS1 string pointer */
extern u32 telephone_choice_menu_init_tbl0[];  /* PS1 string pointers */
extern u8 screen_w[];
extern u8 now_icon;
extern s8 cal_cw_prio;
extern u8 cal_cw_mode;
extern u8 cal_cw_disp;
extern u8 cal_cw_flag4;
extern u8 cal_cw_sel;
extern u8 cal_cw_rate;
extern u32 cal_cw_img;   /* PS1 handle */
extern u32 cal_cw_anim_tbl;   /* PS1 handle */
extern u32 cal_cw_hdr;   /* PS1 handle */
extern s32 cal_cw_attr;
extern s16 cal_cw_aux14;
extern s16 cal_cw_anim;
extern s16 cal_cw_frame;
extern s16 cal_cw_x;
extern s16 cal_cw_y;
extern s16 basyo_init_tbl3;
extern u8 telephone_choice_menu_init_str0;
extern u8 telephone_choice_menu_init_str1;
extern u32 basyo_init_tbl1;
extern u32 basyo_init_tbl2;
extern u32 _sys_default_tpage_set_tbl2;

extern void set_kanji_string(s32, s32, s32, u8 *, s32);
extern void menu_set(s32, s32, s16 *, s16 *, s16 *, s16 *);
extern void class_clear_n(s32);

void telephone_choice_menu_init(void) {
    s16 spBC[14];
    s16 spA0[14];
    s16 sp84[14];
    s16 sp68[14];
    s32 i;

    set_kanji_string(-128, -84, 3, &telephone_choice_menu_init_str0, 0);

    for (i = 0; i < 14; i++) {
        sp84[i] = 72;
        spBC[i] = ((i / 5) * 88) - 136;
        sp68[i] = 14;
        spA0[i] = ((i % 5) * 16) - 56;

        if (i == 0) {
            set_kanji_string(spBC[i], spA0[i], 3, (u8 *)(uintptr_t)telephone_choice_menu_init_tbl2, 0);
        } else if (i == 13) {
            spA0[i] += 16;
            set_kanji_string(spBC[i], spA0[i], 3, &telephone_choice_menu_init_str1, 0);
        } else if (i == 12) {
            set_kanji_string(spBC[i], spA0[i], 3, (u8 *)(uintptr_t)telephone_choice_menu_init_tbl1, 0);
        } else if ((*(u32 *)(screen_w + i * 56 + 400) << 28) >> 31) {
            set_kanji_string(spBC[i], spA0[i], 3, (u8 *)(uintptr_t)telephone_choice_menu_init_tbl0[i], 0);
        } else {
            sp84[i] = 0;
        }
    }

    menu_set(0, 14, spBC, spA0, sp84, sp68);

    now_icon = 0;
    class_clear_n(96);

    cal_cw_prio = 0;
    cal_cw_mode = 64;
    cal_cw_attr = 0x40000000;
    cal_cw_disp = 132;
    cal_cw_img = (u32)(uintptr_t)&basyo_init_tbl1;
    cal_cw_anim_tbl = (u32)(uintptr_t)&basyo_init_tbl2;
    cal_cw_hdr = (u32)(uintptr_t)&_sys_default_tpage_set_tbl2;
    cal_cw_aux14 = basyo_init_tbl3;
    cal_cw_flag4 = 8;
    cal_cw_sel = 1;
    cal_cw_anim = 0;
    cal_cw_rate = 2;
    cal_cw_frame = 63;
    cal_cw_x = -16;
    cal_cw_y = -19;
}
