/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tt_board_init: retain 100-byte sprite records on x64.
 * @DivergesFrom: src/overlay/TT/tt_board_init.c -- the cel address at +72
 * is a four-byte cell. Grid, attributes and scroll behavior are unchanged.
 */
#include "types.h"

typedef struct {
    /* 0x00 */ u8  unk00;
    /* 0x01 */ u8  unk01;
    /* 0x02 */ u8  pad02[14];
    /* 0x10 */ u16 unk10;
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
    /* 0x44 */ u16 unk44;
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

extern u8 *tt_step_wk;
extern TTRec *tt_anim_buf;
extern TTCel *tt_scale_buf;

void tt_board_init(u8 arg0, u8 arg1)
{
    u8 *hit;
    TTRec *p;
    TTCel *q;
    s32 i;

    hit = tt_step_wk;
    for (i = 0, p = tt_anim_buf + 64, q = tt_scale_buf; i < 25;
         i++, p++, q++) {
        q->unk4 = (i / 5) * 72 - 92;
        q->unk6 = (i % 5) * 72 - 72;
        q->unk0 = 4096;
        q->unk2 = 4096;
        p->unk00 = 1;
        p->unk01 = 0;
        p->unk44 = 8;
        p->unk3B = 128;
        p->unk48 = (u32)(unsigned long)q;
        p->unk4D = 0;
        p->unk4E = 1;
        p->unk3A = 64;
        p->unk39 = 64;
        p->unk38 = 64;
        p->unk50 = 0;
        p->unk14 = q->unk4 - 36;
        p->unk16 = q->unk6 - 36;
        /* one source line (a macro in the original?): as1 tie-break */
        p->unk46 = 56; p->unk47 = 184;
        p->unk4F = arg1;
        p->unk3C = 72;
        p->unk3E = 72;
        p->unk40 = 10;
        p->unk42 = (arg0 & 15) + (((arg0 >> 4) + 240) << 6);
        p->unk4C = 0;
        p->unk10 = 13;
    }
    for (i = 0; i < 4; i++, p++) {
        p->unk00 = 1;
        p->unk01 = 0;
        p->unk44 = 8;
        if (i & 1) {
            p->unk14 = 88;
        } else {
            p->unk14 = -96;
        }
        if (i >> 1) {
            p->unk16 = 184;
        } else {
            p->unk16 = 48;
        }
        p->unk3C = 8;
        p->unk3E = 8;
        p->unk46 = (i << 3) + 64;
        p->unk47 = 24;
        p->unk40 = 10;
        p->unk42 = 15373;
        p->unk3B = 101;
        p->unk10 = 0;
    }
    for (i = 0; i < 2; i++, p += 3) {
        p->unk00 = 1;
        p->unk01 = 0;
        p->unk44 = 8;
        p->unk14 = -88;
        p->unk16 = ((i == 0) ? 1 : 2) + i * 140 + 48;
        p->unk3C = 176;
        p->unk3E = 1;
        p->unk38 = 248;
        p->unk39 = 216;
        p->unk3A = 0;
        p->unk10 = 0;
        p->unk3B = 97;
        p++;
        p->unk00 = 1;
        p->unk01 = 0;
        p->unk44 = 8;
        p->unk14 = -88;
        p->unk16 = ((i == 0) ? 2 : 1) + i * 140 + 48;
        p[1].unk16 = (i * 140 + 48);
        p->unk3C = 176;
        p->unk3E = 1;
        p->unk38 = 248;
        p->unk39 = 168;
        p->unk3A = 0;
        p->unk10 = 0;
        p->unk3B = 97;
        p[1].unk00 = 1;
        p[1].unk01 = 0;
        p[1].unk44 = 8;
        p[1].unk14 = -88;
        p[1].unk3C = 176;
        p[1].unk3E = 4;
        p[1].unk38 = 192;
        p[1].unk39 = 112;
        p[1].unk3A = 0;
        p[1].unk10 = 0;
        p[1].unk3B = 97;
    }

    for (i = 0; i < 2; i++, p += 3) {
        p->unk00 = 1;
        p->unk01 = 0;
        p->unk44 = 8;
        p->unk14 = ((i == 0) ? 1 : 2) + i * 188 - 96;
        p->unk16 = 56;
        p->unk3C = 1;
        p->unk3E = 128;
        p->unk38 = 248;
        p->unk39 = 216;
        p->unk3A = 0;
        p->unk10 = 0;
        p->unk3B = 97;
        p++;
        p->unk00 = 1;
        p->unk01 = 0;
        p->unk44 = 8;
        p->unk14 = ((i == 0) ? 2 : 1) + i * 188 - 96;
        p[1].unk14 = (i * 188 - 96);
        p->unk16 = 56;
        p->unk3C = 1;
        p->unk3E = 128;
        p->unk38 = 248;
        p->unk39 = 168;
        p->unk3A = 0;
        p->unk10 = 0;
        p->unk3B = 97;
        p[1].unk00 = 1;
        p[1].unk01 = 0;
        p[1].unk44 = 8;
        p[1].unk16 = 56;
        p[1].unk3C = 4;
        p[1].unk3E = 128;
        p[1].unk38 = 192;
        p[1].unk39 = 112;
        p[1].unk3A = 0;
        p[1].unk10 = 0;
        p[1].unk3B = 97;
    }

    p->unk14 = -94;
    p->unk00 = 1;
    p->unk01 = 0;
    p->unk16 = 50;
    p->unk3C = 188;
    p->unk3E = 140;
    p->unk3A = 0;
    p->unk39 = 0;
    p->unk38 = 0;
    p->unk10 = 12;
    p->unk3B = 99;
    switch (arg0) {
    case 12:
        hit[19] = 123;
        hit[20] = 213;
        hit[21] = 148;
        break;
    case 13:
        hit[19] = 0;
        hit[20] = 205;
        hit[21] = 238;
        break;
    case 14:
        hit[19] = 255;
        hit[20] = 156;
        hit[21] = 205;
        break;
    }
}
