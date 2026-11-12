/* SPDX-License-Identifier: GPL-3.0-or-later */
/* taco_actor_fall -- PC override of src/overlay/TACO/taco_actor_fall.c
 * (0x8015AB90).
 *
 * Statement for statement the matched function except the call to
 * taco_actor_exit, which passes `idx`.  The matched C declares
 * `taco_actor_exit(void)` and calls it bare: on the console the slot index
 * is still in $a0 at the jal (the listing never writes a0: 0x8015AB90..
 * 0x8015ABE0), so taco_actor_exit(idx) runs.  On x86-64 the bare call
 * leaves %edi whatever the last callee left there; taco_actor_exit then
 * stored at taco_actor_rec + garbage * 136 -- SIGSEGV in
 * TACO_taco_actor_exit+0x2a, fault_addr 0x89e3a3f9, ~370,000 recovered
 * crashes in TACO gameplay (scene 144 sub-step 11; lit_ingame_check.py).
 *
 * @DivergesFrom: src/overlay/TACO/taco_actor_fall.c -- the argument the
 * console passes in $a0 is written out.
 */
#include "types.h"

typedef struct {
    u8  pad0[120];
    s16 f120;
    u8  pad122[14];
} TacoRec;   /* 136 bytes */

extern TacoRec *taco_actor_rec;

extern void taco_actor_exit(s32 idx);

void
taco_actor_fall(s32 idx)
{
    TacoRec *p;
    s32 ofs;

    ofs = idx * 136;
    p = &taco_actor_rec[idx];
    p->f120 = p->f120 - 250;
    p = (TacoRec *)(ofs + (u32)taco_actor_rec);
    if (p->f120 >= -30000) {
        return;
    }
    taco_actor_exit(idx);
}
