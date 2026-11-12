/*
 * join_club_message -- PC override of src/main/matched/join_club_message.c (0x80060948).
 *
 * Statement for statement the matched function; the divergence is the
 * table WIDTH.  album_info_disp_tbl1 (0x800B9DF4) is a table of 4-byte PS1 club-name
 * pointers; the matched C declares it `u32 *album_info_disp_tbl1[]`, so entry i is read 8
 * bytes wide at stride 8 (words 2i and 2i+1 joined: a non-canonical
 * source).  Here each entry is read as u32 and widened (PS1_PTR), as the
 * console reads it.  Found by pc/tools/audit_ps1_wide_access.py (WIDE-INDEX
 * through a lea'd base); not entered by the scripted playthroughs yet.
 *
 * @DivergesFrom: matched join_club_message -- table read width only.
 */
#include <stdint.h>
/* string/BIOS calls -> the BIOS trampolines, as in the matched TU (pc-port
 * objects do not get the force-include) */
#include "sony_bios_alias.h"
#include "types.h"

extern u8 step_mode;
extern u16 basyo_pack;
extern u32 album_info_disp_tbl1[];    /* PS1 club-name pointers */
#define PS1_PTR(t, i) ((void *)(uintptr_t)(t)[i])

extern char join_club_message_str0[];
extern char join_club_message_str1[];
extern char join_club_message_str2[];
extern char join_club_message_str3[];
extern char join_club_message_str4[];
extern char join_club_message_str5[];
extern char join_club_message_str6[];
extern char join_club_message_str7[];

extern void k_sub_reset(void);
extern void strcpy(char *, u32 *);
extern void strcat(char *, char *);
extern void sndisp(char *, s32, s32);
extern s32 k_sub_disp_start(s32);
extern s32 xa_wait(void);
extern void gnsx(char *);
extern void set_kanji_string(s16, s16, s32, char *, s32);
extern s32 xa_set_m(u32, u32, s32, s32);
extern s32 xa_wait_m(void);
extern s32 sub_sub_step_up(void);

s32 join_club_message(void) {
    u32 val;
    char buffer[64];
    u32 r;

    r = step_mode;

    if (r >= 5) {
        if (r != 255) {
            goto out;
        }
        goto do_429FC;
    }

    switch (r) {
    case 0:
        k_sub_reset();
        val = basyo_pack;
        strcpy(buffer, PS1_PTR(album_info_disp_tbl1, val >> 12));
        strcat(buffer, join_club_message_str0);
        sndisp(buffer, 0, 255);
        r = k_sub_disp_start(1);
        step_mode = step_mode + 1;
        break;
    case 1:
        r = xa_wait();
        break;
    case 2:
        val = basyo_pack;
        if ((u32)(val >> 12) >= 6) {
            k_sub_reset();
            gnsx(join_club_message_str1);
            set_kanji_string(-86, 50, 0, join_club_message_str2, 0);
            set_kanji_string(-44, 50, 1, join_club_message_str3, 0);
            set_kanji_string(26, 50, 0, join_club_message_str4, 0);
            set_kanji_string(-72, 66, 0, join_club_message_str5, 0);
            set_kanji_string(-72, 82, 1, join_club_message_str6, 0);
            set_kanji_string(-16, 82, 0, join_club_message_str7, 0);
            r = xa_set_m(0x0D000011, 0x0300001F, -1, 14);
            step_mode = step_mode + 1;
        } else {
            step_mode = 255;
        }
        break;
    case 3:
        r = xa_wait_m();
        break;
    case 4:
        step_mode = 255;
        break;
    }
    goto out;
do_429FC:
    r = sub_sub_step_up();
out:
    return r;
}
