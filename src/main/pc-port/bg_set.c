/*
 * bg_set -- build one 36-byte background record and its 16-byte map header
 * (PC override of src/main/matched/bg_set.c).
 *
 * Statement for statement the matched function; the divergence is the
 * STRUCT WIDTHS (src/main/pc-port/mv_state.h doctrine).  The matched
 * typedefs carry `BG_MAP *cells`, `u16 *idx`, `BG_MAP *map` -- 4-byte
 * pointers on the console (the comments even record the PS1 offsets
 * the PS1 offset 20 right in the comment) but 8 bytes on LP64, which
 * shifted every field after
 * them.  bg_set wrote `map` at native +24 while the caller (vblank_function,
 * `(s32)&Bg + i*0x24`) and GsSortBg (PC override) use the 36-byte PS1
 * record with map at +20 -- so the sorter read the map handle from the
 * wrong bytes (measured: GsSortBg+0x41 over a garbage map pointer).
 *
 * Here the struct carries u32 PS1 pointer HANDLES at the console's exact
 * offsets and the handles are zero-extended at the three stores.
 * BG_CELL (8 bytes, pointer-free) and the Cell[][]/C_index[][] array
 * walks are layout-stable and stay verbatim.
 *
 * The matched header follows.
 */
#include "types.h"
#ifdef PC_PORT
#include "sony_bios_alias.h"   /* printf -> the BIOS A(0x3F) trampoline */
#endif

typedef struct {
    u8 b0;          /* 0 */
    u8 b1;          /* 1 */
    u16 h2;         /* 2 */
    u16 h4;         /* 4 */
    u16 h6;         /* 6 */
} BG_CELL;             /* 8 */

typedef struct {
    u8 r;           /* 0 */
    s8 s;           /* 1 */
    s16 cols;       /* 2 */
    s16 rows;       /* 4 */
    s16 pad;        /* 6 */
    u32 cells;      /* 8: PS1 handle -> BG_CELL[] */
    u32 idx;        /* 12: PS1 handle -> u16[] */
} BG_MAP;              /* 16 */

typedef struct {
    u32 flags;      /* 0 */
    u16 h4;         /* 4 */
    u16 h6;         /* 6 */
    s16 w;          /* 8 */
    s16 h;          /* 10 */
    s16 x;          /* 12 */
    s16 y;          /* 14 */
    u8 r, g, b, pad; /* 16 */
    u32 map;        /* 20: PS1 handle -> BG_MAP */
    s16 h24;        /* 24 */
    s16 h26;        /* 26 */
    s16 sx;         /* 28 */
    s16 sy;         /* 30 */
    u32 w32;        /* 32 */
} BG_WORK;               /* 36 */

extern BG_WORK Bg[];
extern BG_MAP Map[];
extern BG_CELL Cell[][64];
extern u16 C_index[][64];
extern u8 bg_pri[];
extern char bg_set_str0[];

extern void load_tpage_buf_lock();
extern s32 GetClut();
extern s32 printf(const char *format, ...);

void bg_set(src, slot, unused, cols, rows, arg5, arg6, arg7, arg8, base)
    s32 *src;
    s32 slot;
    s32 unused;
    s32 cols;
    s32 rows;
    s32 arg5;
    s32 arg6;
    s32 arg7;
    s32 arg8;
    s32 base;
{
    s32 pad[2];
    s32 row;
    s32 col;

    Bg[slot].flags = 0x1000000;
    Bg[slot].h4 = arg6;
    Bg[slot].h6 = arg7;
    Bg[slot].w = cols << 7;
    Bg[slot].h = rows * 120;
    Bg[slot].sx = 4096;
    Bg[slot].sy = 4096;
    Bg[slot].map = (u32)(unsigned long)&Map[slot];
    Bg[slot].x = 0;
    Bg[slot].y = 0;
    Bg[slot].r = 128;
    Bg[slot].g = 128;
    Bg[slot].b = 128;
    Bg[slot].h24 = 0;
    Bg[slot].h26 = 0;
    Bg[slot].w32 = 0;
    Map[slot].r = 128;
    Map[slot].s = 120;
    Map[slot].cols = cols;
    Map[slot].cells = (u32)(unsigned long)Cell[slot];
    Map[slot].idx = (u32)(unsigned long)C_index[slot];
    Map[slot].rows = rows;
    for (row = 0; row < rows; row++) {
        for (col = 0; col < cols; col++) {
            load_tpage_buf_lock(base + col + row * 32, src[row * cols + col], 1);
        }
    }
    for (row = 0; row < rows; row++) {
        for (col = 0; col < cols; col++) {
            C_index[slot][row * cols + col] = row * cols + col;
            Cell[slot][row * cols + col].b0 = 0;
            Cell[slot][row * cols + col].b1 = row * 120;
            Cell[slot][row * cols + col].h6 = base + col;
            Cell[slot][row * cols + col].h2 = GetClut(0, arg5);
            Cell[slot][row * cols + col].h4 = 0;
            if (row * cols + col >= 65) {
                printf(bg_set_str0);
            }
        }
    }
    bg_pri[slot] = arg8;
}
