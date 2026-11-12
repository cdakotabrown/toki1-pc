/*
 * srn_init -- PC override of src/main/matched/srn_init.c (0x8004E788).
 *
 * Statement for statement the matched function; the divergence is the
 * RECORD WIDTH (the src/main/pc-port/basyo_init.c doctrine).  srn_w (0x80129C30) holds 40-byte records.
 * On the console the record's pointer members are 4-byte cells; the
 * matched C declares them as pointers, 8 bytes (and 8-aligned) on LP64,
 * so every later member moves, the record stride grows and the four mod_w
 * readers/writers (mod_init, mod_set, mod_set_gpu's all-s32 view,
 * mod_trans_vram) and srn_init / srn_vram_set no longer agree on one
 * layout.  Here the pointer members are u32 PS1 addresses, zero-extended
 * where they are used (pc/tools/audit_ps1_pointer_casts.py RECORD-PTR).
 *
 * @DivergesFrom: src/main/matched/srn_init.c -- pointer members as u32.
 */
#include "types.h"

typedef struct {
    u16  h0;
    u16  h2;
    u16  h4;
    u16  h6;
    u32  w8;     /* PS1 address */
    u8   b12;
    u8   b13;
    u8   b14;
    u8   b15;
    u8   b16;
    u8   b17;
    u8   b18;
    u8   b19;
    u8   b20;
    u8   b21;
    u16  h22;
    u16  h24;
    u16  h26;
    s32  w28;
    s32  w32;
    u32  w36;    /* PS1 address */
} Rec40;

typedef char rec40_ps1_size[sizeof(Rec40) == 40 ? 1 : -1];

extern Rec40 srn_w[];
extern u8 mod_w[];

void srn_init(s32 idx, u32 *src, s32 link) {
    Rec40 *p = &srn_w[idx];

    p->b13 = (src[0] & 0xFF00) >> 8;
    p->h2 = 0;
    p->h6 = 0;
    p->b12 = 0;
    p->w8 = (u32)(unsigned long)(src + 5);
    p->b14 = (src[1] & 0xFF000000) >> 24;
    p->b15 = (src[1] & 0x00FF0000) >> 16;
    p->b16 = (src[1] & 0xFF00) >> 8;
    p->b17 = src[1] & 0xFF;
    p->b18 = (src[2] & 0xFF000000) >> 24;
    p->b19 = (src[2] & 0x00FF0000) >> 16;
    p->b20 = (src[2] & 0xFF00) >> 8;
    p->b21 = src[2] & 0xFF;
    p->h22 = 0;
    p->h24 = 480;
    p->w28 = src[4];
    p->w32 = src[3];
    p->w36 = (u32)(unsigned long)&mod_w[link * 20];
}
