/*
 * holiday_screen_init -- PC override of src/main/matched/holiday_screen_init.c
 * (0x80075674).
 *
 * Statement for statement the matched function; the divergence is the
 * table WIDTH.  basyo_bg_load_tbl0 (0x800BBE60) holds the holiday room's
 * background key per season, 4-byte words (retail: 0x3FBE spring, 0x3FC6
 * summer, 0x3FAD autumn, 0x3FB6 winter; basyo_bg_load / holiday_bg_init /
 * vacation_day_bg_load declare it s32[]).  The matched C declares it
 * `void *[]`, so season i is read 8 bytes wide at stride 8 and truncated
 * to the s32 parameter: spring reads 0x3FBE (right by luck), summer the
 * autumn key 0x3FAD, autumn and winter the words after the table
 * (0x01000019, 0x03000079) -- value_cache_find_or_load would then queue a
 * read of that many bytes.  Here the table is s32[], as the console reads
 * it.  (Reached by every holiday screen: audit_ps1_wide_access.py
 * WIDE-INDEX, reach list of the date_save / new_game playthroughs.)
 *
 * @DivergesFrom: matched holiday_screen_init -- table read width only.
 */
#include "types.h"
/* strcpy/strcat/... -> the BIOS trampolines, as in the matched TU
 * (pc-port objects do not get the force-include) */
#include "sony_bios_alias.h"

extern u16 byouki_state;
extern u8 byouki_kind;
extern u8 now_year;
extern u8 now_month;
extern u8 now_day;
extern u8 now_youbi;
extern u8 icon_num;
extern u8 now_icon;
extern s32 reserved_date;
extern s32 work_tarao;
extern s32 event_girl_no;
extern u8 p_sei;

extern void hizuke_init(void);
extern s32 fujisaki_2nin_birth_check(void);
extern void goto_step(s32);
extern void prim_init(void);
extern void back_clear_switch(s32);
extern void tpage_buf_clear(void);
extern void text_sys_init(void);
extern void basyo_init2(void *);
extern void k_disp_start(s32);
extern void icon_init(void);
extern void icon_menu_set(u8);
extern void icon_can_use_init(void);
extern void gnsx(void *);
extern void icon_can_use_clear(s32);
extern void icon_can_use_set(s32, s32);
extern void sndisp(void *, s32, s32);
extern s32 reserved_date_check(u8, u8, u8);
extern void strcpy(void *, void *);
extern void get_g_name(void *, s32);
extern void strcat(void *, void *);
extern s32 basyo_range_check(void);
extern void set_kanji_string(s32, s32, s32, void *, s32);
extern void class_clear_f(void);
extern void cursor_init(s32);
extern s32 parameter_show_init(void);
extern s32 message_window_init(void);
extern s32 get_season(void);
extern s32 value_cache_find_or_load(void *, s32);
extern void set_dec_bri(s32);
extern void indexed_byte_lsb_set(s32, s32);
extern void sub_step_up(void);
extern void sub_sub_step_up(void);

extern s32 holiday_screen_init_str0;
extern s32 holiday_screen_init_str1;
extern s32 holiday_screen_init_str2;
extern s32 holiday_screen_init_str3;
extern s32 holiday_screen_init_str4;
extern s32 holiday_screen_init_str5;
extern s32 holiday_screen_init_str6;
extern s32 holiday_screen_init_str7;
extern s32 holiday_screen_init_str8;
extern s32 holiday_screen_init_str9;
extern s32 holiday_screen_init_str10;
extern s32 holiday_screen_init_str11;
extern s32 basyo_bg_load_tbl0[];    /* 4-byte background keys, one per season */
extern u8 tel_code_check_tbl0;
/* Defined in this TU (initialised): as1 shares one `lui at` across the
 * three remainder-iteration stores only for an initialised same-TU symbol.
 * The PC port has its own definition (pc/src/globals.c). */
#ifdef PC_PORT
extern u8 holiday_screen_init_tbl0[];
#else
u8 holiday_screen_init_tbl0[8] = { 0 };
#endif

void holiday_screen_init(void) {
    s32 i;
    s32 result;
    s32 sp15C;
    s32 sp88[65];
    s32 sp56[8];

    tel_code_check_tbl0 = 0;
    for (i = 0; i < 7; i++) holiday_screen_init_tbl0[i] = 255;

    hizuke_init();

    if (fujisaki_2nin_birth_check()) {
        event_girl_no = 0;
        work_tarao = 8;
        goto_step(93);
        return;
    }

    prim_init();
    back_clear_switch(1);
    tpage_buf_clear();
    text_sys_init();
    basyo_init2(&holiday_screen_init_str0);
    k_disp_start(1);
    icon_num = 14;
    icon_init();
    icon_menu_set(icon_num);
    icon_can_use_init();
    gnsx(&p_sei);

    if (byouki_state & 1) {
        icon_num = 14;
        icon_init();
        icon_menu_set(icon_num);
        icon_can_use_clear(1);
        icon_can_use_set(13, 0);
        icon_can_use_set(0, 1);
        icon_can_use_set(1, 1);
        icon_can_use_set(2, 1);
        sndisp(&holiday_screen_init_str1, 0, 31);
    } else if ((byouki_kind & 0xF) == 3) {
        icon_num = 14;
        icon_init();
        icon_menu_set(icon_num);
        icon_can_use_clear(1);
        icon_can_use_set(13, 0);
        icon_can_use_set(7, 1);
        now_icon = 7;
        sndisp(&holiday_screen_init_str2, 0, 31);
    } else {
        sp15C = 0;
        if (reserved_date_check(now_year, now_month, now_day) > 0) {
            strcpy(sp88, &holiday_screen_init_str3);
            get_g_name(sp56, ((u32)reserved_date << 3) >> 28);
            strcat(sp88, sp56);
            strcat(sp88, &holiday_screen_init_str4);
            sndisp(sp88, 0, 31);
            now_icon = 9;
            sp15C = 1;
        }

        if (basyo_range_check() == 1) {
            if ((u32)(now_day + 6) / 7u == 3) {
                if (now_youbi == 6) {
                    if (now_month % 3u == 0) {
                        if (sp15C != 0) {
                            strcpy(sp88, &holiday_screen_init_str5);
                            strcat(sp88, &holiday_screen_init_str6);
                            set_kanji_string(-72, 66, 0, sp88, 0);
                        } else {
                            strcpy(sp88, &holiday_screen_init_str7);
                            sndisp(sp88, sp15C, 31);
                        }
                        sp15C++;
                    } else {
                        if (sp15C != 0) {
                            strcpy(sp88, &holiday_screen_init_str8);
                            strcat(sp88, &holiday_screen_init_str9);
                            set_kanji_string(-72, 66, 0, sp88, 0);
                        } else {
                            strcpy(sp88, &holiday_screen_init_str10);
                            sndisp(sp88, sp15C, 31);
                        }
                        sp15C++;
                    }
                    now_icon = 4;
                }
            }
        }

        if (sp15C == 0) {
            sndisp(&holiday_screen_init_str11, 0, 31);
        }
    }

    k_disp_start(5);
    class_clear_f();
    cursor_init(0);
    parameter_show_init();
    parameter_show_init();
    message_window_init();
    result = get_season();
    result = value_cache_find_or_load((void *)(u32)basyo_bg_load_tbl0[result], 0);

    if (result == -1) {
        goto neg_one;
    }
    if (result == 0 || result == 1) {
        set_dec_bri(128);
        indexed_byte_lsb_set(0, result);
        sub_step_up();
    }
    goto done;
neg_one:
    sub_sub_step_up();
done:
    ;
}
