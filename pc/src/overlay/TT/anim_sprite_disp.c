/* SPDX-License-Identifier: GPL-3.0-or-later */
/* anim_sprite_disp: keep request, table and entry pointers as PS1 words.
 * @DivergesFrom: src/overlay/TT/anim_sprite_disp.c -- only pointer storage
 * and widening differ. Cel iteration, flips, palette/UV decode, packet
 * allocation and sorter calls follow the matched source.
 */
#include "types.h"

extern void tt_sprite_ot_add_16x16(s32, s32, s32);

extern s32 *tt_disp_wk;
extern u8  *tt_db_buf;
extern s16 *tt_scale_buf;

void anim_sprite_disp(s16 *pos, u8 *req, u16 idx)
{
    s32  unused0;
    u8  *ot;
    s16 *scr;
    u32 *lists;
    u32 *entry;
    u8  *cels;
    u16 *ip;
    u8  *cel;
    u8  *prim;
    u8  *pix;
    s32  otslot;
    s32  n;
    s32  x;
    s32  y;
    s32  flags;
    u32  tex;
    u32  lo;
    s32  clut;
    s32  one;
    u8   pal;

    ot = tt_db_buf + tt_disp_wk[2] * 32952;
    lists = (u32 *)(unsigned long)*(u32 *)req;
    scr = tt_scale_buf;

    if (lists == (u32 *)0) {
        return;
    }

    entry = (u32 *)(unsigned long)lists[req[5]];
    ip = (u16 *) ((u8 *)(unsigned long)entry[0] + req[6] * 4);
    cels = (u8 *)(unsigned long)entry[1] + *ip * 4;
    cel = (u8 *)(unsigned long)entry[2] + *(u16 *) (cels + 0) * 8
          + *(u16 *) (cels + 2) * 8 - 8;
    pix = (u8 *)(unsigned long)entry[3];
    clut = *(u16 *) ((u8 *) entry + 16);
    scr[2] = pos[0];
    scr[3] = pos[1];

    if (req[16] != 0) {
        scr[0] = *(s16 *) (req + 12);
        scr[1] = *(s16 *) (req + 14);
    }

    n = *(u16 *) cels;
    otslot = (s32) (ot + idx * 4) + 0x8008;
    one = 1;

    for (; n > 0; n--, cel -= 8) {
        prim = (u8 *)(unsigned long)*(u32 *)(ot + 4);
        *(u32 *)(ot + 4) = (u32)(unsigned long)(prim + 0x30);

        prim[0x28] = (u8) (cel[7] ^ req[4]);

        flags = req[4];
        if ((flags & 1) != 0) {
            y = pos[1] - *(s16 *) (cel + 4);
            y -= 16;
        } else {
            y = *(s16 *) (cel + 4) + pos[1];
        }
        if ((flags & 2) != 0) {
            x = pos[0] - *(s16 *) (cel + 2);
            x -= 16;
        } else {
            x = *(s16 *) (cel + 2) + pos[0];
        }
        *(s16 *) (prim + 8) = (s16) x;
        *(s16 *) (prim + 10) = (s16) y;

        tex = *(u16 *) cel;
        lo = tex & 0x3F;
        prim[0x0C] = (u8) ((pix[tex >> 6] & 0x80) + ((lo & 7) << 4));
        prim[0x0D] = (u8) (((pix[tex >> 6] & 0x40) << 1)
                           + (((s32) lo >> 3) << 4));
        prim[0x29] = 0;
        prim[0x2A] = (u8) one;
        *(s16 *) (prim + 0x16) = (s16) (pix[tex >> 6] & 0x1F);

        if (req[10] != 0) {
            pal = req[11];
            *(s16 *) (prim + 0x0E) =
                (s16) (clut + (pal & 0xF) + ((pal & 0xF0) << 2));
        } else {
            pal = cel[6];
            *(s16 *) (prim + 0x0E) =
                (s16) (clut + (pal & 0xF) + ((pal & 0xF0) << 2));
        }

        prim[0x2C] = 0;
        prim[0x2B] = req[16];
        tt_sprite_ot_add_16x16(otslot, (s32) prim, (s32) scr);

    }
}
