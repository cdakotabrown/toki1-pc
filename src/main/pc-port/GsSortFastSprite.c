/*
 * GsSortFastSprite -- PC pointer-width adapter for matched/GsSortFastSprite.c.
 * Statement-for-statement matched body; only host pointer width diverges.
 * GsOUT_PACKET_P is a four-byte PS1 handle, never a native pointer cell.
 * Ordering-table records remain 12 bytes, with org at +4 and offset at +8.
 * Handles are zero-extended at use sites; packet links retain PS1 width.
 * See mv_state.h for the host-adaptation doctrine.
 */
#include "types.h"

extern u32 GsOUT_PACKET_P;  /* current prim buffer pointer */
extern u16 GsORGOFSX;   /* global X offset */
extern u16 GsORGOFSY;   /* global Y offset */

typedef struct PrimSpec {
    u32 flags;    /* +0  */
    u16 u04;      /* +4  */
    u16 u06;      /* +6  */
    u16 u08;      /* +8  */
    u16 u0A;      /* +10 */
    u16 u0C;      /* +12 */
    u8  u0E;      /* +14 */
    u8  u0F;      /* +15 */
    s16 u10;      /* +16 */
    s16 u12;      /* +18 */
    u8  u14;      /* +20 */
    u8  u15;      /* +21 */
    u8  u16;      /* +22 */
} PrimSpec;

typedef struct SlotHdr {
    u8   pad0[4]; /* +0 */
    u32  p04;     /* +4 */
    s32  v08;     /* +8 */
} SlotHdr;

void GsSortFastSprite(PrimSpec *spec, SlotHdr *slot, u16 idx) {
    u32 flags;
    u32 *p;
    u32 *ot;
    u16 x, y;

    flags = spec->flags;
    if ((s32)flags < 0) {
        return;
    }
    if (spec->u08 == 0) {
        return;
    }
    if (spec->u0A == 0) {
        return;
    }
    p = (u32 *)(unsigned long)GsOUT_PACKET_P;
    x = spec->u04 + GsORGOFSX; y = spec->u06 + GsORGOFSY;
    p[1] = (spec->u0C & 0x1F) | 0xE1000200 | ((flags >> 17) & 0x180) | ((flags >> 23) & 0x60);
    p[2] = ((flags >> 5) & 0x02000000) | 0x64000000 | ((flags << 18) & 0x01000000)
         | ((u32)spec->u16 << 16) | (spec->u15 << 8) | spec->u14;
    p[3] = x | ((u32)y << 16);
    p[4] = spec->u0E | (spec->u0F << 8) | ((u32)spec->u12 << 22) | ((spec->u10 << 12) & 0x3F0000);
    p[5] = spec->u08 | ((u32)spec->u0A << 16);
    ot = idx + (u32 *)(unsigned long)slot->p04 - slot->v08;
    p[0] = *ot + 0x05000000;
    *ot = (u32)(unsigned long)p & 0x00FFFFFF;
    GsOUT_PACKET_P = (u32)(unsigned long)(p + 6);
}
