/* SPDX-License-Identifier: GPL-3.0-or-later */
/* taco_actor_step1 -- PC override of src/overlay/TACO/taco_actor_step1.c
 * (0x80155218).
 *
 * Statement for statement the matched function except the call to
 * taco_actor_exit, which passes `idx`.  The matched C calls it bare
 * (`taco_actor_exit(void)`); on the console the slot index is still in $a0
 * at the jal (0x8015537C; nothing in the listing writes a0), so
 * taco_actor_exit(idx) runs.  On x86-64 the bare call passes whatever %edi
 * holds -- the fault taco_actor_fall's override describes.
 *
 * @DivergesFrom: src/overlay/TACO/taco_actor_step1.c -- the argument the
 * console passes in $a0 is written out.
 */
#include "types.h"

typedef struct {
    u8  pad0[100];
    s16 v[3];
    u16 sel;
    s16 w[3];
    u8  pad1[6];
    s16 cnt;
    u8  pad2[2];
    s16 lvl;
    u8  pad3[10];
} Rec;

extern Rec *taco_actor_rec;
extern void taco_actor_exit(s32 idx);

#define REC(i) ((Rec *)((i) * 136 + (u8 *)taco_actor_rec))

void taco_actor_step1(s32 idx)
{
    s32 a1;

    REC(idx)->cnt += 256;
    a1 = REC(idx)->lvl;
    REC(idx)->w[2] = (a1 << 9) + 512;
    taco_actor_rec[idx].w[1] = (a1 << 9) + 512;
    taco_actor_rec[idx].w[0] = (a1 << 9) + 512;
    switch (REC(idx)->sel) {
    case 0:
        REC(idx)->v[0] += 50;
        break;
    case 1:
        REC(idx)->v[1] += 50;
        break;
    case 2:
        REC(idx)->v[2] += 50;
        break;
    case 3:
        REC(idx)->v[0] += 99;
        break;
    case 4:
        REC(idx)->v[1] += 99;
        break;
    case 5:
        REC(idx)->v[2] += 99;
        break;
    }
    if (REC(idx)->cnt >= 4097) {
        taco_actor_exit(idx);
    }
}
