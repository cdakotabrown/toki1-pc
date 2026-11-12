/*
 * GsSortBg -- background/map sprite sorter (PC override of
 * src/main/matched/GsSortBg.c).
 *
 * Statement for statement the matched function; the divergence is the
 * STRUCT WIDTHS (src/main/pc-port/mv_state.h doctrine -- the pointer width
 * is a property of the host, not of the game).  The caller (vblank_function)
 * hands over PS1-LAYOUT data with hardcoded PS1 arithmetic:
 *
 *     GsSortBg((s32)&Bg + (i * 0x24), (s32)&ot_work + mulVal, ...)
 *
 * -- a 36-byte (0x24) GsBG stride.  The matched file's own typedefs inflate
 * on LP64 (unsigned long/long = 8 bytes; GsMAP's pointers 8 bytes), so the
 * native walk read fused words at +0x18 (the map pointer) and every cell
 * lookup ran wild.  Here the structs carry u32 PS1 pointer HANDLES at
 * exactly the console's offsets, and the handles are zero-extended where
 * they are dereferenced.  Measured first-fail class: GsSortBg+0x277 x147,
 * GsSortBg+0x41 x9 (scene matrix 2026-10-19).
 *
 * bg_render.h checks the stored record sizes and supplies the HAL's
 * 32-byte MATRIX. Packet builders receive their arguments explicitly;
 * no call relies on the host compiler preserving MIPS argument registers.
 * GsOUT_PACKET_P remains a four-byte PS1 handle, widened only at use.
 *
 * The matched header follows.
 */
#include "bg_render.h"

extern u32 GsOUT_PACKET_P;     /* PS1-width packet cursor cell */
extern MATRIX GsIDMATRIX;

extern void gte_rotate_z_matrix(MATRIX *m, s32 rotate);
extern void gte_scale_matrix(MATRIX *m, s16 sx, s16 sy, s32 sz);

void GsSortBg(GsBG *bg, SortOT *otp, u16 pri)
{
    MATRIX mtx;
    BgCellParams params;
    u32 attr;
    u16 blendBits;
    s32 clipW, clipH;
    s32 totalW, totalH;
    u16 tileW, tileH;
    u16 mapW, mapH;
    u8 *pk;
    s32 rowStartX;
    s32 baseY;
    s32 curY;
    s32 screenY;
    s32 tileHi;
    s32 colEndY;
    s32 rowEndX;
    u16 chunkH;
    s32 vOff;
    s32 tileWi;
    s32 mapWm;
    s32 idx;
    u32 *bucket;
    u16 sv;
    s32 th;
    s32 slot;
    GsMAP *map;
    u16 *mapIndex;
    GsCELL *mapBase;
    u32 *otOrg;

    map = (GsMAP *)(unsigned long)bg->map;

    attr = bg->attribute;
    if ((s32)attr < 0) {
        return;
    }
    /* PS1 zero-page semantics: an unset slot (map = 0) reads RAM-zero at
     * +2/+4 on the console and the guards below return.  Address 0 is not
     * mappable on the host (mmap_min_addr = 65536), so the guard is folded
     * to its console outcome here.  (Zero-page class: see PC-PORT-TODO.) */
    if (map == (GsMAP *)0 || map->ncellw == 0) {
        return;
    }
    if (map->ncellh == 0) {
        return;
    }
    mapIndex = (u16 *)(unsigned long)map->index;
    mapBase = (GsCELL *)(unsigned long)map->base;
    otOrg = (u32 *)(unsigned long)otp->org;

    tileW = map->cellw;
    if (tileW == 0) {
        tileW = 0x100;
    }
    tileH = map->cellh;
    if (tileH == 0) {
        tileH = 0x100;
    }
    mapW = map->ncellw;
    mapH = map->ncellh;

    totalW = tileW * mapW;
    totalH = tileH * mapH;
    params.code = (((attr >> 0x1D) & 2) | 0x2C) | ((attr >> 6) & 1);
    params.r = bg->r;
    blendBits = ((attr >> 0x11) & 0x180) | ((attr >> 0x17) & 0x60);
    params.g = bg->g;
    params.b = bg->b;

    if ((attr & 0x08000000) ||
        (*(u32 *)&bg->scalex == 0x10001000 && bg->rotate == 0)) {
        params.useMatrix = 0;
    } else {
        params.useMatrix = 1;
        mtx = GsIDMATRIX;
        gte_rotate_z_matrix(&mtx, bg->rotate);
        gte_scale_matrix(&mtx, bg->scalex, bg->scaley, 0x1000);
    }

    pk = (u8 *)(unsigned long)GsOUT_PACKET_P;
    rowStartX = bg->scrollx % totalW;
    clipW = bg->w;
    do { th = tileH; } while (0);
    clipH = bg->h;
    if (rowStartX < 0) {
        rowStartX += totalW;
    }
    baseY = bg->scrolly % totalH;
    if (baseY < 0) {
        baseY += totalH;
    }
    curY = baseY;
    do { vOff = curY % th; } while (0);
    screenY = bg->y;
    do { do { chunkH = tileH - vOff; } while (0); } while (0);
    if (clipH < chunkH) {
        chunkH = clipH;
    }
    do { mapWm = mapW; } while (0);
    colEndY = curY + clipH;
    tileHi = th;
    rowEndX = rowStartX + clipW;
    params.h = chunkH;
    params.my = -bg->my;
    tileWi = tileW;
    params.y = screenY;

    do {
        s32 withinTileX;
        u16 remCol;
        s32 colPos;
        s32 varS8;
        u16 cell;
        GsCELL *entry;

        withinTileX = rowStartX % tileWi;
        varS8 = bg->x;
        remCol = tileW - withinTileX;
        if (clipW < remCol) {
            remCol = clipW;
        }
        colPos = rowStartX;
        idx = ((colPos % totalW) / tileWi) + (((curY % totalH) / tileHi) * mapWm);
        cell = mapIndex[idx];
        if (cell != 0xFFFF) {
            entry = &mapBase[cell];
            *(u32 *)pk = (u32)(unsigned long)(pk + 0x28);
            do { do { pk[3] = 9; } while (0); } while (0);
            params.tpage = blendBits | (entry->tpage & 0x1F);
            params.clut = entry->cba;
            params.x = varS8;
            if (entry->flag & 2) {
                params.u = entry->u;
            } else {
                params.u = entry->u + withinTileX;
            }
            if (entry->flag & 1) {
                sv = ((tileH + entry->v) - chunkH) - vOff;
            } else {
                do { sv = entry->v + vOff; } while (0);
            }
            params.v = sv;
            params.w = remCol;
            params.mx = (colPos - rowStartX) - bg->mx;
            params.flag = entry->flag;
            _mk_spr_packet(pk + 4, &params, &mtx, entry);
            pk += 0x28;
        }

        while (colPos < rowEndX) {
            s32 step;

            idx++;
            if ((idx % mapWm) == 0) {
                idx -= mapWm;
            }
            step = remCol;
            colPos += step;
            remCol = rowEndX - colPos;
            do { do { varS8 += step; } while (0); } while (0);
            if ((u32)tileWi < remCol) {
                remCol = tileW;
            }
            cell = mapIndex[idx];
            if (cell != 0xFFFF) {
                entry = &mapBase[cell];
                *(u32 *)pk = (u32)(unsigned long)(pk + 0x28);
                pk[3] = 9;
                params.tpage = blendBits | (entry->tpage & 0x1F);
                params.clut = entry->cba;
                params.x = (varS8 - colPos) + rowStartX;
                if (entry->flag & 2) {
                    params.u = (entry->u + tileW) - remCol;
                } else {
                    params.u = entry->u;
                }
                if (entry->flag & 1) {
                    sv = ((tileH + entry->v) - chunkH) - vOff;
                } else {
                    sv = entry->v + vOff;
                }
                params.v = sv;
                params.w = remCol;
                params.mx = (colPos - rowStartX) - bg->mx;
                params.flag = entry->flag;
                _mk_spr_packet(pk + 4, &params, &mtx, entry);
                pk += 0x28;
            }
        }

        vOff = 0;
        screenY += chunkH;
        curY += chunkH;
        chunkH = colEndY - curY;
        if ((u32)tileHi < chunkH) {
            chunkH = tileH;
        }
        params.my = (curY - baseY) - bg->my;
        params.y = (screenY - curY) + baseY;
        params.h = chunkH;
    } while (curY < colEndY);

    /* Link the chain into the OT: the LAST packet written (pk - 40) takes
     * the bucket's current head, the bucket takes the chain start, and
     * the cursor stays at pk. */
    pk -= 0x28;
    slot = pri + otp->offset;
    if (slot < 0) {
        slot = 0;
    }
    bucket = &otOrg[slot];
    pk += 0x28;
    *(u32 *)(pk - 0x28) = *bucket;
    pk[-0x25] = 9;
    *bucket = GsOUT_PACKET_P;
    ((u8 *)bucket)[3] = 0;
    GsOUT_PACKET_P = (u32)(unsigned long)pk;
}
