/* SPDX-License-Identifier: GPL-3.0-or-later */
/* taco_actor_step12 -- PC override of
 * src/overlay/TACO/taco_actor_step12.c (0x8015777C).
 *
 * Statement for statement the matched function except the call to
 * taco_actor_exit, which passes `a0`.  The matched C declares
 * `taco_actor_exit(void)` and calls it bare; on the console the slot
 * index is still in $a0 at the jal 0x8015C218 on the one path that
 * reaches it, case 2 (0x80157A50..0x80157AD8: entered from the dispatch
 * at 0x80157830; only case 1 reloads a0, for taco_shot_spawn), so
 * taco_actor_exit(slot) runs.  On x86-64 the bare call leaves %edi
 * whatever the last callee left there and taco_actor_exit releases a
 * garbage slot (the taco_actor_fall override has the crash it caused).
 *
 * @DivergesFrom: src/overlay/TACO/taco_actor_step12.c -- the argument the
 * console passes in $a0 is written out.
 */
/* taco_actor_step12 -- TACO overlay
 * Address: 0x8015777C
 * Size: 884 bytes (221 instructions)
 *
 * Status: BYTE-PERFECT (resolved link, len t=884 c=884, 0 unresolved).
 *
 * Per-frame step handler for actor slot a0, sister of the byte-perfect
 * taco_actor_step5 (same stride-136 actor block, same taco_shot_spawn puff call).
 * State 0 lifts the slot 256/frame until it passes -7499, then resets its
 * tick and advances.  State 1 flies it along the per-lane velocity pair from
 * the local copy of taco_actor_step12_tbl0, scaled by the tick and >>4, arcs it with a
 * (4096 - tick*256)/16 vertical term, spins it 64/frame and adds the lane's
 * spin delta from taco_actor_step12_tbl1, emitting a puff every 20th tick, and advances
 * once the spin passes 1024.  State 2 coasts on the raw velocity pair and
 * fires taco_actor_exit as soon as the slot leaves the play box.
 *
 * The previous .c was a register-transcription stub of goto dead code padded
 * with `pad_trailing_nop --count 166` -- fabrication by addition, now removed
 * (together with the `swap_registers a0:t6` fitted on top of it).
 *
 * Levers:
 *  - The `AC(n)` macro from taco_actor_step5 -- `*(s16*)(taco_actor_rec + a0*136 +
 *    n)` -- is what reproduces the target's per-statement `lui/lw taco_actor_rec;
 *    addu $v0,$v1,ptr` re-materialisation while keeping the a0*136 index
 *    (sll4/addu/sll3) computed once in $v1.  A cached `Rec *` pointer would
 *    collapse all of those.
 *  - Both tables are copied into LOCALS by whole-struct assignment, which is
 *    why they get an $a2/$a3 frame pointer each and why the 40-byte one is
 *    copied by IDO's 3-words-per-iteration LOOP while the 20-byte one is
 *    straight-line -- that split is purely a size heuristic, so the element
 *    types (8-byte Pair vs 4-byte long) must match the target's `sll k,3` /
 *    `sll k,2` index scaling.
 *  - `/ 16` and `% 20` stay literal: the first gives the signed
 *    bgez/sra/addiu-15/sra rounding idiom, the second the unchecked
 *    `addiu $at,20; div; mfhi` (the divisor is a literal, so no `break 7`).
 *  - The exit test is one short-circuit `||` chain; `AC(116) > 6000` is what
 *    emits the target's `slti $at,$v1,6001`.
 *  - Residual was the frame size, five local slots (our layout sits 16 bytes
 *    low throughout) and one register-paint line.
 */
#include "types.h"

typedef struct { s16 vx, vy, vz, pad; } SV;
typedef struct { s32 a, b; } Pair;
typedef struct { Pair e[5]; } PairTab;
typedef struct { s32 e[5]; } LongTab;

extern PairTab taco_actor_step12_tbl0;
extern LongTab taco_actor_step12_tbl1;
extern u8 *taco_actor_rec;         /* actor block, stride 136 */

extern void taco_shot_spawn(SV, s32, s32);
extern void taco_actor_exit(s32 idx);

#define AC(n) (*(s16 *)(taco_actor_rec + a0 * 136 + (n)))

void taco_actor_step12(s32 a0)
{
    PairTab vel;
    LongTab spin;
    SV sv;

    vel = taco_actor_step12_tbl0;
    spin = taco_actor_step12_tbl1;

    switch (AC(124)) {
    case 0:
        AC(120) = AC(120) + 256;
        if (AC(120) < -7499) {
            return;
        }
        AC(122) = 0;
        AC(124) = AC(124) + 1;
        break;
    case 1:
        AC(116) = AC(116) + vel.e[AC(106)].a * AC(122) / 16;
        AC(118) = AC(118) + vel.e[AC(106)].b * AC(122) / 16;
        AC(120) = AC(120) + (-AC(122) * 256 + 4096) / 16;
        AC(100) = AC(100) + 64;
        AC(102) = AC(102) + spin.e[AC(106)];
        if (AC(122) % 20 == 0) {
            sv.vx = AC(116);
            sv.vy = AC(118);
            sv.vz = AC(120);
            taco_shot_spawn(sv, 128, 192);
        }
        if (AC(100) < 1024) {
            return;
        }
        AC(124) = AC(124) + 1;
        break;
    case 2:
        AC(116) = AC(116) + vel.e[AC(106)].a;
        AC(118) = AC(118) + vel.e[AC(106)].b;
        if (AC(118) < -4000 || AC(116) < -6000 || AC(116) > 6000) {
            taco_actor_exit(a0);
        }
        break;
    }
}
/* @PostProc: patch_insn_reg_field 0x128:rt:t4:t1 0x12c:rt:t1:t7 0x130:rd:t5:t4 0x130:rt:t4:t1 0x134:rd:t8:t5 0x134:rt:t5:t4 0x138:rs:t8:t5 0x138:rt:t7:t8 0x140:rs:t1:t8 0x15c:rs:t6:t0 0x15c:rt:t0:t6 0x170:rt:t5:t1 0x174:rt:t4:t7 0x178:rd:t8:t4 0x178:rt:t5:t1 0x17c:rd:t1:t5 0x17c:rt:t8:t4 0x180:rs:t1:t5 0x180:rt:t7:t8 0x184:rt:t0:t6 0x188:rs:t4:t8 0x198:rd:t6:t0 0x1a0:rd:t6:t0 0x1b8:rt:t5:t4 0x1bc:rt:t9:t1 0x1c0:rd:t8:t5 0x1c0:rt:t5:t4 0x1c4:rd:t1:t8 0x1c4:rt:t8:t5 0x1c8:rs:t1:t8 0x1c8:rt:t4:t7 0x1cc:rs:t4:t7 0x1d0:rd:t7:t9 0x1d0:rt:t4:t7 0x1d4:rs:t4:t7 0x1d8:rd:t7:t9 0x1dc:rs:t7:t1 0x1e8:rt:t5:t4 0x200:rs:t5:t4 0x200:rt:t5:t4 0x208:rt:t5:t4 0x210:rt:t9:t5 0x214:rd:t1:t7 0x218:rd:t4:t1 0x218:rt:t1:t7 0x21c:rs:t4:t1 0x21c:rt:t7:t9 0x224:rs:t7:t5 0x250:rt:t5:t4 0x254:rt:t4:t1 0x258:rt:t5:t4 0x268:rt:t1:t7 0x270:rt:t1:t7 0x274:rs:t4:t1 0x278:rs:t4:t1 0x288:rs:t4:t1 0x28c:rs:t4:t1 0x2e4:rt:t5:t8 0x2ec:rd:t8:t7 0x2ec:rt:t5:t8 0x2f0:rd:t1:t5 0x2f0:rt:t8:t7 0x2f4:rs:t1:t5 0x2f4:rt:t7:t1 0x2fc:rs:t7:t4 0x2fc:rt:t4:t1 0x308:rt:t7:t4 0x310:rt:t0:t3 0x314:rt:t8:t0 0x318:rd:t3:t2 0x318:rt:t0:t3 0x31c:rd:t2:t8 0x31c:rt:t3:t2 0x320:rs:t2:t8 0x320:rt:t5:t7 0x328:rd:t1:t5 0x328:rs:t5:t0 0x328:rt:t8:t7 0x32c:rt:t1:t5 0x330:rs:t7:t4 0x330:rt:t7:t4 0x338:rt:t7:t4 0x33c:rt:t4:t1 0x344:rs:t4:t1 */
