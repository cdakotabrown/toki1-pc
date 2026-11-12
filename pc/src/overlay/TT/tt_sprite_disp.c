/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tt_sprite_disp: preserve the retail drawing-buffer layout.
 * @DivergesFrom: src/overlay/TT/tt_sprite_disp.c
 * The four-byte cursor is widened only at use. Packet writes,
 * record selection, colors and ordering-table links are unchanged.
 */
#include "tt_draw_buffer.h"

typedef TtDrawTag Tag;

typedef struct {
    Tag tag;
    u8  r0, g0, b0, code;
    s16 x0, y0;
    u8  u0, v0;
    u16 clut;
    s16 w, h;
} Sprt;

typedef struct {
    Tag tag;
    u32 code[2];
} DrMode;

typedef TtDrawBuffer DB;

typedef struct {
    u8  pad0[8];
    s32 buf;            /* 0x08 */
} TTDisp;

typedef struct {
    u8  flag;        /* 0x00 */
    u8  pad1[15];
    u16 pri;         /* 0x10 */
    u8  pad12[16];
    s16 x;           /* 0x22 */
    u8  pad24[2];
    s16 y;           /* 0x26 */
    u8  pad28[22];
    u16 tpage;       /* 0x3E */
    u16 clut;        /* 0x40 */
    u8  u;           /* 0x42 */
    u8  v;           /* 0x43 */
    u8  pad44[16];
} Rec;                  /* 84 */

extern TTDisp *tt_disp_wk;
extern Rec    *tt_player_shot_buf;
extern DB     *tt_db_buf;

#define addPrim(ot, p) ((p)->tag.addr = (ot)->addr, (ot)->addr = (u32)(p))

void tt_sprite_disp()
{
    DB *db;
    Rec *r;
    Sprt *p;
    DrMode *dm;
    s32 i;

    db = &tt_db_buf[tt_disp_wk->buf];
    r = tt_player_shot_buf;
    for (i = 0; i < 12; i++) {
        if (r->flag) {
            p = (Sprt *)(unsigned long)db->prim_addr;
            db->prim_addr += sizeof(Sprt);
            p->tag.len = 4, p->code = 101;
            p->x0 = r->x - 8;
            p->y0 = r->y - 8;
            p->h = 16;
            p->w = 16;
            p->u0 = r->u;
            p->v0 = r->v;
            p->clut = r->clut;
            addPrim(&db->ot[r->pri + 2], p);
            dm = (DrMode *)(unsigned long)db->prim_addr;
            db->prim_addr += sizeof(DrMode);
            SetDrawMode(dm, 0, 0, r->tpage, 0);
            addPrim(&db->ot[r->pri + 2], dm);
        }
        r++;
    }
}
