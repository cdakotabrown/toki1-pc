/*
 * mod_trans_vram -- PC override of src/main/matched/mod_trans_vram.c (0x8004D728).
 *
 * Statement for statement the matched function; the divergence is the
 * RECORD WIDTH (the src/main/pc-port/basyo_init.c doctrine).  mod_w (0x80129C80) holds 20-byte records.
 * On the console the record's pointer members are 4-byte cells; the
 * matched C declares them as pointers, 8 bytes (and 8-aligned) on LP64,
 * so every later member moves, the record stride grows and the four mod_w
 * readers/writers (mod_init, mod_set, mod_set_gpu's all-s32 view,
 * mod_trans_vram) and srn_init / srn_vram_set no longer agree on one
 * layout.  Here the pointer members are u32 PS1 addresses, zero-extended
 * where they are used (pc/tools/audit_ps1_pointer_casts.py RECORD-PTR).
 *
 * @DivergesFrom: src/main/matched/mod_trans_vram.c -- pointer members as u32.
 */
#include "types.h"

typedef struct {
    u32 ptr0;      /* offset 0: PS1 address of a table of PS1 pointers */
    u32 ptr4;      /* offset 4: PS1 address of the tile data */
    u8 type;       /* offset 8 */
    u8 pad[3];     /* offset 9-11 */
    u8 tw;         /* offset 12: tiles wide */
    u8 th;         /* offset 13: tiles high */
    u8 tileSz;     /* offset 14: tile pixel size */
    u8 pad2;       /* offset 15 */
    u32 data;      /* offset 16 */
} TileEntry;

typedef char tile_entry_ps1_size[sizeof(TileEntry) == 20 ? 1 : -1];

extern TileEntry mod_w[];

extern void get_fnt(u8 *, u8 *, s32, s32, s32, s32);
extern void load_image_rect(u16, u16, u16, u16, u8 *);

void mod_trans_vram(s32 idx, u32 tileId, s32 destX, s32 destY, s32 arg4) {
    s32 pad1;
    u32 hi;
    u32 row;
    TileEntry *entry;
    u32 col;
    u32 *srcPtr;
    u32 word;
    u32 m3;
    u8 buf[248];
    u8 cs;
    u32 tw;
    u8 th;
    s32 csVal;
    s32 tileSize;
    u16 w, h, y;

    entry = &mod_w[idx];

    if (entry->tileSz == 8) {
        tileSize = 8;
    } else {
        tileSize = 16;
    }

    switch (entry->type) {
    case 0: cs = 1; break;
    case 1: cs = 2; break;
    case 2: cs = 2; break;
    case 3: cs = 1; break;
    case 4: cs = 1; break;
    case 5: cs = 1; break;
    default: cs = 1; break;
    }

    th = entry->th;
    tw = entry->tw;
    srcPtr = th * tileId * tw + (u32 *)(unsigned long)entry->ptr4;

    row = 0;
    if (th != 0) {
      do {
        col = 0;
        tw = entry->tw;
        if (tw != 0) {
          csVal = cs;
          w = (u32)(tileSize / 2) / cs;
          h = tileSize;
          y = ((destX & 0x10) << 4) + ((destX & 0x20) << 2) + arg4 + row * tileSize;
          do {
            word = *srcPtr;
            m3 = word & 0xFF0000;
            m3 >>= 16;
            hi = word & 0xFF000000;
            get_fnt(buf, (u8 *)(unsigned long)((u32 *)(unsigned long)entry->ptr0)[hi >> 24], m3, csVal, tileSize, word & 0xFF);
            load_image_rect(((destX & 0xF) << 6) + (u32)(destY / 2) / cs + (u32)((s32)(col * tileSize) / 2) / cs, y, w, h, buf);
            srcPtr++;
            col++;
            tw = entry->tw;
          } while (col < tw);
        }
        row++;
        th = entry->th;
      } while (row < th);
    }
}
