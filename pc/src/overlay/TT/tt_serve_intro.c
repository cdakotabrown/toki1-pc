/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tt_serve_intro: keep 20-byte unit records and enclosing retail offsets.
 * @DivergesFrom: src/overlay/TT/tt_serve_intro.c -- shared four-byte script cells
 * replace native pointers. State transitions and script semantics are unchanged.
 */
#include "types.h"
#include "tt_serve_unit.h"

typedef TtServeUnit Unit;

typedef struct {
    u8  b0;         /* 0x000 */
    u8  pad001[3];
    u16 h4;         /* 0x004 */
    u16 h6;         /* 0x006 */
    u16 h8;         /* 0x008 */
    u8  pad00A[46];
    Unit unit[24];  /* 0x038 */
    s32 w536;       /* 0x218 */
    u8  pad21C[44];
    s16 h584[24];   /* 0x248 */
    s16 h632[24];   /* 0x278 */
    s16 h680[24];   /* 0x2A8 */
    s16 h728[4];    /* 0x2D8 */
    u8  pad2E0[44];
    s16 h780;       /* 0x30C */
    s16 h782;       /* 0x30E */
    u8  b784[24];   /* 0x310 */
    u8  pad328[28];
    u8  b836;       /* 0x344 */
    u8  pad345[31];
    u16 h868;       /* 0x364 */
    u8  pad366[110];
    u16 h980;       /* 0x3D4 */
} Serve;

extern Serve *tt_boss_wk;
extern u8     tt_anim_tbl[];
extern s16    tt_serve_intro_scr[];
extern u8     tt_flash_seq_tbl[];
extern u8     tt_puff_seq_tbl[];

void tt_serve_intro()
{
    Serve *p;
    s32 i;
    s32 n;
    s32 t;

    p = tt_boss_wk;
    switch (p->h8) {
    case 0:
        tt_cmd_once(1281);
        p->b836 = 1;
        tt_shape_clear();
        p->h868 = 0;
        p->h980 = 0;
        for (i = 8; i < 16; i++) {
            p->unit[i].b8 = 128;
        }
        for (i = 16; i < 19; i++) {
            p->unit[i].script_addr = (u32)(unsigned long)tt_anim_tbl;
            p->unit[i].b4 = 0;
            p->unit[i].b5 = 4;
            p->unit[i].b10 = 0;
            p->unit[i].b16 = 0;
            p->b784[i] = 0;
            p->h680[i] = 7;
            p->h584[i] = 0;
            p->h632[i] = 0;
        }
        p->h8 = 64;
        break;
    case 64:
        t = tt_serve_intro_scr[tt_boss_wk->h980];
        if (t != -1 && t == tt_boss_wk->h868) {
            tt_cmd_once(1281);
            n = tt_serve_intro_scr[p->h980 + 4];
            p->b784[n] = 1;
            p->unit[n].b8 = 128;
            p->h584[n] = tt_serve_intro_scr[p->h980 + 1];
            p->h632[n] = tt_serve_intro_scr[p->h980 + 2];
            p->h680[n] = tt_serve_intro_scr[p->h980 + 3];
            p->h980 += 5;
        }
        for (i = 8; i < 16; i++) {
            tt_seq_step(tt_flash_seq_tbl, i);
        }
        for (i = 16; i < 19; i++) {
            tt_seq_step(tt_puff_seq_tbl, i);
        }
        if (p->h868 == 133) {
            for (i = 0; i < 16; i++) {
                p->b784[i] = 0;
            }
        }
        if (p->h868 == 156) {
            for (i = 0; i < 24; i++) {
                p->b784[i] = 0;
            }
            p->h8 = 0;
            p->h4 = 160;
        }
        break;
    }
    p->h868++;
}
