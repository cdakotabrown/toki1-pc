/* SPDX-License-Identifier: GPL-3.0-or-later */
/* taco_actor_ramp -- PC override of src/overlay/TACO/taco_actor_ramp.c
 * (0x8015A388).
 *
 * Statement for statement the matched function except the call to
 * taco_actor_exit, which passes `idx`.  The matched C declares
 * `taco_actor_exit(void)` and calls it bare; on the console the slot
 * index is still in $a0 at the jal 0x8015C218 on the one path that
 * reaches it, case 3 (0x8015A618..0x8015A678: entered from the dispatch
 * at 0x8015A3C4, no call and no write of a0 before the jal; cases 0 and 2
 * load a0 for rsin/rcos, case 3 does not), so taco_actor_exit(slot) runs.
 * On x86-64 the bare call leaves %edi whatever the last callee left there
 * and taco_actor_exit releases a garbage slot (the taco_actor_fall
 * override has the crash it caused).
 *
 * @DivergesFrom: src/overlay/TACO/taco_actor_ramp.c -- the argument the
 * console passes in $a0 is written out.
 */
/* taco_actor_ramp -- TACO overlay
 * Address: 0x8015A388
 * Size: 776 bytes (194 insns)
 *
 * Four-phase animation step for the stride-136 record taco_actor_rec[idx]:
 *   0  seed the position from rsin/rcos of the angle at +106 (radius 1000,
 *      >>12), park the depth at -20000, set the ramp to 1 and advance;
 *   1  ramp up (+122 << 9) with the fade byte at +132 tracking 22/32 of it,
 *      advancing once the ramp reaches the limit at +108;
 *   2  re-place from the angle, spin it by 32 per frame until +122 >= 601;
 *   3  ramp back down from 16 and hand off to taco_actor_exit once it hits 0.
 */
#include "types.h"

typedef struct {
    u8 pad0[106];
    s16 f106;
    s16 f108;
    s16 f110;
    u8 pad1[4];
    s16 f116;
    s16 f118;
    s16 f120;
    s16 f122;
    s16 f124;
    u8 pad2[6];
    u8 f132;
    u8 pad3[3];
} Rec;

extern Rec *taco_actor_rec;

extern s32 rsin(s32);
extern s32 rcos(s32);
extern void taco_actor_exit(s32 idx);

void taco_actor_ramp(s32 idx)
{
    switch (taco_actor_rec[idx].f124) {
    case 0:
        taco_actor_rec[idx].f116 =
            (s16)((rsin(taco_actor_rec[idx].f106) * 1000) >> 12);
        taco_actor_rec[idx].f118 =
            (s16)(((rcos(taco_actor_rec[idx].f106) * 1000) >> 12) - 700);
        taco_actor_rec[idx].f120 = -20000;
        taco_actor_rec[idx].f110 = 1;
        taco_actor_rec[idx].f124 = taco_actor_rec[idx].f124 + 1;
        break;
    case 1:
        taco_actor_rec[idx].f110 = (s16)(taco_actor_rec[idx].f122 << 9);
        taco_actor_rec[idx].f132 = (u8)((taco_actor_rec[idx].f122 * 22) / 32);
        if (taco_actor_rec[idx].f110 >= taco_actor_rec[idx].f108) {
            taco_actor_rec[idx].f124 = taco_actor_rec[idx].f124 + 1;
            taco_actor_rec[idx].f122 = 0;
        }
        break;
    case 2:
        taco_actor_rec[idx].f116 =
            (s16)((rsin(taco_actor_rec[idx].f106) * 1000) >> 12);
        taco_actor_rec[idx].f118 =
            (s16)(((rcos(taco_actor_rec[idx].f106) * 1000) >> 12) - 700);
        taco_actor_rec[idx].f106 = taco_actor_rec[idx].f106 + 32;
        if (taco_actor_rec[idx].f122 >= 601) {
            taco_actor_rec[idx].f124 = taco_actor_rec[idx].f124 + 1;
            taco_actor_rec[idx].f122 = 0;
        }
        break;
    case 3:
        taco_actor_rec[idx].f110 = (s16)((16 - taco_actor_rec[idx].f122) << 9);
        taco_actor_rec[idx].f132 = (u8)(((16 - taco_actor_rec[idx].f122) * 32) / 32);
        if (taco_actor_rec[idx].f110 <= 0) {
            taco_actor_exit(idx);
        }
        break;
    }
}
