/* @DivergesFrom: src/main/matched/serifu_main.c
 * Dialogue globals hold four-byte PS1 addresses; widen only after loading.
 * Script interpretation and dialogue state transitions retain retail order.
 */
#include "types.h"

extern s32 check_end_k(void);
extern s32 get_CD_State(void);
extern void device_cmd_send(s32 arg);
extern void k_reset(s32 arg);
extern void mes_arrow_anim(void);
extern s32 serifu_cmd_exec(s32, s32, s32, s32, s32, s32, s32);
extern void bustup_speech(void);
extern void bustup_flag_on(void);

extern s32 serifu_wait;
extern s32 serifu_voice;
extern s32 msg_wait;
extern s32 serifu_state;
extern u8 voice_level;
extern s32 pad_trig;
extern s16 sound_seek_time;
extern u8 xcut;
extern u8 yazirushi_cw_disp;
extern s32 yajirusi_timer;
extern s32 step_counter;

extern u32 now_s;
#define now_s ((s16 *)(unsigned long)now_s)
extern u32 now_s_in;
#define now_s_in ((s16 *)(unsigned long)now_s_in)
extern u32 ss_s;
#define ss_s ((s32 *)(unsigned long)ss_s)
extern u32 ss_s_kind;
#define ss_s_kind ((s32 *)(unsigned long)ss_s_kind)
extern s32 sss_so;
extern u8 sound_seek_wait;

extern s32 k_wait_cnt;
extern s32 bustup_on;
extern s32 mugon_flag;
extern s32 kuchi_stop;
extern s32 date_sprite_on;
extern s32 bustup_mode;

s32 serifu_main(u8 arg0, u8 arg1, u8 arg2)
{
    s32 state;
    s32 result;

    if (serifu_wait != 0) {
        if (check_end_k() != 0 &&
            (serifu_voice == 0 ||
             (serifu_voice == 1 && get_CD_State() != 4) ||
             voice_level == 0)) {
            serifu_wait = 0;
            sound_seek_time = 0;
        } else if ((pad_trig & 0x400040) != 0 && voice_level == 2) {
            device_cmd_send(180);
            serifu_wait = 2;
            sound_seek_time = 0;
            xcut = 1;
        }

        if (check_end_k() != 0 && serifu_wait == 2 && get_CD_State() != 4) {
            serifu_wait = 0;
            sound_seek_time = 0;
        }
        goto state_machine;
    }

    if (msg_wait != 0) {
        if (check_end_k() != 0) {
            state = serifu_voice;
            if (state == 0 || (state == 1 && get_CD_State() != 4) || voice_level == 0) {
                k_reset(1);
                msg_wait = 0;
                sound_seek_time = 0;
            }
        }
        goto state_machine;
    }

    if (serifu_state != 0) {
        if ((pad_trig & 0x400040) != 0 && voice_level == 2) {
            device_cmd_send(180);
            xcut = 1;
            serifu_state = 2;
            sound_seek_time = 0;
        }

        if (check_end_k() != 0 && serifu_state == 2 && get_CD_State() != 4) {
            k_reset(1);
            serifu_state = 0;
            yazirushi_cw_disp = yazirushi_cw_disp & 0xff7f;
            sound_seek_time = 0;
            serifu_voice = 0;
            return 1;
        }

        if ((pad_trig & 0x200020) != 0 && check_end_k() != 0) {
            state = serifu_voice;
            if (state == 0 || state == 10 || (state == 1 && get_CD_State() != 4) || voice_level == 0) {
                k_reset(1);
                serifu_state = 0;
                yazirushi_cw_disp = yazirushi_cw_disp & 0xff7f;
                sound_seek_time = 0;
                serifu_voice = 0;
                return 1;
            }
        }

        if (check_end_k() != 0) {
            state = serifu_voice;
            if (state == 0 || state == 10 ||
                (state == 1 && get_CD_State() != 4) || voice_level == 0) {
                yazirushi_cw_disp = yazirushi_cw_disp | 0x80;
                mes_arrow_anim();
                goto state_machine;
            }
        }
        yazirushi_cw_disp = yazirushi_cw_disp & 0x7F;
        goto state_machine;
    }

    {
        s16 *ptr1;
        s16 *ptr2;
        s32 *ptr3;
        s32 *ptr4;
        s16 idx1;
        s16 idx2;

        yajirusi_timer = 0;
        ptr1 = now_s;
        step_counter = 0;
        idx1 = ptr1[0];
        ptr2 = now_s_in;
        ptr3 = ss_s;
        ptr4 = ss_s_kind;
        idx2 = ptr2[0];

        result = serifu_cmd_exec(
            ((s32 *)(u32)(ptr3[idx1]))[idx2],
            ((u8 *)(u32)(ptr4[idx1]))[idx2],
            arg0,
            sss_so,
            idx1,
            idx2,
            arg2
        );

        if (arg1 != 0 && sound_seek_wait == 0 && (result & 0xff) != 100) {
            s16 *p;
            s32 *q;
            s16 v1_val;
            s16 v0_val;
            s32 t1_val;

            p = now_s;
            v1_val = p[0];
            q = ss_s_kind;
            v0_val = ((s16 *)now_s_in)[0];
            t1_val = q[v1_val];

            if (*((u8 *)(u32)(v0_val + t1_val)) == 255) {
                p[0] = v1_val + 1;
                ((s16 *)now_s_in)[0] = 0;
            } else {
                ((s16 *)now_s_in)[0] = (s16)(v0_val + 1);
            }
        }
    }

state_machine:
    if (serifu_wait != 0) goto do_func;
    if (serifu_state != 0) goto do_func;
    if (msg_wait == 0) goto done;
do_func:
    if (check_end_k() != 0) goto done;
    if (voice_level != 0) goto done;

    {
        s16 *p;
        s32 *q;
        s16 *r;
        s16 v1_val;
        s16 v0_val;
        s32 t1_val;
        u8 v0_byte;

        p = now_s;
        q = ss_s_kind;
        r = now_s_in;
        v1_val = p[0];
        v0_val = r[0];
        t1_val = q[v1_val];
        v0_byte = *(((u8 *)(u32)(v0_val + t1_val)) - 1);

        if (v0_byte != 1 && v0_byte != 2 && v0_byte != 5 &&
            v0_byte != 10 && v0_byte != 11 && v0_byte != 12 && v0_byte != 13) {
            goto done;
        }
    }

check_state:
    if (k_wait_cnt != 0) goto done;
    if (bustup_on == 0) goto call_67f2c;
    if (mugon_flag != 0) goto call_67f2c;
    if (kuchi_stop != 0) goto call_67f2c;
    bustup_speech();
    goto done;

call_67f2c:
    if (date_sprite_on == 0) goto done;
    if (bustup_mode != 0) goto done;
    if (mugon_flag != 0) goto done;
    bustup_flag_on();

done:
    return 0;
}

/* 2026-08-23 Hook 18 PRECOLOR_WEB sweep (main): every bitpos
 * IDO52_GC_COLOR_TRACE reports COLOURED (first 12) x v0/v1/a0-a3/t0-t9/s0-s7/fp,
 * accepted only when the target word count, the COMPILED length and the harness
 * `Fixup:` count all held.  `IDO52_PRECOLOR_WEB=t5:173`
 * takes exact 338/381 -> 346.
 * Hook 37 FORCE_WEB_COLOR cannot substitute -- it refuses any colour in the
 * web's own forbidden bitvector, which is exactly the case to override.
 */
