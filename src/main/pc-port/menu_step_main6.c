/*
 * menu_step_main6 -- PC override of src/main/matched/menu_step_main6.c (0x8005D1AC).
 *
 * Statement for statement the matched function; the divergence is the
 * pointer WIDTH of the album-name table read.  card_file_save_tbl0
 * (0x800B97B8) holds the PS1 address of a table of 4-byte PS1 string
 * pointers (0x800B9778: the 14 album names, the card_file_write class,
 * 2a3399c4fe).  The matched C reads entry i as `*(u8 **)(tbl + 4*i)`, an
 * 8-byte load on LP64 that joins entries i and i+1 into a non-canonical
 * address; the same read as save_file_select_init, in the sister
 * save/load slot menu.
 * Here the table cells are read as u32 and widened, as the console reads
 * them.
 *
 * @DivergesFrom: matched menu_step_main6 -- table read width only.
 */
#include <stdint.h>
#include "common.h"

/* External functions */
extern void text_sys_init(void);
extern void basyo_init2(u8 *a0);
extern void set_kanji_string(s16 x, s16 y, s32 a2, u8 *a3, s32 stack0);
extern void menu_set(s32 a0, s32 a1, s16 *a2, s16 *a3, s16 *stack0, s16 *stack1);
extern void k_sub_reset_point_set(void);
extern void sub_sub_step_up(void);
extern void goto_sub_sub_step(s32 a0);
extern void album_info_disp(s32 a0, s32 a1);
extern void save_msg_show(void);
extern void slot_box_disp(void);

/* External data */
extern u8 menu_step_main6_str0;
extern u32 card_file_save_tbl0;     /* PS1 pointer (0x800B9778) */
extern u8 menu_csr;
extern u8 now_album;
extern u8 album_work[];
extern u8 card_sys_present;
extern u8 last_album;

void menu_step_main6(void) {
    s32 i;
    s32 offset;
    u8 *pV0;
    s16 spCC[16];
    s16 spAC[16];
    s16 sp8C[16];
    s16 sp6C[16];
    s16 *ps1;
    s16 *ps2;
    s16 *ps3;
    s16 *ps4;

    text_sys_init();
    basyo_init2(&menu_step_main6_str0);

    i = 0;
    ps1 = spCC;
    ps2 = spAC;
    ps3 = sp8C;
    ps4 = sp6C;
    offset = 0;

    do {
        *ps3 = 32;
        *ps4 = 16;
        *ps1 = (i / 5) << 5;
        *ps2 = ((i % 5) << 4) - 64;
        set_kanji_string(*ps1, *ps2, 15,
                         (u8 *)(uintptr_t)*(u32 *)(uintptr_t)(card_file_save_tbl0 + offset), 0);
        i++;
        ps1++;
        ps2++;
        ps3++;
        ps4++;
        offset += 4;
    } while (i < 14);

    menu_set(1, 14, spCC, spAC, sp8C, sp6C);
    menu_csr = 0;
    k_sub_reset_point_set();

    pV0 = &album_work[1];   /* an array: pV0[4] is album_status[1] */
    i = 1;
    do {
        if (pV0[4] == 1) {
            break;
        }
        i++;
        pV0++;
    } while (i != 15);

    if (card_sys_present != 0) {
        menu_csr = last_album - 1;
        sub_sub_step_up();
        now_album = last_album;
        album_info_disp(0, 1);
        save_msg_show();
    } else if (i == 15) {
        save_msg_show();
        sub_sub_step_up();
    } else {
        now_album = (u8)i;
        goto_sub_sub_step(176);
    }

    slot_box_disp();
}
