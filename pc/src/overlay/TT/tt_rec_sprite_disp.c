/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tt_rec_sprite_disp: preserve the retail drawing-buffer layout.
 * @DivergesFrom: src/overlay/TT/tt_rec_sprite_disp.c
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
    u8  pad00[16];
    u16 pri;            /* 0x10 */
    u8  pad12[2];
    s16 x;              /* 0x14 */
    s16 y;              /* 0x16 */
    u8  pad18[32];
    u8  r;              /* 0x38 */
    u8  g;              /* 0x39 */
    u8  b;              /* 0x3A */
    u8  code;           /* 0x3B */
    s16 w;              /* 0x3C */
    s16 h;              /* 0x3E */
    u16 tpage;          /* 0x40 */
    u16 clut;           /* 0x42 */
    u16 bits;           /* 0x44 */
    u8  u;              /* 0x46 */
    u8  v;              /* 0x47 */
} Rec;

extern u8     *tt_step_wk;
extern TTDisp *tt_disp_wk;
extern DB     *tt_db_buf;

#define addPrim(ot, p) ((p)->tag.addr = (ot)->addr, (ot)->addr = (u32)(p))

void tt_rec_sprite_disp(Rec *r)
{
    s32 unused0;        /* declared, never used: moves the db spill to sp+56 */
    DB *db;
    Sprt *p;
    DrMode *dm;
    u8 *pal;

    db = &tt_db_buf[tt_disp_wk->buf];
    p = (Sprt *)(unsigned long)db->prim_addr;
    db->prim_addr += sizeof(Sprt);
    p->tag.len = 4;
    if (r->bits & 8) {
        pal = tt_step_wk;
        p->code = r->code & ~1;
        p->r0 = pal[29];
        p->g0 = pal[30];
        p->b0 = pal[31];
    } else {
        p->code = r->code;
        p->r0 = r->r;
        p->g0 = r->g;
        p->b0 = r->b;
    }
    p->x0 = r->x;
    p->y0 = r->y;
    p->w = r->w;
    p->h = r->h;
    p->u0 = r->u;
    p->v0 = r->v;
    p->clut = r->clut;
    addPrim(&db->ot[r->pri + 2], p);
    dm = (DrMode *)(unsigned long)db->prim_addr;
    db->prim_addr += sizeof(DrMode);
    SetDrawMode(dm, 0, 0, r->tpage, 0);
    addPrim(&db->ot[r->pri + 2], dm);
}
