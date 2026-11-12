/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tt_text_disp: retain retail pointer cells and double-buffer stride.
 * @DivergesFrom: src/overlay/TT/tt_text_disp.c
 * Only pointer storage differs: DB cursor (and board texture cells)
 * are u32, widened at use. All packet writes, helper calls and
 * ordering-table links follow the matched PS1 function.
 */
#include "tt_draw_buffer.h"

typedef TtDrawTag Tag;

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
    u8  flag;           /* 0x00 */
    u8  pad1[5];
    u16 bits;           /* 0x06 */
    u8  pad8[12];
} Ent;                  /* 20 */

extern TTDisp *tt_disp_wk;
extern Ent    *tt_text_buf;
extern DB     *tt_db_buf;

#define addPrim(ot, p) ((p)->tag.addr = (ot)->addr, (ot)->addr = (u32)(p))

void tt_text_disp()
{
    s32 unused0;        /* two declared, never used locals: the 80-byte frame */
    s32 unused1;
    DB *db;
    Ent *e;
    DrMode *dm;
    s32 i;

    db = &tt_db_buf[tt_disp_wk->buf];
    e = tt_text_buf;
    for (i = 0; i < 128; i++) {
        if (e->flag && !(e->bits & 1)) {
            tt_qstr_disp(e);
        }
        e++;
    }
    dm = (DrMode *)(unsigned long)db->prim_addr;
    db->prim_addr += sizeof(DrMode);
    SetDrawMode(dm, 0, 0, 10, 0);
    addPrim(&db->ot[2], dm);
}
