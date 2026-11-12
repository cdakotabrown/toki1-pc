/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Native libgs background contracts. Stored records retain PS1 widths;
 * handler metadata below is host-only and may contain native pointers.
 * MATRIX/VECTOR come from the HAL, never the SDK's LP64 long typedefs.
 */
#ifndef PC_BG_RENDER_H
#define PC_BG_RENDER_H

#include "hal.h"
#include <stddef.h>

typedef struct {
    unsigned char u, v;
    unsigned short cba;
    unsigned short flag;
    unsigned short tpage;
} GsCELL;                       /* 8 bytes, pointer-free */

typedef struct {
    unsigned char cellw, cellh;
    unsigned short ncellw, ncellh;
    u32 base;                   /* PS1 handle -> GsCELL[] */
    u32 index;                  /* PS1 handle -> u16[] */
} GsMAP;                        /* 16 bytes */

typedef struct {
    u32 attribute;              /* 0 */
    short x, y;                 /* 4 */
    short w, h;                 /* 8 */
    short scrollx, scrolly;     /* 12 */
    unsigned char r, g, b;      /* 16 */
    unsigned char pad1;         /* 19 */
    u32 map;                    /* 20: PS1 handle -> GsMAP */
    short mx, my;               /* 24 */
    short scalex, scaley;       /* 28 */
    s32 rotate;                 /* 32 */
} GsBG;                         /* 36 bytes = the caller's 0x24 stride */

typedef struct {
    u8  code;        /* 0x00 */
    u8  r, g, b;     /* 0x01 */
    u16 tpage;       /* 0x04 */
    u16 clut;        /* 0x06 */
    u8  u;           /* 0x08 */
    u8  v;           /* 0x09 */
    s16 x;           /* 0x0A */
    s16 y;           /* 0x0C */
    u16 w;           /* 0x0E */
    s16 h;           /* 0x10 */
    s16 mx;          /* 0x12 */
    s16 my;          /* 0x14 */
    u16 flag;        /* 0x16 */
    s32 useMatrix;   /* 0x18 */
} BgCellParams;

typedef struct {
    u32  length;
    u32  org;        /* PS1 handle -> u32[] */
    s32  offset;
} SortOT;           /* 12-byte read view, not the full 20-byte GsOT. */

enum {
    BG_PACKET_NORMAL,
    BG_PACKET_TRANSFORMED,
    BG_PACKET_HANDLER_COUNT,
    BG_PACKET_PAYLOAD_WORDS = 9
};

typedef void (*BgPacketHandler)(u8 *, BgCellParams *, MATRIX *);
typedef struct {
    const char *name;
    u32 payload_words;
    int uses_matrix;
    BgPacketHandler emit;
} BgPacketHandlerInfo;

#ifdef __cplusplus
extern "C" {
#endif

extern const BgPacketHandlerInfo bg_packet_handlers[BG_PACKET_HANDLER_COUNT];

void GsSortBg(GsBG *bg, SortOT *ot, u16 priority);
void _mk_spr_packet(u8 *out, BgCellParams *params, MATRIX *matrix,
                    GsCELL *cell);
void _mk_normsp(u8 *out, u8 *params);
void _mk_xpndsp(u8 *out, u8 *params, void *matrix);

#ifdef __cplusplus
}
#endif

typedef char pc_bg_cell_size[sizeof(GsCELL) == 8 ? 1 : -1];
typedef char pc_bg_map_size[sizeof(GsMAP) == 16 ? 1 : -1];
typedef char pc_bg_size[sizeof(GsBG) == 36 ? 1 : -1];
typedef char pc_bg_map_offset[offsetof(GsBG, map) == 20 ? 1 : -1];
typedef char pc_bg_params_size[sizeof(BgCellParams) == 28 ? 1 : -1];
typedef char pc_bg_handler_offset[
    offsetof(BgCellParams, useMatrix) == 24 ? 1 : -1];
typedef char pc_bg_ot_size[sizeof(SortOT) == 12 ? 1 : -1];
typedef char pc_bg_matrix_size[sizeof(MATRIX) == 32 ? 1 : -1];
typedef char pc_bg_matrix_translation[offsetof(MATRIX, t) == 20 ? 1 : -1];

#endif
