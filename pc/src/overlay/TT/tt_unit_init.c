/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tt_unit_init: preserve 20-byte records and +584/+632/+680/+784 arrays.
 * @DivergesFrom: src/overlay/TT/tt_unit_init.c -- use shared four-byte
 * script cells. Slot attributes, coordinates and visibility are unchanged.
 */
#include "types.h"
#include "tt_serve_unit.h"


struct Obj {
    unsigned char pad0[56];
    TtServeUnit rec[26];
    unsigned char pad576[8];
    short x[24];
    short y[24];
    short h[24];
    unsigned char pad728[56];
    unsigned char flag[8];
};

extern struct Obj *tt_boss_wk;
extern char tt_anim_tbl[];
extern short tt_unit_y_tbl[];

void tt_unit_init()
{
    struct Obj *p;
    int k;

    p = tt_boss_wk;
    for (k = 2; k < 7; k++) {
        p->rec[k].script_addr = (u32)(unsigned long)tt_anim_tbl;
        p->rec[k].b4 = 0;
        p->rec[k].b5 = 6;
        if (k == 6) {
            p->rec[k].b6 = 20;
        } else {
            p->rec[k].b6 = 31;
        }
        p->rec[k].b8 = 96;
        p->rec[k].b9 = 0;
        p->rec[k].b11 = 15;
        p->rec[k].b16 = 0;
        p->flag[k] = 1;
        p->h[k] = 9;
        if (k == 6) {
            p->x[k] = 0;
        } else {
            p->x[k] = k * 16 - 56;
        }
        p->y[k] = tt_unit_y_tbl[k - 2];
    }
}
