/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Shared retail unit layout used by TT's serve and scripted sequences. */
#ifndef TT_SERVE_UNIT_H
#define TT_SERVE_UNIT_H

#include "types.h"

typedef struct {
    u32 script_addr;  /* +0: stored PS1 address, widened only at use */
    u8 b4;
    u8 b5;
    u8 b6;
    u8 pad07;
    u8 b8;
    u8 b9;
    u8 b10;
    u8 b11;
    s16 h12;
    s16 h14;
    u8 b16;
    u8 pad11[3];
} TtServeUnit;

typedef char TtServeUnitSizeCheck[sizeof(TtServeUnit) == 20 ? 1 : -1];

#endif
