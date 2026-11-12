/*
 * mod_set -- PC override of src/main/matched/mod_set.c (0x8004E228).
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
 * @DivergesFrom: src/main/matched/mod_set.c -- pointer members as u32.
 */
#include "types.h"

typedef struct {
    u32 pal;         /* 0: PS1 address of an s32 table */
    u32 map;         /* 4: PS1 address */
    u8 mode;         /* 8 */
    u8 pad[3];
    u8 cols;         /* 12 */
    u8 rows;         /* 13 */
    u8 tileW;        /* 14 */
    u8 tileH;        /* 15 */
    s32 f10;         /* 16 */
} TileInfo;

typedef char tile_info_ps1_size[sizeof(TileInfo) == 20 ? 1 : -1];

typedef struct {
    u32 flags;       /* 0 */
    s16 x;           /* 4 */
    s16 y;           /* 6 */
    s16 w;           /* 8 */
    s16 h;           /* 10 */
    u16 tpage;       /* 12 */
    u8 u;            /* 14 */
    u8 v;            /* 15 */
    s16 x2;          /* 16 */
    s16 y2;          /* 18 */
    u8 r;            /* 20 */
    u8 g;            /* 21 */
    u8 b;            /* 22 */
    u8 pad2;         /* 23 */
    s16 tw2;         /* 24 */
    s16 th2;         /* 26 */
    s16 sx;          /* 28 */
    s16 sy;          /* 30 */
    u32 flags2;      /* 32 */
} Sprite;

typedef struct {
    u8 b[20];
} Clut;

extern TileInfo mod_w[];
extern s32 activeBuff;
extern Clut ot_work[];

extern s32 get_brightness();
extern s32 search_tpage_multi();
extern void GsSortFastSprite(Sprite *, Clut *, u16);

void mod_set(n, base, x0, y0, sx, sy, mode)
s32 n;
s32 base;
s32 x0;
s32 y0;
s32 sx;
s32 sy;
u16 mode;
{
    TileInfo *info;
    s32 col;
    u32 *src;
    u32 b2;
    s32 row;
    Sprite spr;
    u32 b1;
    s32 x;
    s32 y;
    s32 bright;
    u8 tileW;
    u16 m;
    s32 padB[10];
    s32 cs;
    u32 b3;
    s32 padC[5];

    bright = get_brightness();
    switch (mod_w[n].mode) {
    case 0:
        spr.flags = 0x1000000;
        cs = 1;
        break;
    case 1:
        cs = 2;
        spr.flags = 0;
        break;
    case 2:
        cs = 2;
        spr.flags = 0;
        break;
    case 3:
        spr.flags = 0x1000000; cs = 1;
        break;
    case 4:
        spr.flags = 0x1000000; cs = 1;
        break;
    case 5:
        spr.flags = 0x1000000; cs = 1;
        break;
    default:
        spr.flags = 0x1000000;
        cs = 1;
        break;
    }
    tileW = mod_w[n].tileW;
    spr.w = tileW;
    spr.h = mod_w[n].tileH;
    spr.tw2 = 0;
    spr.th2 = 0;
    spr.r = bright;
    spr.g = bright;
    spr.b = bright;
    spr.flags2 = 0;
    spr.sx = 4096;
    spr.sy = 4096;
    src = (u32 *)(unsigned long)mod_w[n].map + mod_w[n].cols * (mod_w[n].rows * base);
    for (row = 0; row < mod_w[n].rows; row++) {
        for (col = 0; col < mod_w[n].cols; col++) {
            spr.x = col * tileW + x0;
            spr.y = row * tileW + y0;
            m = mode;
            b2 = (*src & 0xFF0000) >> 16;
            b3 = *src & 0xFF000000;
            b1 = (*src & 0xFF00) >> 8;
            spr.tpage = search_tpage_multi(((s32 *)(unsigned long)mod_w[n].pal)[b3 >> 24], cs);
            switch (tileW) {
            default:
                spr.u = (b2 & 0xF) << 3;
                spr.v = ((b2 & 0xF0) >> 4) << 3;
                break;
            case 8:
                spr.u = (b2 & 0xF) << 3;
                spr.v = ((b2 & 0xF0) >> 4) << 3;
                break;
            case 16:
                spr.u = (b2 & 0x7) << 4;
                spr.v = ((b2 & 0xF8) >> 4) << 4;
                break;
            }
            spr.x2 = sx + (b1 << 4);
            spr.y2 = sy;
            GsSortFastSprite(&spr, &ot_work[activeBuff], m);
            src++;
        }
    }
}
