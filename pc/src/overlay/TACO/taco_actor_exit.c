/* SPDX-License-Identifier: GPL-3.0-or-later */
/* taco_actor_exit -- PC override of src/overlay/TACO/taco_actor_exit.c
 * (0x8015C218).
 *
 * Statement for statement the matched function except the
 * taco_actor_disp_switch call, which passes the slot as its second
 * argument.  taco_actor_disp_switch(mode, id) takes two (every other
 * caller passes both); the matched C writes `taco_actor_disp_switch(0)`
 * and matches because IDO keeps `id` in $a1 across the call
 * (`move a1,a0 ; sw a1,40(sp) ; jal 0x8015C0B0 ; move a0,zero`), so the
 * console runs taco_actor_disp_switch(0, id).  On x86-64 the second
 * argument was whatever %esi held: SIGSEGV in
 * TACO_taco_actor_disp_switch+0x1a (fault_addr 0xc8779bd9), ~140,000
 * recovered crashes in TACO gameplay (scene 144 sub-step 11), once the
 * bare taco_actor_exit() callers pass their slot (taco_actor_fall's
 * override).
 *
 * @DivergesFrom: src/overlay/TACO/taco_actor_exit.c -- the argument the
 * console passes in $a1 is written out.
 */
#include "types.h"

extern u8 *taco_actor_rec;
extern u8 taco_node_own[];
extern u8 taco_kind_mdl_tbl[];

extern void taco_actor_disp_switch(s32 mode, s32 id);

void taco_actor_exit(s32 id)
{
    u8 *p;

    taco_actor_disp_switch(0, id);

    taco_actor_rec[id * 136 + 1] = 0;
    taco_actor_rec[id * 136 + 134] = 0;

    p = taco_node_own;
    do {
        if (id == p[0]) {
            p[0] = 0;
        }
        if (id == p[1]) {
            p[1] = 0;
        }
        if (id == p[2]) {
            p[2] = 0;
        }
        if (id == p[3]) {
            p[3] = 0;
        }
        p += 4;
    } while (p != taco_kind_mdl_tbl);
}
