/*
 * zyotai_sprite_set_chk -- PC override of
 * src/main/matched/zyotai_sprite_set_chk.c (main binary).
 *
 * Statement for statement the matched function; the divergence is the
 * pointer WIDTH.  bustup_check_init_tbl0 / tbl1 / tbl2 (0x800D0620 / 24 /
 * 28) are adjacent 4-byte PS1 pointer cells (see bustup_check_init.c); the
 * matched C declares them `s32 *`, 8-byte loads on LP64 that join two
 * cells into a non-canonical address.  Here each cell is read as u32 and
 * widened, as the console reads it.
 *
 * @DivergesFrom: matched zyotai_sprite_set_chk -- pointer read width only
 * (pc/tools/audit_ps1_wide_access.py).
 */
#include <stdint.h>
#include "types.h"

extern volatile u8 now_step;
extern u8  now_sub_sub_step;
extern u8  now_girl;
extern u32 bustup_check_init_tbl0;      /* PS1 pointers */
extern u32 bustup_check_init_tbl1;
extern u32 bustup_check_init_tbl2;
extern s32 bustup_check_init_tbl3;

extern s32  block_checksum_check(s32 a0, void *a1);
extern s32  get_g_zyotai_h(s32 a0);
extern void bustup_sprite_init(s32 a0, s32 a1, s32 a2, s32 a3, s32 sp16);
extern void sub_sub_step_up(void);

s32 zyotai_sprite_set_chk(void)
{
    u32 v0;
    s32 v1;
    s32 masked;
    s32 r;
    s32 *p1 = (s32 *)(uintptr_t)bustup_check_init_tbl0;
    s32 *p2 = (s32 *)(uintptr_t)bustup_check_init_tbl2;
    s32 *p3 = (s32 *)(uintptr_t)bustup_check_init_tbl1;

    if (p1[0] == (s32)0x801820AC && p1[1] == (s32)0x801860AC &&
        p1[2] == (s32)0x8018A0AC && p1[3] == (s32)0x8018E0AC &&
        p1[4] == (s32)0x801920AC &&
        p2[0] == (s32)0x8018005C && p2[1] == (s32)0x80180070 &&
        p3[0] == (s32)0x80180000) {
        goto ok;
    }

    v0 = now_step;
    if (v0 >= 80 && v0 < 112) {
        now_sub_sub_step = now_sub_sub_step - 3;
        sub_sub_step_up();
        return 0;
    }

ok:
    v0 = now_step;
    if (v0 >= 80 && v0 < 112) {
        r = block_checksum_check(45, (void *)0x80180000);
        if (r == 0) {
            sub_sub_step_up();
            now_sub_sub_step = now_sub_sub_step - 3;
            return 0;
        }
    }

    v1 = get_g_zyotai_h(now_girl);
    masked = ((u8)v1) & 0x7F;
    v1 = masked;
    if (!((u32)now_girl < 12)) {
        v1 = 0;
    }
    bustup_sprite_init(bustup_check_init_tbl3, (s32)bustup_check_init_tbl0, (s32)bustup_check_init_tbl1, (s32)bustup_check_init_tbl2, v1);
    sub_sub_step_up();
}
