/* SPDX-License-Identifier: GPL-3.0-or-later */
/* kangei_girl_set -- PC override of src/overlay/KANGEI/kangei_girl_set.c
 * (0x801325D0).
 *
 * Statement for statement the matched function except the date_name_set
 * call.  date_name_set(girl) copies girl's names (it reads (u8)arg0 < 14);
 * the matched C declares it `(void)` and calls it bare.  The listing loads
 * a0 = girl_no (lbu a0 at 0x801325D4) and stores it to now_girl in the
 * delay slot of the jal 0x800849C0, so the console runs date_name_set(a0).
 * On x86-64 the argument was whatever %edi held: another girl's name, or
 * none. pc/tools/audit_ps1_call_args.py SHORT-READ.
 *
 * @DivergesFrom: src/overlay/KANGEI/kangei_girl_set.c -- the argument the
 * console passes in $a0 is written out.
 */
/* kangei_girl_set - KANGEI overlay, Address: 0x801325D8, Size: 52 bytes (13 insns) 
 * Status: BYTE-PERFECT 🎉
 */
#include "types.h"

extern u8 now_girl;
extern void date_name_set(s32 girl);
extern void kangei_ptr_tbl_init(void);
extern void kangei_girl_ptr_set(void);
extern u8 girl_no;

void kangei_girl_set(void)
{
    u8 a0 = girl_no;

    now_girl = a0;
    date_name_set(a0);
    kangei_ptr_tbl_init();
    kangei_girl_ptr_set();
}
