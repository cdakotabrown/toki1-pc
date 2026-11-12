/*
 * dec_bg_load_check -- PC override of src/main/matched/dec_bg_load_check.c
 * (0x8007EE3C).
 *
 * Statement for statement the matched function; the divergence is the
 * pointer WIDTH.  mdec_bs (0x8012DC54) is a 4-byte PS1 pointer cell, set by
 * dec_env_set to the bitstream buffer 0x80162000; mdec_rl (0x80167000) is
 * the next word.  The matched C declares it `u32 *`, an 8-byte load on
 * LP64, so the check read the header through 0x8016700080162000: a
 * non-canonical address the fault emulation reads as zero.  The MDEC
 * header test (0x3800 magic, version) then failed on every pass and the
 * date's background load retried forever (sub-sub-steps 9 -> 10 -> 11 -> 9
 * of date_main0; date overlay table D_8015BF30[11] = 0x8007EE3C), where
 * retail decodes the park picture and goes on to sub-sub-step 12.  Here the
 * cell is read as u32 and widened, as the console reads it.
 *
 * @DivergesFrom: matched dec_bg_load_check -- pointer read width only
 * (pc/tools/audit_ps1_wide_access.py).
 */
#include <stdint.h>
#include "types.h"

extern u32 mdec_bs;     /* PS1 pointer to the MDEC bitstream */
extern s32 date_effect_init_tbl0;
extern s32 date_effect_init_tbl1;
extern u8 now_sub_sub_step;
extern void dec_bg_init(s32);
extern void sub_sub_step_up(void);

s32 dec_bg_load_check(void) {
    u32 *ptr = (u32 *)(uintptr_t)mdec_bs;
    u32 field0 = *ptr;

    if ((field0 & 0xFFFF0000) != 0x38000000 || (ptr[1] & 0xFF010000) != 0x00010000) {
        date_effect_init_tbl0 = 0;
        date_effect_init_tbl1 = 0;
        sub_sub_step_up();
        now_sub_sub_step -= 3;
        return 0;
    }
    dec_bg_init(1);
    sub_sub_step_up();
}
