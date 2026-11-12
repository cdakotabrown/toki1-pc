/* SPDX-License-Identifier: GPL-3.0-or-later */
/* lang_patch_entries.c -- English entry points of the routed patch hooks.
 *
 * docs/pc-port/TRANSLATION-ON-PC.md.  The generated stubs
 * (pc/tools/gen_lang_hooks.py -> pc/build/translation/gen/lang_hooks.c)
 * send a call of a hooked retail function here when g_hal_lang_en is set,
 * exactly where the console's patched executable has `j <patch function>`.
 * Each entry takes the retail function's arguments as the native callers
 * pass them (matched C passes PS1 string addresses as 32-bit words in
 * places) and narrows the pointer arguments with hal_ps1_ptr before the
 * patch function (src/patch/main, compiled natively) runs.
 *
 * The patch functions' own names: set_kanji_string / load_string /
 * disp_string are compiled as pt_set_kanji_string / pt_load_string /
 * pt_disp_string (pc/Makefile PATCH_CFLAGS), the rest keep their pt_ names.
 */
#include <stdint.h>
#include "types.h"
#include "hal.h"

#define P(x) ((u8 *)hal_ps1_ptr((const void *)(uintptr_t)(x)))

typedef long A;     /* one argument register, as the caller left it */

extern s16  pt_set_kanji_string(s16, s16, u8, u8 *, s32);
extern s32  pt_load_string(u8 *);
extern void pt_disp_string(void);
extern s32  pt_k_disp_inc(void);
extern s32  pt_schedule_analyze(s16, s16, u8, u8 *, s32, u8 *, u8 *, u8);
extern void pt_choice_shuffle(u8 *);
extern void pt_field_message(u8 *, s32, s32);
extern s32  pt_field_line(u8 *, s32, s32);
extern void pt_speaker_label(u8 *);
extern s32  pt_event_message(u8 *, s32, s32, void *, u16, u16, u8);
extern void pt_basyo_init(u8 *);
extern void pt_vacation_day_init1(u8);
extern void pt_telephone_choice_menu_init(void);
extern void pt_menu2_init(void);
extern void pt_menu3_init(void);
extern void pt_get_p_name(u8 *, s32);
extern u8  *pt_strcpy(u8 *, u8 *);
extern u8  *pt_strcat(u8 *, u8 *);
extern u8  *pt_strncat(u8 *, u8 *, s32);

s32 lang_en_set_kanji_string(A x, A y, A attr, A str, A ot)
{
    return pt_set_kanji_string((s16)x, (s16)y, (u8)attr, P(str), (s32)ot);
}

s32 lang_en_load_string(A str) { return pt_load_string(P(str)); }
void lang_en_disp_string(void) { pt_disp_string(); }
s32 lang_en_k_disp_inc(void) { return pt_k_disp_inc(); }

s32 lang_en_msg_str_disp(A x, A y, A color, A str, A a4, A suffix, A bullet, A show)
{
    return pt_schedule_analyze((s16)x, (s16)y, (u8)color, P(str), (s32)a4,
                               P(suffix), P(bullet), (u8)show);
}

void lang_en_msg_part_reorder(A s) { pt_choice_shuffle(P(s)); }
void lang_en_sndisp(A s, A y, A kind) { pt_field_message(P(s), (s32)y, (s32)kind); }
s32 lang_en_sndi(A s, A mode, A kind) { return pt_field_line(P(s), (s32)mode, (s32)kind); }
void lang_en_gnsx(A name) { pt_speaker_label(P(name)); }

s32 lang_en_serifu_cmd_exec(A str, A sel, A a2, A voice, A i1, A i2, A color)
{
    return pt_event_message(P(str), (s32)sel, (s32)a2, (void *)P(voice),
                            (u16)i1, (u16)i2, (u8)color);
}

void lang_en_basyo_init(A s) { pt_basyo_init(P(s)); }
void lang_en_girl_menu_set(A sel) { pt_vacation_day_init1((u8)sel); }
s32 lang_en_telephone_choice_menu_init(void) { pt_telephone_choice_menu_init(); return 0; }
s32 lang_en_menu2_init(void) { pt_menu2_init(); return 0; }
s32 lang_en_menu3_init(void) { pt_menu3_init(); return 0; }
void lang_en_get_p_name(A dst, A kind) { pt_get_p_name(P(dst), (s32)kind); }

char *lang_en_func_800B4890(A dst, A src) { return (char *)pt_strcpy(P(dst), P(src)); }
char *lang_en_func_800B48A0(A dst, A src) { return (char *)pt_strcat(P(dst), P(src)); }
char *lang_en_func_800B48B0(A dst, A src, A n) { return (char *)pt_strncat(P(dst), P(src), (s32)n); }

/* ---- overlay hooks (the <sym>__langhook stubs, pc/tools/gen_lang_hooks.py
 * --ovl-filter) and the ETC exam-board call site ---- */
extern void pt_name_page_latin(u8 *);
extern void pt_name_band(void);
extern void pt_name_key(s32);
extern void pt_name_cursor(s32);
extern void pt_name_commit(void);
extern void pt_shougatu_greeting(void);
extern void pt_bunka_sd_rollcall(void);
extern void pt_bunka_sd_announcer(void);
extern void pt_bunkasai_photo(void);
extern void pt_date2_lyrics(s16);
extern void pt_date2_song_menu(void);
extern void pt_ending_song(s16);
extern void pt_full_name_cat(u8 *, u8 *, s32);

s32 lang_en_ovl_NAME_ENT_801330E8(A title) { pt_name_page_latin(P(title)); return 0; }
s32 lang_en_ovl_NAME_ENT_80133E60(void) { pt_name_band(); return 0; }
s32 lang_en_ovl_NAME_ENT_80134254(A sel) { pt_name_key((s32)sel); return 0; }
s32 lang_en_ovl_NAME_ENT_80132B78(A mode) { pt_name_cursor((s32)mode); return 0; }
s32 lang_en_ovl_NAME_ENT_80138D08(void) { pt_name_commit(); return 0; }
s32 lang_en_ovl_SHOUGATU_80133B48(void) { pt_shougatu_greeting(); return 0; }
s32 lang_en_ovl_BUNKA_SD_8013320C(void) { pt_bunka_sd_rollcall(); return 0; }
s32 lang_en_ovl_BUNKA_SD_80132FF8(void) { pt_bunka_sd_announcer(); return 0; }
s32 lang_en_ovl_BUNKASAI_801527B0(void) { pt_bunkasai_photo(); return 0; }
s32 lang_en_ovl_DATE2_801395A8(A n) { pt_date2_lyrics((s16)n); return 0; }
s32 lang_en_ovl_DATE2_801385CC(void) { pt_date2_song_menu(); return 0; }
s32 lang_en_ovl_ENDING_8013BE20(A idx) { pt_ending_song((s16)idx); return 0; }
s32 lang_en_lang_ovlsite_ETC_80147BBC(A dst, A src, A n)
{
    pt_full_name_cat(P(dst), P(src), (s32)n);
    return 0;
}
