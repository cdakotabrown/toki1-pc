/*
 * config_screen_init -- PC override of
 * src/main/matched/config_screen_init.c (0x8006A378).
 *
 * Statement for statement the matched function; the divergence is the
 * RECORD WIDTH (the src/main/pc-port/bg_set.c / mv_state.h doctrine, same
 * defect as src/main/pc-port/basyo_init.c).  The matched Slot
 * declares the class_w slot's three data handles (cels, anims, hdr) as
 * native pointers: 4 bytes on the console, 8 bytes (8-aligned) on LP64.
 * There the 68-byte slot becomes an 88-byte record, so `class_w[n]`
 * strides 88 bytes, every field after +12 moves, and the writes meant
 * for slot 96 and the 32 slots 97..128 (the page sprites)
 * landed across the wrong slots with their handles at host offsets
 * +16/+24/+56.  Measured by an LP64-vs-PS1 slot oracle: the matched code
 * leaves ~1,500 bytes of class_w different from the console image; this
 * override leaves none.
 *
 * Here every slot access goes through the PS1-width ClassSlot
 * (pc/include/game/class_slot.h), fields named by PS1 offset:
 *   layer -> f1, f2..f5 -> f2..f5, rate -> f6, bright -> f7,
 *   cels -> f12, anims -> f16, pad14 -> f20, frame -> f24,
 *   x.w.hi -> f38, y.w.hi -> f42, hdr -> f52, base -> f56.
 * The handle stores zero-extend the .set-aliased PS1 addresses
 * (pc/src/ps1_addr_aliases.c), the value the console stores.  The body
 * below is the matched body with exactly those renames (generated, then
 * diffed against the matched file: no other line differs), and one read
 * width: card_file_save_tbl0 (0x800B97B8) is a 4-byte PS1 pointer to a
 * table of 4-byte PS1 string pointers.  As `u8 **` it loaded 8 bytes
 * (0xB292CC91800B9778, non-canonical) and indexed 8-byte entries; here
 * CARD_FILE_NAME() reads both levels as u32, as the console does
 * (pc/tools/audit_ps1_wide_access.py; the matched C has the same read).
 * config_screen_init_tbl3 (0x800BCBA4), the message per setting, is a
 * table of 4-byte PS1 pointers too: `u8 *[]` read entry sel at stride 8
 * (two messages joined); it is read as u32 (WIDE-INDEX).
 *
 * @DivergesFrom: matched config_screen_init -- record field and
 * card_file_save_tbl0 / config_screen_init_tbl3 read widths only.
 */
#include <stdint.h>
#include "types.h"
#include "class_slot.h"

typedef union {
    s32 l;
    struct { u16 lo; s16 hi; } w;
} Fix;

extern ClassSlot class_w[];
extern u8 now_year;
extern u8 now_month;
extern u8 basyo_flag1;
extern u8 basyo_flag3;
extern u8 basyo_flag5;
extern u8 basyo_flag6;
extern u8 basyo_flag7;
extern u8 basyo_flaga;
extern u8 basyo_flagb;
extern u8 basyo_flagd;
extern u32 basyo_init_tbl1;
extern u32 basyo_init_tbl2;
extern u32 _sys_default_tpage_set_tbl2;
extern s16 basyo_init_tbl3;
extern u8 config_screen_init_str2[];
extern u8 config_screen_init_str3[];
extern u8 config_screen_init_str4[];
extern u8 config_screen_init_str5[];
extern u8 config_screen_init_str6[];
extern u8 config_screen_init_str7[];
extern u8 config_screen_init_tbl0[];
extern u32 card_file_save_tbl0;   /* PS1 pointer to PS1 string pointers */
#define CARD_FILE_NAME(i) \
    ((u8 *)(uintptr_t)((u32 *)(uintptr_t)card_file_save_tbl0)[i])
extern u8 config_screen_init_tbl2[];
extern u8 config_screen_init_tbl1[];
extern u8 config_screen_init_str1[];
extern u8 config_screen_init_str0[];
extern u8 config_screen_init_str8[];
extern u32 config_screen_init_tbl3[];    /* PS1 message pointers */
extern u8 p_sei[];

void config_screen_init()
{
    s32 c0;
    s32 i;
    s32 n;
    s32 c1;
    s32 sel;

    class_clear_f();
    class_clear_n(96);
    class_w[96].f1 = 5;
    class_w[96].f2 = 64;
    class_w[96].f56 = 0x40000000;
    class_w[96].f3 = 132;
    class_w[96].f12 = CLASS_SLOT_HANDLE(&basyo_init_tbl1);
    class_w[96].f16 = CLASS_SLOT_HANDLE(&basyo_init_tbl2);
    class_w[96].f52 = CLASS_SLOT_HANDLE(&_sys_default_tpage_set_tbl2);
    class_w[96].f20 = basyo_init_tbl3;
    class_w[96].f4 = 8;
    class_w[96].f5 = 1;
    class_w[96].f24 = 118;
    class_w[96].f38 = 0;
    class_w[96].f42 = -40;
    for (i = 0; i < 32; i++) {
        class_clear_n(i + 97);
        class_w[i + 97].f1 = 4;
        class_w[i + 97].f2 = 64;
        class_w[i + 97].f56 = 0;
        class_w[i + 97].f3 = 132;
        class_w[i + 97].f12 = CLASS_SLOT_HANDLE(&basyo_init_tbl1);
        class_w[i + 97].f16 = CLASS_SLOT_HANDLE(&basyo_init_tbl2);
        class_w[i + 97].f52 = CLASS_SLOT_HANDLE(&_sys_default_tpage_set_tbl2);
        class_w[i + 97].f20 = basyo_init_tbl3;
        class_w[i + 97].f4 = 8;
        class_w[i + 97].f5 = 1;
        class_w[i + 97].f6 = 8;
        class_w[i + 97].f24 = 0;
        class_w[i + 97].f38 = 256;
        class_w[i + 97].f42 = 256;
        class_w[i + 97].f7 = 128;
    }
    n = 97;
    switch (now_month) {
    case 2:
    case 3:
    case 4:
        if (now_year == 95) {
            sel = 0;
            class_w[n].f24 = 121;
            class_w[n].f38 = 0;
            class_w[n].f42 = -40;
            n++;
            set_kanji_string(-158, -60, 3, config_screen_init_str2, 0);
            set_kanji_string(-158, -44, 3, config_screen_init_str3, 0);
        }
        if (now_year == 96) {
            sel = 4;
            if (now_month != 2) {
                basyo_flagd = 1;
            }
            class_w[n].f24 = 122;
            class_w[n].f38 = 0;
            class_w[n].f42 = -40;
            n++;
            class_w[n].f24 = 129;
            class_w[n].f38 = 8;
            class_w[n].f42 = -40;
            n++;
        }
        if (now_year == 97) {
            sel = 8;
            if (now_month != 2) {
                basyo_flagb = 1;
            }
            class_w[n].f24 = 122;
            class_w[n].f38 = 0;
            class_w[n].f42 = -40;
            n++;
            class_w[n].f24 = 132; class_w[n].f38 = 8; class_w[n].f42 = -40;
            n++;
        }
        if (now_year == 98) {
            sel = 12;
            set_kanji_string(-158, -60, 3, config_screen_init_str6, 0);
            set_kanji_string(-158, -44, 3, config_screen_init_str7, 0);
        }
        break;
    case 5:
    case 6:
    case 7:
        class_w[n].f24 = 123;
        class_w[n].f38 = 0;
        class_w[n].f42 = -40;
        n++;
        if (now_year == 95) {
            sel = 1;
            if (now_month != 5) {
                basyo_flag1 = 1;
            }
            class_w[n].f24 = 126;
            class_w[n].f38 = 8;
            class_w[n].f42 = -40;
            n++;
        }
        if (now_year == 96) {
            sel = 5;
            if (now_month != 5) {
                basyo_flag5 = 1;
            }
            class_w[n].f24 = 130; class_w[n].f38 = 8; class_w[n].f42 = -40;
            n++;
        }
        if (now_year == 97) {
            sel = 9;
            set_kanji_string(-158, -60, 3, config_screen_init_str4, 0);
            set_kanji_string(-158, -44, 3, config_screen_init_str5, 0);
        }
        break;
    case 8:
    case 9:
    case 10:
        class_w[n].f24 = 124;
        class_w[n].f38 = 0;
        class_w[n].f42 = -40;
        n++;
        if (now_year == 95) {
            sel = 2;
            if ((now_month | 0) != 8) {
                basyo_flag7 = 1;
            }
            class_w[n].f24 = 127;
            class_w[n].f38 = 8;
            class_w[n].f42 = -40;
            n++;
        }
        if (now_year == 96) {
            sel = 6;
            if ((now_month | 0) != 8) {
                basyo_flaga = 1;
            }
            class_w[n].f24 = 131; class_w[n].f38 = 8; class_w[n].f42 = -40;
            n++;
        }
        if (now_year == 97) {
            sel = 10;
            set_kanji_string(-158, -60, 3, config_screen_init_str4, 0);
            set_kanji_string(-158, -44, 3, config_screen_init_str5, 0);
        }
        break;
    case 11:
    case 12:
        class_w[n].f24 = 125;
        class_w[n].f38 = 0;
        class_w[n].f42 = -40;
        n++;
        if (now_year == 95) {
            sel = 3;
            if (now_month == 12) {
                basyo_flag3 = 1;
            }
            class_w[n].f24 = 128;
            class_w[n].f38 = 8;
            class_w[n].f42 = -40;
            n++;
        }
        if (now_year == 96) {
            sel = 7;
            if (now_month == 12) {
                basyo_flag6 = 1;
            }
            class_w[n].f24 = 133; class_w[n].f38 = 8; class_w[n].f42 = -40;
            n++;
        }
        if (now_year == 97) {
            sel = 11;
            set_kanji_string(-158, -60, 3, config_screen_init_str6, 0);
            set_kanji_string(-158, -44, 3, config_screen_init_str7, 0);
        }
        break;
    case 1:
        class_w[n].f24 = 125;
        class_w[n].f38 = 0;
        class_w[n].f42 = -40;
        n++;
        if (now_year == 96) {
            sel = 3;
            basyo_flag3 = 1;
            class_w[n].f24 = 128;
            class_w[n].f38 = 8;
            class_w[n].f42 = -40;
            n++;
        }
        if (now_year == 97) {
            sel = 7;
            basyo_flag6 = 1;
            class_w[n].f24 = 133; class_w[n].f38 = 8; class_w[n].f42 = -40;
            n++;
        }
        if (now_year == 98) {
            sel = 11;
            set_kanji_string(-158, -60, 3, config_screen_init_str6, 0);
            set_kanji_string(-158, -44, 3, config_screen_init_str7, 0);
        }
        break;
    }
    if (now_year != 95 || now_month >= 6) {
        class_w[n].f24 = 119;
        class_w[n].f38 = 0;
        class_w[n].f42 = -40;
        n++;
    }
    if (now_year == 95) {
        class_w[n].f38 = 0;
        class_w[n].f24 = sel + 151;
        class_w[n].f42 = -40;
        n++;
    } else if (now_year == 96 && now_month < 2) {
        class_w[n].f38 = 0;
        class_w[n].f24 = sel + 151;
        class_w[n].f42 = -40;
        n++;
    } else {
        class_w[n].f24 = 120;
        class_w[n].f1++;
        c0 = config_screen_init_tbl0[sel * 3];
        c1 = config_screen_init_tbl0[sel * 3 + 1];
        class_w[n + 1].f42 = 24;
        class_w[n + 1].f24 = c0 + 144;
        class_w[n].f38 = 0;
        class_w[n].f42 = -40;
        class_w[n + 1].f38 = 272;
        class_w[n + 2].f24 = c1 + 144;
        class_w[n + 2].f42 = 40;
        class_w[n + 2].f38 = 272;
        class_w[n + 3].f24 = config_screen_init_tbl0[sel * 3 + 2] + 144;
        class_w[n + 3].f42 = 56;
        class_w[n + 3].f38 = 272;
        n += 4;
        set_kanji_string(256, 16, 6, CARD_FILE_NAME((sel * 3 + 2) % 12), 0);
        set_kanji_string(256, 32, 6, CARD_FILE_NAME((sel * 3 + 3) % 12), 0);
        set_kanji_string(256, 48, 6, CARD_FILE_NAME((sel * 3 + 4) % 12), 0);
    }
    if (config_screen_init_tbl2[sel] != 255) {
        class_w[n].f24 = config_screen_init_tbl2[sel];
        class_w[n].f38 = 0;
        class_w[n].f42 = -40;
        n++;
    }
    set_kanji_string(32, -94, 7, &config_screen_init_str1[sel * 17], 0);
    if (config_screen_init_tbl1[sel] != 255) {
        class_w[n].f42 = -40;
        class_w[n].f24 = config_screen_init_tbl1[sel];
        class_w[n].f38 = 0;
        n++;
    }
    set_kanji_string(32, -62, 8, &config_screen_init_str0[sel * 17], 0);
    if (sel < 4) {
        class_w[n].f24 = (sel & 3) + 151;
        class_w[n].f42 = -40;
        class_w[n].f38 = 0;
        n++;
    }
    if (sel >= 4) {
        class_w[n].f24 = (sel & 3) + 147;
        class_w[n].f42 = -40;
        class_w[n].f38 = 0;
    }
    set_kanji_string(-143, -14, 6, &config_screen_init_str8[sel * 108], 0);
    set_kanji_string(-143, 2, 6, &config_screen_init_str8[sel * 108 + 36], 0);
    set_kanji_string(-143, 18, 6, &config_screen_init_str8[sel * 108 + 72], 0);
    k_sub_reset_point_set();
    k_sub_disp_start(0);
    gnsx(p_sei);
    sndisp((u8 *)(uintptr_t)config_screen_init_tbl3[sel], 0, 31);
}
