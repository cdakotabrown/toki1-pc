/* SPDX-License-Identifier: GPL-3.0-or-later */
/* taco_actor_step5 -- PC override of src/overlay/TACO/taco_actor_step5.c
 * (0x80155F80).
 *
 * Statement for statement the matched function except the call to
 * taco_actor_exit, which passes `a0`.  The matched C declares
 * `taco_actor_exit(void)` and calls it bare; on the console the slot
 * index is still in $a0 at the jal 0x8015C218 on the one path that
 * reaches it, case 2 (0x80156118..0x80156148: entered from the dispatch
 * at 0x80155FBC; only case 0 reloads a0, for taco_shot_spawn), so
 * taco_actor_exit(slot) runs.  On x86-64 the bare call leaves %edi
 * whatever the last callee left there and taco_actor_exit releases a
 * garbage slot (the taco_actor_fall override has the crash it caused).
 *
 * @DivergesFrom: src/overlay/TACO/taco_actor_step5.c -- the argument the
 * console passes in $a0 is written out.
 */
/* taco_actor_step5 -- TACO overlay
 * Address: 0x80155F90 (real entry 0x80155F80)
 *
 * The 16-byte symbol at 0x80155F80 is this function's cross-prologue head:
 * IDO -O2 hoisted the taco_actor_rec load and the a0*17 half of the *136 index
 * above the `addiu sp` prologue.  It has no `jr ra` and falls straight
 * through, so it is absorbed here rather than being its own function.
 * Callers (taco_actor_dispatch) legitimately jal 0x80155F80 -- that reference must
 * stay named func_80155F80.
 *
 * Per-frame step handler for actor slot a0: state 0 pulses a shockwave every
 * 7th tick and at tick 60 picks a turn direction and advances the state;
 * state 1 slides the slot sideways until its tick passes 41; state 2 sinks it
 * and fires taco_actor_exit once it drops below -30000.
 */
#include "types.h"

typedef struct {
    s16 vx;
    s16 vy;
    s16 vz;
    s16 pad;
} SV;

extern void taco_shot_spawn(SV, s32, s32);
extern void taco_actor_exit(s32 idx);

extern u8 *taco_actor_rec;         /* actor block, stride 136 */

#define AC(n) (*(s16 *)(taco_actor_rec + a0 * 136 + (n)))

void taco_actor_step5(s32 a0)
{
    SV sv;
    s32 c;

    c = AC(126);

    switch (c) {
    case 0:
        c = AC(122);
        if (c % 7 == 0) {
            sv.vx = AC(116);
            sv.vy = AC(118);
            sv.vz = AC(120);
            taco_shot_spawn(sv, 256, 192);
            c = AC(122);
        }
        if (c == 60) {
            if (*(s16 *)(taco_actor_rec + 116) < AC(124)) {
                AC(124) = 1;
            } else {
                AC(124) = -1;
            }
            AC(122) = 0;
            AC(126) = AC(126) + 1;
        }
        break;
    case 1:
        AC(102) = AC(102) + AC(124) * 50;
        if (AC(122) >= 41) {
            AC(126) = AC(126) + 1;
        }
        break;
    case 2:
        AC(120) = AC(120) - 800;
        if (AC(120) < -30000) {
            taco_actor_exit(a0);
        }
        break;
    }
}
