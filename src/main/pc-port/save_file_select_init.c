/*
 * save_file_select_init -- PC override of src/main/matched/save_file_select_init.c (0x8005C834).
 *
 * Statement for statement the matched function; the divergence is the
 * pointer WIDTH of the album-name table read.  card_file_save_tbl0
 * (0x800B97B8) holds the PS1 address of a table of 4-byte PS1 string
 * pointers (0x800B9778: the 14 album names, the card_file_write class,
 * 2a3399c4fe).  The matched C reads entry i as `*(u8 **)(tbl + 4*i)`, an
 * 8-byte load on LP64 that joins entries i and i+1 into a non-canonical
 * address; set_kanji_string then looped on it until the watchdog (SIGALRM
 * every frame, scene 52 sub 3: the schedule screen's save menu hung the
 * port).
 * Here the table cell is read as u32 and widened, as the console reads
 * it.
 *
 * @DivergesFrom: matched save_file_select_init -- table read width only.
 */
#include <stdint.h>
#include "common.h"

/* External functions */
extern void text_sys_init(void);
extern void basyo_init2(u8 *a0);
extern void set_kanji_string(s16 x, s16 y, s32 a2, u8 *a3, s32 stack0);
extern void menu_set(s32 a0, s32 a1, s16 *a2, s16 *a3, s16 *stack0, s16 *stack1);
extern void k_sub_reset_point_set(void);
extern void goto_sub_sub_step(s32 a0);
extern void album_info_disp(s32 a0, s32 a1);
extern void save_file_msg_disp(void);
extern void save_file_box_disp(void);

/* External data */
extern u8 save_file_select_init_str0;       /* 0x800B68AC - string data */
extern u32 card_file_save_tbl0;      /* 0x800B97B8 - pointer value */
extern u8 menu_csr;       /* 0x800EF7F4 */
extern u8 now_album;       /* 0x800EF878 */
extern u8 album_work[];
extern u8 card_sys_present;       /* 0x800F10CE */
extern u8 last_album;       /* 0x800F0216 */

void save_file_select_init(void) {
    s32 pad[3];
    s16 spCC[16];   /* x positions: sp+204 */
    s16 spAC[16];   /* y positions: sp+172 */
    s16 sp8C[16];   /* width: sp+140 */
    s16 sp6C[16];   /* height: sp+108 */
    s32 i;
    s16 *ps1;
    s16 *ps2;
    s16 *ps3;
    s16 *ps4;
    s32 offset;
    u8 *pV0;
    u8 val;

    (void)&pad;
    text_sys_init();
    basyo_init2(&save_file_select_init_str0);

    i = 0;
    ps1 = spCC;
    ps2 = spAC;
    ps3 = sp8C;
    ps4 = sp6C;
    offset = 0;

    do {
        *ps3 = 32;
        *ps4 = 16;
        *ps1 = ((i / 5) << 5) - 124;
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
        val = last_album;
        menu_csr = val - 1;
        now_album = val;
        goto_sub_sub_step(4);
        album_info_disp(0, 0);
        save_file_msg_disp();
    } else if (i == 15) {
        save_file_msg_disp();
        goto_sub_sub_step(4);
    } else {
        now_album = i;
        goto_sub_sub_step(192);
    }

    save_file_box_disp();
}
