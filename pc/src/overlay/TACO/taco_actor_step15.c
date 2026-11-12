/* SPDX-License-Identifier: GPL-3.0-or-later */
/* taco_actor_step15 -- PC override of
 * src/overlay/TACO/taco_actor_step15.c (0x80158B94).
 *
 * Statement for statement the matched function except the call to
 * taco_actor_exit, which passes `a0`.  The matched C declares
 * `taco_actor_exit(void)` and calls it bare; on the console the slot
 * index is still in $a0 at the jal 0x8015C218 on the one path that
 * reaches it, the first test (0x80158B94..0x80158C1C: no call and no
 * write of a0 since entry; the effect spawns come after it), so
 * taco_actor_exit(slot) runs.  On x86-64 the bare call leaves %edi
 * whatever the last callee left there and taco_actor_exit releases a
 * garbage slot (the taco_actor_fall override has the crash it caused).
 *
 * @DivergesFrom: src/overlay/TACO/taco_actor_step15.c -- the argument the
 * console passes in $a0 is written out.
 */
/* taco_actor_step15 -- TACO overlay
 * Address: 0x80158BA4 (real entry 0x80158B94)
 *
 * The 16-byte symbol at 0x80158B94 is this function's cross-prologue head:
 * IDO -O2 hoisted the taco_actor_rec load and the a0*17 half of the *136 index
 * above the `addiu sp` prologue.  It has no `jr ra` and falls through, so it
 * is absorbed here rather than being a function of its own.
 *
 * Places actor slot a0 on the conveyor: x is stepped 20 units per tick from
 * +7000, y/z are pinned, the slot is retired once it runs off the -7000 end,
 * and the three fixed x stops (-800, 0, 800) fire their scripted handlers.
 */
#include "types.h"

extern void taco_actor_exit(s32 idx);
extern void taco_effect_spawn(s16, s32, s32);
extern void taco_effect_spawn2(s16, s32, s32);

extern u8 *taco_actor_rec;         /* actor block, stride 136 */

#define AC(n) (*(s16 *)(taco_actor_rec + a0 * 136 + (n)))

void taco_actor_step15(s32 a0)
{
    AC(116) = 7000 - AC(122) * 20;
    AC(118) = -1700;
    AC(120) = -20000;

    if (AC(116) < -7000) {
        taco_actor_exit(a0);
    }

    if (AC(116) == -800) {
        taco_effect_spawn(AC(116), AC(118), AC(120));
    }

    if (AC(116) == 0) {
        taco_effect_spawn2(AC(116), AC(118), AC(120));
    }

    if (AC(116) == 800) {
        taco_effect_spawn(AC(116), AC(118), AC(120));
    }
}
