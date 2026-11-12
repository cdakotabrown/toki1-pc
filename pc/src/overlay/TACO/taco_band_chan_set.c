/* SPDX-License-Identifier: GPL-3.0-or-later */
/* taco_band_chan_set -- PC override of src/overlay/TACO/taco_band_chan_set.c.
 *
 * Statement for statement the matched function; the divergence is the
 * TABLE WIDTH (the src/main/pc-port/basyo_init.c doctrine).  The matched C
 * takes `s5 = &taco_node_prim_tbl[a0]` through an `s32 **` and reads s5[0] /
 * s5[1].  taco_node_prim_tbl is a table of 4-byte PS1 packet-range handles in
 * TACO's data (written 4 bytes at a time by rec_word_copy through
 * taco_node_prim_tbl_1), so on LP64 the native walk starts at cell 2*a0 and reads
 * two cells fused into one 8-byte pointer.  generate_overlay_modules.py
 * cannot widen this form (the address of a cell, not an access; see
 * ps1_pointer_cells.py), so the table is read here as u32 cells and each
 * handle is used as the PS1 address it holds.
 *
 * @DivergesFrom: src/overlay/TACO/taco_band_chan_set.c -- table cell width only.
 * Test: pc/tools/test_ps1_handle_records.py.
 */
#include "types.h"

/* taco_node_prim_tbl: PS1 handles of s32 packet words */
extern u32 taco_node_prim_tbl[];

extern s32 func_801542EC();

/* the packet word at a PS1 handle */
#define BAND_CHAN_PKT(h) ((s32 *)(unsigned long)(h))

void taco_band_chan_set(a0, a1)
s32 a0;
u16 a1;
{
    s32 s1;
    s32 s2;
    s32 s3;
    s32 s4;
    u32 *s5;
    s32 *s0;
    s32 *end;
    s32 v0;
    s32 t1;
    s32 t2;
    s32 val;

    s1 = 0x24000507;
    s2 = 0x34000609;
    s3 = 0x24000014;
    s4 = 0x01E00000;
    s5 = &taco_node_prim_tbl[a0];
    s0 = BAND_CHAN_PKT(s5[0]);
    do {
        v0 = func_801542EC(s0);
        if (v0 == 0) {
            break;
        }
        t1 = a1 & 0x1F;
        v0 = s0[0];
        t2 = t1 << 16;
        if (s1 == v0 || s2 == v0 || s3 == v0) {
            val = s0[2];
            s0 += 2;
            s0[0] = (val & 0xFFFF) + (val & s4) + t2;
        }
        end = BAND_CHAN_PKT(s5[1]);
        s0 += 1;
    } while ((u32)s0 < (u32)end);
}
