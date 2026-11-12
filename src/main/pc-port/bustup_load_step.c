/*
 * bustup_load_step -- PC override of src/main/matched/bustup_load_step.c
 * (main binary).
 *
 * Statement for statement the matched function; the divergence is the
 * pointer WIDTH.  bustup_check_init_tbl0 / tbl1 / tbl2 (0x800D0620 / 24 /
 * 28) are adjacent 4-byte PS1 pointer cells (see bustup_check_init.c); the
 * matched C declares them `s32 *`, 8-byte loads on LP64 that join two
 * cells into a non-canonical address.  Here each cell is read as u32 and
 * widened, as the console reads it.
 *
 * The read size: the matched C declares bustup_load_step(void) and calls
 * bustup_cd_read() bare, but bustup_cd_read(size) hands it to
 * cd_read_start2 as the byte count, and the console's listing writes no a0
 * between the entry and jal bustup_cd_read (0x8008683C): the size is
 * bustup_load_step's own a0, which every caller passes
 * (func_80086828(27743) in BUNKAKEN, (21038) in BUNKASAI, ...;
 * missing_funcs_gen.c's func_80086828 jumps here with the registers
 * intact).  Written out here (pc/tools/audit_ps1_call_args.py SHORT-READ).
 *
 * @DivergesFrom: matched bustup_load_step -- pointer read width
 * (pc/tools/audit_ps1_wide_access.py) and the size argument the console
 * passes in $a0.
 */
#include <stdint.h>
#include "types.h"

extern u8 step_mode;
extern u32 bustup_check_init_tbl0;      /* PS1 pointers */
extern u32 bustup_check_init_tbl1;
extern u32 bustup_check_init_tbl2;

#define TBL0 ((s32 *)(uintptr_t)bustup_check_init_tbl0)
#define TBL1 ((s32 *)(uintptr_t)bustup_check_init_tbl1)
#define TBL2 ((s32 *)(uintptr_t)bustup_check_init_tbl2)

extern void bustup_cd_read(s32 size);
extern s32 get_CD_State2(void);
extern s32 block_checksum_check(s32, u32);
extern void sub_sub_step_up(void);

void bustup_load_step(s32 size)
{
    if (step_mode == 0) {
        bustup_cd_read(size);
        step_mode++;
        return;
    }

    if ((get_CD_State2() & 1) != 0) {
        if (TBL0[0] != 0x801820AC || TBL0[1] != 0x801860AC ||
            TBL0[2] != 0x8018A0AC || TBL0[3] != 0x8018E0AC ||
            TBL0[4] != 0x801920AC || TBL2[0] != 0x8018005C ||
            TBL2[1] != 0x80180070 || TBL1[0] != 0x80180000) {
            step_mode = 0;
            return;
        }
        if (block_checksum_check(45, 0x80180000) == 0) {
            step_mode = 0;
            return;
        }
        sub_sub_step_up();
    }
}
