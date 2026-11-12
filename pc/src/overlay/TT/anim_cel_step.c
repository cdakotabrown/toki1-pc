/* SPDX-License-Identifier: GPL-3.0-or-later */
/* anim_cel_step: four-byte animation table and cel pointer cells.
 * @DivergesFrom: src/overlay/TT/anim_cel_step.c -- only pointer storage
 * and a void return prototype differ. Flag/timer/control-byte behavior,
 * including byte-width frame wraparound, follows the matched function.
 */
#include "types.h"

typedef struct {
    u8 pad0[2];
    u8 dur;
    u8 ctrl;
} Cel;

typedef struct {
    u32 cels_addr;
} Anim;

typedef struct {
    u8 pad0[5];
    u8 anim;
    u8 frame;
    u8 pad7;
    u8 flags;
    u8 timer;
} Obj;

extern u32 tt_anim_tbl[];

void anim_cel_step(Obj *o)
{
    Cel *cel;
    int fr;
    int c;

    if (o->flags & 0x80) {
        o->flags &= ~0x80;
        o->frame = 0;
        cel = ((Cel *)(unsigned long)((Anim *)(unsigned long)
            tt_anim_tbl[o->anim])->cels_addr);
        o->timer = cel->dur;
        return;
    }
    if (o->flags & 0x20) {
        o->flags &= ~0x20;
        cel = &((Cel *)(unsigned long)((Anim *)(unsigned long)
            tt_anim_tbl[o->anim])->cels_addr)[o->frame];
        o->timer = cel->dur;
        return;
    }
    if (o->flags & 0x40) {
        return;
    }
    if (--o->timer == 0) {
        fr = o->frame;
        cel = &((Cel *)(unsigned long)((Anim *)(unsigned long)
            tt_anim_tbl[o->anim])->cels_addr)[fr];
        c = cel->ctrl;
        if (c == 0) {
            o->frame = fr + 1;
            cel = &((Cel *)(unsigned long)((Anim *)(unsigned long)
            tt_anim_tbl[o->anim])->cels_addr)[o->frame];
            o->timer = cel->dur;
            return;
        }
        if (c == 255) {
            o->flags |= 1;
            return;
        }
        o->frame = fr - c;
        cel = &((Cel *)(unsigned long)((Anim *)(unsigned long)
            tt_anim_tbl[o->anim])->cels_addr)[o->frame];
        o->timer = cel->dur;
    }
}
