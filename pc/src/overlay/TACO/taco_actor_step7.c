/* SPDX-License-Identifier: GPL-3.0-or-later */
/* taco_actor_step7 -- PC override of src/overlay/TACO/taco_actor_step7.c.
 *
 * Statement for statement the matched function except the call to
 * taco_actor_exit, which passes the slot index.  The matched C calls it
 * bare; the console still holds the index in $a0 at the jal (0x80156A44:
 * no call and no write of a0 before it), so taco_actor_exit(a0) runs.  On
 * x86-64 the bare call passes whatever %edi holds (taco_actor_fall's
 * override has the fault it caused).
 *
 * @DivergesFrom: src/overlay/TACO/taco_actor_step7.c -- the argument the
 * console passes in $a0 is written out.
 */
#include "types.h"

extern void taco_actor_exit(s32 idx);

extern u8 *taco_actor_rec;         /* actor block, stride 136 */

#define AC(n)   (*(s16 *)(taco_actor_rec + a0 * 136 + (n)))
#define BASE(n) (*(s16 *)(taco_actor_rec + (n)))

void taco_actor_step7(s32 a0)
{
    AC(120) = AC(120) + 250;
    AC(116) = AC(116) + (BASE(116) - AC(116)) / AC(124);
    AC(118) = AC(118) + (BASE(118) - AC(118)) / AC(124);

    if (AC(126) == 0) {
        AC(104) = AC(104) + 200;
    } else {
        AC(104) = AC(104) - 200;
    }

    if (AC(120) >= 2001) {
        taco_actor_exit(a0);
    }
}
