/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tt_board_scroll: retain 100-byte sprite records on x64.
 * @DivergesFrom: src/overlay/TT/tt_board_scroll.c -- the cel address at +72
 * is a four-byte cell. Grid, attributes and scroll behavior are unchanged.
 */
#include "types.h"

typedef struct {
    /* 0x00 */ u8  unk00;
    /* 0x01 */ u8  unk01;
    /* 0x02 */ u8  pad02[14];
    /* 0x10 */ s16 unk10;
    /* 0x12 */ s16 unk12;
    /* 0x14 */ s16 unk14;
    /* 0x16 */ s16 unk16;
    /* 0x18 */ u8  pad18[32];
    /* 0x38 */ u8  unk38;
    /* 0x39 */ u8  unk39;
    /* 0x3A */ u8  unk3A;
    /* 0x3B */ u8  unk3B;
    /* 0x3C */ s16 unk3C;
    /* 0x3E */ s16 unk3E;
    /* 0x40 */ s16 unk40;
    /* 0x42 */ s16 unk42;
    /* 0x44 */ s16 unk44;
    /* 0x46 */ u8  unk46;
    /* 0x47 */ u8  unk47;
    /* 0x48 */ u32 unk48;
    /* 0x4C */ u8  unk4C;
    /* 0x4D */ u8  unk4D;
    /* 0x4E */ u8  unk4E;
    /* 0x4F */ u8  unk4F;
    /* 0x50 */ u8  unk50;
    /* 0x51 */ u8  pad51[19];
} TTRec;

typedef struct {
    /* 0x0 */ s16 unk0;
    /* 0x2 */ s16 unk2;
    /* 0x4 */ s16 unk4;
    /* 0x6 */ s16 unk6;
    /* 0x8 */ u8  pad8[8];
} TTCel;

typedef struct { u8 pad[0x2E]; u16 unk2E; } TTHit;
extern TTHit *tt_step_wk;
extern TTRec *tt_anim_buf;
extern TTCel *tt_scale_buf;


void tt_board_scroll(void)
{
    TTHit *hit;
    TTRec *p;
    TTCel *q;
    s32 i;

    hit = tt_step_wk;
    for (i = 0, p = tt_anim_buf + 64, q = tt_scale_buf; i < 25; i++) {
        q->unk4 = ((i / 5) * 72 - hit->unk2E - 92);
        q->unk6 = (hit->unk2E + (i % 5) * 72 - 72);
        p->unk14 = (q->unk4 - 36);
        p->unk16 = (q->unk6 - 36);
        p++;
        q++;
    }
    hit->unk2E++;
    if (hit->unk2E >= 72) {
        hit->unk2E = 0;
    }
}
