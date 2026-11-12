/* SPDX-License-Identifier: GPL-3.0-or-later */
/* taco_actor_rise -- PC override of src/overlay/TACO/taco_actor_rise.c.
 *
 * Statement for statement the matched function except the call to
 * taco_actor_exit, which passes `idx`.  The matched C calls it bare; the
 * console still holds the slot index in $a0 at the jal (0x8015AB78: no
 * call and no write of a0 before it), so taco_actor_exit(idx) runs.  On
 * x86-64 the bare call passes whatever %edi holds (taco_actor_fall's
 * override has the fault it caused).
 *
 * @DivergesFrom: src/overlay/TACO/taco_actor_rise.c -- the argument the
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
taco_actor_rise(s32 idx)
{
    TacoRec *p;
    s32 ofs;

    ofs = idx * 136;
    p = &taco_actor_rec[idx];
    p->f120 = p->f120 + 256;
    p = (TacoRec *)(ofs + (u32)taco_actor_rec);
    if (p->f120 < 4097) {
        return;
    }
    taco_actor_exit(idx);
}
