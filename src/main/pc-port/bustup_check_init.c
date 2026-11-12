/*
 * bustup_check_init -- PC override of src/main/matched/bustup_check_init.c
 * (main binary).
 *
 * Statement for statement the matched function; the divergence is the
 * pointer WIDTH.  bustup_check_init_tbl0 / tbl1 / tbl2 (0x800D0620 / 24 /
 * 28) are adjacent 4-byte PS1 pointer cells into the bust-up archive just
 * read to 0x80180000 (bustup_cd_read).  The matched C declares tbl0
 * `u32 *`, an 8-byte load on LP64: (tbl1 << 32) | tbl0, a non-canonical
 * address, and the port faulted here as soon as the date's first bust-up
 * loaded (date_main0's handler table, D_8015BF30[37] = 0x800838A8).  Here
 * the cell is read as u32 and widened, as the console reads it.
 *
 * @DivergesFrom: matched bustup_check_init -- pointer read width only
 * (pc/tools/audit_ps1_wide_access.py).
 */
#include <stdint.h>
#include "types.h"

extern u8 now_sub_sub_step;
extern u8 now_girl;
extern s32 bustup_check_init_tbl3;
extern u32 bustup_check_init_tbl0;      /* PS1 pointer */
extern s32 bustup_check_init_tbl1;
extern s32 bustup_check_init_tbl2;
extern u8 bustup2_cw_bright;
extern u32 bustup2_cw_attr;
extern u32 bustup_cw_attr;
extern u32 Sprites;
extern u32 sprite1_attribute;

extern s32 block_checksum_check(s32 a0, u32 a1);
extern s32 sub_sub_step_up(void);
extern s32 get_g_zyotai_h(u8 a0);
extern void bustup_sprite_init(s32 a0, s32 a1, s32 a2, s32 a3, s32 a4);

s32 bustup_check_init(void) {
    s32 result;
    u32 *ptr = (u32 *)(uintptr_t)bustup_check_init_tbl0;

    if (ptr[0] != 0x801820ACU ||
        ptr[1] != 0x801860ACU ||
        ptr[2] != 0x8018A0ACU ||
        ptr[3] != 0x8018E0ACU ||
        ptr[4] != 0x801920ACU) {
        now_sub_sub_step -= 4;
        return 0;
    }

    if (block_checksum_check(45, 0x80180000) == 0) {
        sub_sub_step_up();
        now_sub_sub_step -= 4;
        return 0;
    }

    result = get_g_zyotai_h(now_girl);
    bustup_sprite_init(bustup_check_init_tbl3, (s32)bustup_check_init_tbl0, bustup_check_init_tbl1, bustup_check_init_tbl2, result & 0x7F);

    bustup2_cw_bright = 0;
    bustup2_cw_attr |= (u32)0x81000000;
    bustup_cw_attr |= (u32)0x81000000;
    Sprites |= (u32)0x81000000;
    sprite1_attribute |= (u32)0x81000000;

    return sub_sub_step_up();
}
