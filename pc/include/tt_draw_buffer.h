/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Retail TT double buffer: pointer cells stay four bytes on every host.
 * Packet layouts and ordering-table tags are unchanged. Only widen the
 * packet cursor when accessing it; never store a native pointer in the DB.
 */
#ifndef PC_TT_DRAW_BUFFER_H
#define PC_TT_DRAW_BUFFER_H

#include "types.h"

typedef struct {
    u32 addr : 24;
    u32 len : 8;
} TtDrawTag;

typedef struct {
    u8 pad0[4];
    u32 prim_addr;              /* +0x0004 */
    u8 pad8[0x7FF8];
    TtDrawTag ot[46];           /* +0x8000, stride 32952 */
} TtDrawBuffer;

#endif
