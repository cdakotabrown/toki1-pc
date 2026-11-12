/* SPDX-License-Identifier: GPL-3.0-or-later */
/* taco_band_bright -- PC override of src/overlay/TACO/taco_band_bright.c.
 *
 * Statement for statement the matched function; the divergence is the
 * TABLE WIDTH (the src/main/pc-port/basyo_init.c doctrine).  The matched C
 * takes `base = &taco_node_prim_tbl[idx]` through a `u32 **` and reads base[0] /
 * base[1].  taco_node_prim_tbl is a table of 4-byte PS1 packet-range handles in
 * TACO's data (written 4 bytes at a time by rec_word_copy through
 * taco_node_prim_tbl_1), so on LP64 the native walk starts at cell 2*idx and reads
 * two cells fused into one 8-byte pointer.  generate_overlay_modules.py
 * cannot widen this form (the address of a cell, not an access; see
 * ps1_pointer_cells.py), so the table is read here as u32 cells and each
 * handle is used as the PS1 address it holds.
 *
 * @DivergesFrom: src/overlay/TACO/taco_band_bright.c -- table cell width only.
 * Test: pc/tools/test_ps1_handle_records.py.
 */
#include "types.h"

extern s32 func_801542EC();
/* taco_node_prim_tbl: PS1 handles of u32 packet words */
extern u32 taco_node_prim_tbl[];

/* the packet word at a PS1 handle */
#define BAND_BRIGHT_PKT(h) ((u32 *)(unsigned long)(h))

void
taco_band_bright(s32 idx, s32 ofs)
{
    u32 *base;
    u32 *p;
    u32 c;
    s32 r;
    s32 g;
    s32 b;

    base = &taco_node_prim_tbl[idx];
    p = BAND_BRIGHT_PKT(base[0]);
    do {
        if (func_801542EC(p) == 0) {
            return;
        }
        c = *p;
        if (c == 0x21010001 || c == 0x21010002 || c == 0x21010304) {
            c = p[1];
            p++;
            r = ((c >> 16) & 0xFF) + ofs;
            if (r >= 256) {
                r = 255;
            }
            g = ((c >> 8) & 0xFF) + ofs;
            if (g >= 256) {
                g = 255;
            }
            b = (c & 0xFF) + ofs;
            if (b >= 256) {
                b = 255;
            }
            *p = (c & 0xFF000000) + ((r & 0xFF) << 16) +
                 ((g & 0xFF) << 8) + (b & 0xFF);
        }
        p++;
    } while (p < BAND_BRIGHT_PKT(base[1]));
}
