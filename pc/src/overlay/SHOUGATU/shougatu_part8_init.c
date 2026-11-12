/* SPDX-License-Identifier: GPL-3.0-or-later */
/* shougatu_part8_init -- PC override of
 * src/overlay/SHOUGATU/shougatu_part8_init.c (0x80137E54).
 *
 * Statement for statement the matched function except the date_name_set
 * call.  date_name_set(girl) copies girl's names (it reads (u8)arg0 < 14);
 * the matched C declares it `(void)` and calls it bare.  The listing sets
 * a0 = 8 (li a0,8 at 0x80137EE8), stores it to girl_no and now_girl and
 * calls jal 0x800849C0 with it, so the console runs date_name_set(eight).
 * On x86-64 the argument was whatever %edi held: another girl's name, or
 * none. pc/tools/audit_ps1_call_args.py SHORT-READ.
 *
 * @DivergesFrom: src/overlay/SHOUGATU/shougatu_part8_init.c -- the argument
 * the console passes in $a0 is written out.
 */
/* shougatu_part8_init - SHOUGATU overlay, Address: 0x80137E54, Size: 284 bytes.
 * (.sym over-sizes this symbol to 304 bytes, absorbing 5 insns of the next
 * unlabeled function past this one's jr ra; windowed back to the real 284.) */
#include "types.h"

extern void prim_init(void);
extern void sub_sub_step_up(void);
extern void draw2d3d(s32, s32);
extern void back_clear_switch(s32);
extern void tpage_buf_clear(void);
extern void class_switch(s32);
extern void class_clear_all(s32);
extern void text_sys_init(void);
extern void hizuke_init(void);
extern void message_window_init(void);
extern void bustup_ptr_init(void);
extern void date_name_set(s32 girl);
extern void yazirushi_off(void);
extern void shared_system_state_reset(void);
extern s32 strcpy(void *, void *);
extern void shougatu_ptab_init(void);

extern u8 bg_change_tbl2;
extern u8 g_name;
extern u8 screen_bri;
extern u8 girl_no;
extern u8 now_girl;
extern u8 vblank_flag;
extern s32 vb_ready;
extern u8 fade_bri;
extern u8 shougatu_part8_init_str0;
extern s32 shougatu_date_serifu_tbl;
extern s32 shougatu_date_serifu_kind_tbl;
extern s32 shougatu_date_sound_tbl;
extern s32 shougatu_date_ss_serifu;
extern s32 shougatu_date_ss_serifu_kind;
extern s32 shougatu_date_sss_sound;
extern u8 shougatu_date_kind;

void shougatu_part8_init(void)
{
    s32 eight;

    draw2d3d(1, 0);
    class_switch(1);
    prim_init();
    class_clear_all(0);
    back_clear_switch(1);
    tpage_buf_clear();
    text_sys_init();
    vblank_flag = 0;
    vb_ready = 1;
    shared_system_state_reset();
    screen_bri = 128;
    fade_bri = 0;
    bg_change_tbl2 = 0;
    bustup_ptr_init();
    hizuke_init();
    message_window_init();
    yazirushi_off();
    eight = 8;
    girl_no = (u8)eight;
    now_girl = (u8)eight;
    date_name_set(eight);
    if ((u32)shougatu_date_kind < 3U) {
        strcpy(&g_name, &shougatu_part8_init_str0);
    }
    shougatu_ptab_init();
    shougatu_date_ss_serifu = shougatu_date_serifu_tbl;
    shougatu_date_ss_serifu_kind = shougatu_date_serifu_kind_tbl;
    shougatu_date_sss_sound = shougatu_date_sound_tbl;
    sub_sub_step_up();
}
