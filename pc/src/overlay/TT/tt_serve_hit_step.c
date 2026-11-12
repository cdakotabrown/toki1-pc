/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tt_serve_hit_step: keep 20-byte unit records and enclosing retail offsets.
 * @DivergesFrom: src/overlay/TT/tt_serve_hit_step.c -- shared four-byte script cells
 * replace native pointers. State transitions and script semantics are unchanged.
 */
#include "types.h"
#include "tt_serve_unit.h"

typedef TtServeUnit Unit;

typedef struct {
    u8  b0;         /* 0x000 */
    u8  pad001[55];
    Unit unit[24];  /* 0x038 */
    u8  pad218[246];
    u16 h782;       /* 0x30E */
    u8  pad310[50];
    u8  b834;       /* 0x342 */
    u8  b835;       /* 0x343 */
} Serve;

typedef struct {
    u8  pad00[66];
    s16 unk42;      /* 0x42 */
    u8  pad44[32];
} TTRec;            /* 100 */

typedef struct {
    u8  pad[3836];
} Board;

extern Serve *tt_boss_wk;
extern TTRec *tt_anim_buf;
extern Board *tt_board_ptr;

void tt_serve_hit_step()
{
    Serve *p;
    Board *q;
    s32 i;
    TTRec *r;

    p = tt_boss_wk;
    q = tt_board_ptr;
    if (p->b0 == 0) {
        return;
    }
    for (i = 0; i < 24; i++) {
        p->unit[i].b10 = 0;
    }
    if (p->b834 == 0) {
        if (tt_hit_check() != 0) {
            tt_cmd_once(1288);
            for (i = 0, r = &tt_anim_buf[40]; i < 7; i++, r--) {
                r->unk42 = 15583;
            }
            for (i = 0, r = &tt_anim_buf[48]; i < 7; i++, r--) {
                r->unk42 = 15583;
            }
        } else {
            for (i = 0, r = &tt_anim_buf[40]; i < 7; i++, r--) {
                r->unk42 = 15577;
            }
            for (i = 0, r = &tt_anim_buf[48]; i < 7; i++, r--) {
                r->unk42 = 15577;
            }
        }
    }
    if (p->b835 == 0) {
        tt_board_hit(q++);
        tt_board_hit(q);
    }
    if (p->h782 == 1) {
        tt_slot_hit();
        tt_body_hit_scan();
    } else if (p->h782 == 2) {
        tt_body_hit_scan();
    }
}
