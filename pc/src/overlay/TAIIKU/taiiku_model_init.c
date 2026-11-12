/* SPDX-License-Identifier: GPL-3.0-or-later */
/* taiiku_model_init -- PC override of src/overlay/TAIIKU/taiiku_model_init.c
 * (0x80140880).
 *
 * Statement for statement the matched function; the divergence is the
 * RECORD WIDTH (the src/main/pc-port/basyo_init.c doctrine).  `object` is
 * the PS1 GsDOBJ2 array at 0x8012AB20 (16-byte records: attribute,
 * coord2, tmd, id) that GsSortObject4 reads with PS1 widths.  The matched
 * Slot declares the coord2 cell as `u8 *rec`: on LP64 the record is 24
 * bytes and rec an 8-byte store at +8, so the loop strided 24, wrote each
 * coordinate address over the tmd cell and the next record, and left the
 * coord2 cells it should set holding whatever was there.  In the sports
 * festival's ball rolling (TAMA2.TMD, 3 Jun 95; playthrough
 * spring_events.txt) GsSortObject4 then built the local-screen matrix from
 * a garbage coordinate: TR = (0, -1523583766, -1524632344) in the GTE
 * dump, every TG3 face flagged (SZ saturated at 0), no ball drawn.  Here
 * coord2 is the u32 PS1 address of coord_buf[i].
 *
 * @DivergesFrom: src/overlay/TAIIKU/taiiku_model_init.c -- record field
 * width only.
 */
#include "types.h"

typedef struct {
    s32 flag;
    u32 rec;            /* coord2: PS1 address (u8 * on the console) */
    s32 pad[2];
} Slot;

extern Slot object[];
extern u8 coord_buf[];

extern void GsLinkObject4(s32 *a0, Slot *a1, s32 a2);
extern void GsMapModelingData(s32 *a0);

void taiiku_model_init(s32 *arg0)
{
    s32 *base;
    s32 *outer;
    s32 i;

    base = arg0 + 1;
    GsMapModelingData(base);
    outer = base;
    outer += 2;
    for (i = 0; i < 16; i++) {
        GsLinkObject4(outer, &object[i], 0);
        object[i].rec = (u32)(unsigned long)(coord_buf + i * 80);
        object[i].flag = 0x80000000;
    }
}
