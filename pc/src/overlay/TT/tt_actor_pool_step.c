/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tt_actor_pool_step: retain the 120-byte retail actor stride on x64.
 * @DivergesFrom: src/overlay/TT/tt_actor_pool_step.c -- the animation address at +56
 * is a four-byte cell, not a native pointer. Its embedded animation state,
 * all actor fields and every state transition keep their retail layout.
 */
#include "types.h"

typedef struct {
    u8  b0;         /* 0x00 */
    u8  pad01[3];
    u16 h4;         /* 0x04 */
    u16 h6;         /* 0x06 */
    u16 h8;         /* 0x08 */
    u16 h10;        /* 0x0A */
    u8  pad0C;
    u8  b13;        /* 0x0D */
    u8  b14;        /* 0x0E */
    u8  b15;        /* 0x0F */
    u8  pad10[4];
    s16 x;          /* 0x14 */
    s16 y;          /* 0x16 */
    u8  pad18[32];
    u32 anim_addr;  /* 0x38 */
    u8  b60;        /* 0x3C */
    u8  b61;        /* 0x3D */
    u8  b62;        /* 0x3E */
    u8  pad3F;
    u8  b64;        /* 0x40 */
    u8  pad41[21];
    u16 h86;        /* 0x56 */
    u8  pad58[4];
    u8  b92;        /* 0x5C */
    u8  b93;        /* 0x5D */
    u8  pad5E[8];
    u16 h102;       /* 0x66 */
    u8  pad68[16];
} Actor;            /* 120 */

typedef struct {
    u8  pad00[36];
    u8  b36;        /* 0x24 */
    u8  b37;        /* 0x25 */
} TTState;

extern TTState *tt_step_wk;
extern Actor   *tt_obj_pool;
extern u8       tt_anim_tbl[];

extern void tt_obj_move();

void tt_actor_pool_step()
{
    TTState *g;
    Actor *s;
    s32 i;

    g = tt_step_wk;
    tt_obj_spawn();
    for (i = 0, s = tt_obj_pool; i < 32; i++, s++) {
        if (s->b0 == 0) {
            continue;
        }
        switch (s->h4) {
        case 64:
            if (s->y >= 249) {
                s->b0 = 0;
            }
            break;
        case 112:
            tt_cmd_once(1549);
            g->b37++;
            if (g->b36 != 0 && (g->b37 & 1)) {
                if (g->b37 & 2) {
                    tt_shot_fan4_make(s->x, s->y, 256, 32, 160);
                } else {
                    tt_shot_spread_make(s->x, s->y, 256, 32, 160);
                }
            }
            s->anim_addr = (u32)(unsigned long)tt_anim_tbl;
            s->b60 = 0;
            s->b61 = 2;
            s->b62 = 14;
            s->b64 = 32;
            s->h4 = 113;
            s->h6 = s->h8 = s->h10 = 0;
            s->b13 = s->b14 = s->b15 = 0;
            break;
        case 120:
            if (--s->h102 == 0) {
                s->h86 = 2;
                s->h102 = 30;
                s->h4 = 121;
                s->h6 = s->h8 = s->h10 = 0;
                s->b13 = s->b14 = s->b15 = 0;
            }
            break;
        case 121:
            if (--s->h102 == 0) {
                s->h4 = 128;
            }
            break;
        case 122:
            if (s->y >= 249) {
                s->b0 = 0;
            }
            break;
        case 128:
            s->b0 = 0;
            break;
        }
        tt_obj_move(s);
        if (s->h4 == 64) {
            tt_anim(&s->anim_addr, &s->b92, &s->b93);
        } else if (s->h4 == 113) {
            anim_cel_step(&s->anim_addr);
            if (s->b64 & 1) {
                switch (tt_ready_check()) {
                case 0:
                    tt_touzyou_set(s, 109);
                    s->h102 = 60;
                    s->h4 = 120;
                    s->h6 = s->h8 = s->h10 = 0;
                    s->b13 = s->b14 = s->b15 = 0;
                    break;
                case 1:
                    tt_touzyou_set(s, 110);
                    s->h4 = 122;
                    s->h6 = s->h8 = s->h10 = 0;
                    s->b13 = s->b14 = s->b15 = 0;
                    break;
                case 2:
                    tt_touzyou_set(s, 111);
                    s->h4 = 122;
                    s->h6 = s->h8 = s->h10 = 0;
                    s->b13 = s->b14 = s->b15 = 0;
                    break;
                }
            }
        }
    }
}
