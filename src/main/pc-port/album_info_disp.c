/*
 * album_info_disp -- PC override of src/main/matched/album_info_disp.c
 * (0x80069788).
 *
 * Statement for statement the matched function; the divergence is the
 * table WIDTH.  album_info_disp_tbl0 (0x800B9774: the digit / month
 * strings) and album_info_disp_tbl1 (0x800B9DF4: blood types, clubs) are
 * tables of 4-byte PS1 string pointers.  The matched C declares them
 * `u8 *[]`, so entry i is read 8 bytes wide at stride 8: entry 2i joined
 * with 2i+1, a non-canonical source the PC strcpy/strncat trampolines
 * skip ("not mapped on the PC; skipped").  It runs for the profile card of
 * the save and load menus (save_file_select_init -> album_info_disp(0, 0)
 * once a system file exists: the album number, birthday, blood type and
 * count lines).  Here each entry is read as u32 and widened (PS1_STR), as
 * the console reads it.
 *
 * @DivergesFrom: matched album_info_disp -- table read width only
 * (pc/tools/audit_ps1_wide_access.py WIDE-INDEX), and the English patch's
 * six strncat call-site redirects when [game] language = en.
 */
#include <stdint.h>
/* strcpy/strncat -> the BIOS trampolines, as in the matched TU (pc-port
 * objects do not get the force-include) */
#include "sony_bios_alias.h"
#include "types.h"

typedef struct {
    u8 pad0[4];
    u8 status[16];          /* +4 */
    u8 got[13];             /* +20 */
    u8 pad21[5];
    u8 idx;                 /* +38 (0x800F0216) */
    u8 pad27[2097];
    u8 age[16];             /* +2136 */
    u8 month[16];           /* +2152 */
    u8 day[16];             /* +2168 */
    u8 name1[16][8];        /* +2184 */
    u8 name2[16][8];        /* +2312 */
    u8 club[16][16];        /* +2440 */
    u8 pad[1088];
    u8 blood[16];           /* +3784 */
    u8 pad2[6];
    u8 mode;                /* +3806 (0x800F10CE) */
} Info;

typedef struct {
    u8 pad0[517];
    u8 age;                 /* +517 */
    u8 month;               /* +518 */
    u8 day;                 /* +519 */
    u8 pad1[148];
    u8 name1[8];            /* +668 */
    u8 name2[8];            /* +676 */
    u8 club[16];            /* +684 */
    u16 bits;               /* +700 */
    u8 pad2[970];
    u8 got[13];             /* +1672 */
    u8 pad3[2155];
} Chr;                      /* 3840 */

extern Info album_work;
extern Chr album_chr[];
extern u8 album_slot;
extern u8 now_album;
extern u32 album_info_disp_tbl0[];   /* PS1 string pointers */
extern u32 album_info_disp_tbl1[];   /* PS1 string pointers */
#define PS1_STR(t, i) ((u8 *)(uintptr_t)(t)[i])
extern char album_info_disp_str0[];
extern char album_info_disp_str1[];
extern char album_info_disp_str2[];
extern char album_info_disp_str3[];
extern char album_info_disp_str4[];
extern char album_info_disp_str5[];
extern char album_info_disp_str6[];
extern char album_info_disp_str7[];
extern char album_info_disp_str8[];
extern char album_info_disp_str9[];
extern char album_info_disp_str10[];
extern char album_info_disp_str11[];
extern char album_info_disp_str12[];
extern char album_info_disp_str13[];
extern char album_info_disp_str14[];
extern char album_info_disp_str15[];
extern char album_info_disp_str16[];
extern char album_info_disp_str17[];
extern char album_info_disp_str18[];
extern char album_info_disp_str19[];
extern char album_info_disp_str20[];

extern void k_sub_reset();
extern void k_sub_disp_start();
extern void set_kanji_string(s16, s16, s16, u8 *, s32);
extern void strcpy();
extern void strncat();

/* The English patch's call-site redirects in this function
 * (config/patch/english.json main.callsites 0x80069AA8 .. 0x80069FA4):
 * the name and nickname strncats go to the patch engine when English is
 * on (pc/hal/lang.c); Japanese calls strncat exactly as before. */
extern unsigned char g_hal_lang_en __attribute__((weak));   /* weak: harnesses */
extern void pt_name_hold(u8 *, u8 *, s32) __attribute__((weak));
extern void pt_name_put(u8 *, u8 *, s32) __attribute__((weak));
extern void pt_strncat_nick(u8 *, u8 *, s32) __attribute__((weak));
#define EN_CAT(pt, d, s, n) \
    do { if (&g_hal_lang_en && g_hal_lang_en && pt) pt((u8 *)(d), (u8 *)(s), (n)); \
         else strncat((d), (s), (n)); } while (0)

void album_info_disp(a0, a1)
s16 a0;
s32 a1;
{
    s32 n;
    s32 i;
    s32 pos;
    u8 buf200[36];
    u8 buf168[32];
    u8 buf136[32];
    u8 buf104[32];
    u8 buf72[32];
    u8 c;
    u8 d;

    if (a1 != 0) {
        pos = -140;
    } else {
        pos = -8;
    }
    k_sub_reset();
    if (album_work.mode != 0) {
        n = 0;
        for (i = 0; i < 13; i++) {
            if (album_work.got[i] != 0) {
                n++;
            }
        }
        strcpy(buf200, PS1_STR(album_info_disp_tbl0, album_work.idx));
        strncat(buf200, album_info_disp_str0, 2);
        set_kanji_string(pos + 12, -80, 15, buf200, 0);
        if (album_work.idx == 0) {
            set_kanji_string(pos, -64, 15, album_info_disp_str1, 0);
        } else if (album_work.status[album_work.idx] == 0) {
            set_kanji_string(pos, -64, 15, album_info_disp_str2, 0);
        } else if (album_work.status[album_work.idx] == 255) {
            set_kanji_string(pos, -64, 15, album_info_disp_str3, 0);
        } else {
            strcpy(buf200, PS1_STR(album_info_disp_tbl0, album_work.age[album_work.idx] / 10));
            strncat(buf200, PS1_STR(album_info_disp_tbl0, album_work.age[album_work.idx] % 10), 2);
            strncat(buf200, album_info_disp_str4, 2);
            strncat(buf200, PS1_STR(album_info_disp_tbl0, album_work.month[album_work.idx]), 4);
            strncat(buf200, album_info_disp_str5, 2);
            if (album_work.day[album_work.idx] >= 10) {
                strncat(buf200, PS1_STR(album_info_disp_tbl0, album_work.day[album_work.idx] / 10), 2);
            }
            strncat(buf200, PS1_STR(album_info_disp_tbl0, album_work.day[album_work.idx] % 10), 2);
            strncat(buf200, album_info_disp_str6, 2);
            set_kanji_string(pos, -64, 15, buf200, 0);
            strcpy(buf168, album_info_disp_str7);
            EN_CAT(pt_name_hold, buf168, album_work.name1[album_work.idx], 6);
            EN_CAT(pt_name_put, buf168, album_work.name2[album_work.idx], 6);
            strcpy(buf136, album_info_disp_str8);
            EN_CAT(pt_strncat_nick, buf136, album_work.club[album_work.idx], 13);
            set_kanji_string(pos, -48, 15, buf168, 0);
            set_kanji_string(pos, -32, 15, buf136, 0);
            strcpy(buf104, album_info_disp_str9);
            strncat(buf104, PS1_STR(album_info_disp_tbl1, album_work.blood[album_work.idx]));
            set_kanji_string(pos, -16, 15, buf104, 0);
            strcpy(buf72, album_info_disp_str10);
            if (n >= 100) {
                n = 99;
            }
            if (n >= 11) {
                strncat(buf72, PS1_STR(album_info_disp_tbl0, n / 10), 2);
            }
            strncat(buf72, PS1_STR(album_info_disp_tbl0, n % 10), 2);
            set_kanji_string(pos, 0, 15, buf72, 0);
        }
    } else {
        n = 0;
        for (i = 0; i < 13; i++) {
            if (album_chr[album_slot].got[i] != 0) {
                n++;
            }
        }
        if (album_work.status[now_album] == 0) {
            set_kanji_string(pos, -64, 15, album_info_disp_str11, 0);
        } else if (album_work.status[now_album] == 255) {
            set_kanji_string(pos, -64, 15, album_info_disp_str12, 0);
        } else {
            strcpy(buf200, PS1_STR(album_info_disp_tbl0, now_album));
            strncat(buf200, album_info_disp_str13, 2);
            set_kanji_string(pos + 12, -80, 15, buf200, 0);
            strcpy(buf200, PS1_STR(album_info_disp_tbl0, album_chr[album_slot].age / 10));
            strncat(buf200, PS1_STR(album_info_disp_tbl0, album_chr[album_slot].age % 10), 2);
            strncat(buf200, album_info_disp_str14, 2);
            strncat(buf200, PS1_STR(album_info_disp_tbl0, album_chr[album_slot].month), 4);
            strncat(buf200, album_info_disp_str15, 2);
            if (album_chr[album_slot].day >= 10) {
                strncat(buf200, PS1_STR(album_info_disp_tbl0, album_chr[album_slot].day / 10), 2);
            }
            strncat(buf200, PS1_STR(album_info_disp_tbl0, album_chr[album_slot].day % 10), 2);
            strncat(buf200, album_info_disp_str16, 2);
            set_kanji_string(pos, -64, 15, buf200, 0);
            strcpy(buf168, album_info_disp_str17);
            EN_CAT(pt_name_hold, buf168, album_chr[album_slot].name1, 6);
            EN_CAT(pt_name_put, buf168, album_chr[album_slot].name2, 6);
            strcpy(buf136, album_info_disp_str18);
            EN_CAT(pt_strncat_nick, buf136, album_chr[album_slot].club, 13);
            set_kanji_string(pos, -48, 15, buf168, 0);
            set_kanji_string(pos, -32, 15, buf136, 0);
            strcpy(buf104, album_info_disp_str19);
            strncat(buf104, PS1_STR(album_info_disp_tbl1, album_chr[album_slot].bits >> 12));
            set_kanji_string(pos, -16, 15, buf104, 0);
            strcpy(buf72, album_info_disp_str20);
            if (n >= 100) {
                n = 99;
            }
            if (n >= 11) {
                strncat(buf72, PS1_STR(album_info_disp_tbl0, n / 10), 2);
            }
            strncat(buf72, PS1_STR(album_info_disp_tbl0, n % 10), 2);
            set_kanji_string(pos, 0, 15, buf72, 0);
        }
    }
    k_sub_disp_start(6);
}
