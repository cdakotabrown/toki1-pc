/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tt_anim: retain the retail 12-byte animation record on x64.
 * @DivergesFrom: src/overlay/TT/tt_anim.c -- script is a four-byte address;
 * widen at use so pattern/frame/flags/timer remain at +4/+6/+8/+9.
 * Script interpretation and animation state transitions are unchanged.
 */
#include "types.h"

typedef struct {
    u8 dur, pos, pat;
} TtFrame;

typedef struct {
    u32 script_addr;     /* +0 */
    u8 pattern;     /* +4 */
    u8 pad5;
    u8 frame;       /* +6 */
    u8 pad7;
    u8 flags;       /* +8 */
    u8 timer;       /* +9 */
} TtAnim;

void tt_anim(TtAnim *a, u8 *x, u8 *y)
{
    u32 f;
    u8 *s;
    u8 *p;
    u8 c;

    f = a->flags;
    if (f & 0x80) {
        a->flags = f & ~0x80;
        a->frame = 0;
        goto show;
    }
    if (f & 0x20) {
        a->flags = f & ~0x20;
        goto show;
    }
    if (f & 0x40) {
        return;
    }
    if (--a->timer != 0) {
        return;
    }
    a->frame++;
show:
    s = (u8 *)(unsigned long)a->script_addr;
    p = (u8 *)((TtFrame *)s + a->frame);
    c = *p;
    a->timer = c;
    if (c == 254) {
        a->frame = 0;
        a->timer = *s;
        p = s;
        a->flags |= 1;
    }
    *x = ((p[1] & 7) << 4) + 128;
    *y = (p[1] & ~7) << 1;
    p += 2;
    if (*p != 255) {
        a->pattern = *p;
    }
}
