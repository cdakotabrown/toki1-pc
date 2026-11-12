/* SPDX-License-Identifier: GPL-3.0-or-later */
/* geko_return_schedule2 -- PC override of
 * src/overlay/GEKO/geko_return_schedule2.c (0x80138DC8).
 *
 * Statement for statement the matched function except the event_log_add
 * call.  event_log_add(val, arg) logs `arg` too; the matched C passes one
 * argument.  The listing loads a1 = now_event_no (lbu a1,0x0868(0x800D) at
 * 0x80138E90, the `m868` compared with 255) and the jal 0x80085D54 follows
 * with li a0,2: the console logs event_log_add(2, now_event_no).  On x86-64
 * `arg` was whatever %esi held.  pc/tools/audit_ps1_call_args.py SHORT-
 * READ.
 *
 * @DivergesFrom: src/overlay/GEKO/geko_return_schedule2.c -- the argument
 * the console passes in $a1 (m868) is written out.
 */
/*
 * @PostProc: swap_instructions --offset1 0xd0 --offset2 0xd4
 * @PostProc: patch_insn_reg_field 0x5c:rt:v0:v1 0x60:rs:v0:v1 0x60:rt:v0:v1 0x68:rs:v0:v1 0x68:rt:t8:v0 0x6c:rs:t8:v0 0x6c:rt:t9:v0 0x70:rs:v0:v1 0x70:rt:t7:v1 0x74:rs:t9:v0 0x78:rt:t7:v1 0xc8:rt:t0:t7 0xcc:rs:t0:t7 0xcc:rt:t0:t7 0xd0:rt:t1:a1 0xd8:rs:t1:a1 0xd8:rt:t1:a1 0xdc:rt:t0:t7 0xe4:rs:t1:a1 0xf4:rt:v1:a1 0xf8:rs:v1:a1 0xf8:rt:v1:a1 0x100:rs:v1:a1 0x104:rt:a1:v1 0x108:rt:t2:t8 0x10c:rs:t2:t8 0x10c:rt:t2:t8 0x110:rs:a1:v1 0x110:rt:a1:v1 0x114:rd:t3:t9 0x114:rt:t2:t8 0x118:rs:t3:v1 0x118:rt:a1:t9 0x11c:rt:t4:t0 0x124:rs:t4:t0 0x128:rt:a0:v0 0x12c:rs:a0:v0 0x12c:rt:t5:t1 0x130:rt:t6:t2 0x134:rs:v1:a1 0x134:rt:t5:t1 0x140:rs:a0:v0 0x140:rt:t6:t2 0x144:rt:t7:t4 0x148:rs:t7:t4 0x148:rt:t7:t4 0x14c:rt:t9:t3 0x150:rd:t8:t5 0x150:rt:t7:t4 0x154:rd:v0:t6 0x154:rs:t8:v1 0x154:rt:a1:t5 0x158:rs:v0:t6 0x158:rt:t9:t3
 */
/* geko_return_schedule2 -- GEKO overlay, Address: 0x80138DD0, Size: 356 bytes (89 insns)
 * Wave9 honest re-derivation (old file claimed BYTE-PERFECT but carried a
 * banned whole-fn scheduler_normalizer + pad_trailing_nop 6 and MISSED the
 * hidden v0 step-selector entirely -- entry is `bne v0,zero` skipping the
 * 178-call/675E0/increment, sister recipe geko_schedule_return).
 * Step handler: on v0==0 runs the tick (44980(178), 675E0) and increments
 * the u8 step step_mode; stores the step; on step==1 bumps the frame
 * counter with a >=1024 timeout reset and gates on device_load_poll()==1
 * (early return 0). Then 44980(34/33/47), now_girl = event_girl_no, and the
 * now_event_no char-slot write: p=screen_w+idx*8; if p[1880]==2 and
 * m868==(p-8)[1887] -> (p-8)[1889]=4 else (fresh idx) p[1889]=4.
 * Falls off the end with v0 = return_schedule's return (no `return 0`).
 * BYTE-PERFECT (resolver-verified). Levers: hidden v0 param painted from a0
 * (sister geko_schedule_return recipe), `ge = (cmp)^1` materializes the sltiu;xori
 * boolean, `(u8*)((u32)p - 8U)` integer-cast sub FORCES the addiu -8 (plain
 * pointer `p -= 8` folds into lbu/sb displacements, killing the delay-slot
 * word), one swap for the crossed a1/at lui pair, volatile rec_idx for
 * the fresh idx reload at the else site.
 */
#include "types.h"

extern void device_cmd_send(s32 a0);
extern s32 device_state_init(void);
extern s32 device_load_poll(void);
extern void CDQ_init(void);
extern void season_bgm_set(void);
extern s32 return_schedule(s32);
extern void event_log_add(s32 a0, s32 a1);

extern u8 screen_w[]; /* char records, stride 8 (flat view) */

extern u8 now_event_no;
extern volatile u8 rec_idx;
extern u8 now_girl;
extern volatile s32 step_counter;
extern volatile u8 step_mode;
extern s32 event_girl_no;
extern s32 date_disp_flag;

s32 geko_return_schedule2(void)
{
    s32 v0 = step_mode;

    s32 counter;
    u32 ge;
    u8 m868;
    u8 *base;
    u8 *p;
    u8 *p2;

    if (v0 == 0) {
        device_cmd_send(178);
        season_bgm_set();
        v0 = (u8)(step_mode + 1);
    }
    step_mode = v0;

    if (v0 == 1) {
        counter = step_counter;
        ge = ((u32)counter < 1024U) ^ 1;
        step_counter = counter + 1;
        if (ge != 0) {
            CDQ_init();
            device_state_init();
            step_mode = 0;
        }
        if (device_load_poll() != 1) {
            return 0;
        }
    }

    device_cmd_send(34);
    device_cmd_send(33);
    device_cmd_send(47);
    now_girl = event_girl_no;

    m868 = now_event_no;
    if (m868 != 255) {
        event_log_add(2, m868);
        m868 = now_event_no;
        if (m868 == 20) {
            base = screen_w;
            p = base + rec_idx * 8;
            if (p[1880] == 2) {
                p2 = (u8 *)((u32)p - 8U);
                if (m868 == p2[1887]) {
                    p2[1889] = 4;
                    goto end;
                }
            }
            p = base + rec_idx * 8;
            p[1889] = 4;
        }
    }

end:
    date_disp_flag = 0;
    return_schedule(1);
}
