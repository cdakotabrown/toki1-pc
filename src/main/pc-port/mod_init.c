/*
 * mod_init -- PC override of src/main/matched/mod_init.c (0x8004E884).
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
 * @DivergesFrom: src/main/matched/mod_init.c -- pointer members as u32.
 */
#include "types.h"

typedef struct {
    s32  f0;
    u32  f4;     /* PS1 address */
    u8   f8;
    u8   f9;
    u16  fA;
    u8   fC;
    u8   fD;
    u8   fE;
    u8   fF;
    u16  f10;
    u16  f12;
} Rec20;

typedef char rec20_ps1_size[sizeof(Rec20) == 20 ? 1 : -1];

extern Rec20 mod_w[];

void mod_init(s32 idx, u32 *src, s32 arg2) {
    Rec20 *p = &mod_w[idx];

    p->f0 = arg2;
    p->f4 = (u32)(unsigned long)(src + 4);
    p->f8 = (src[0] & 0xFF000000) >> 24;
    p->fA = (src[0] & 0xFFFF) >> 24;
    p->fC = (src[1] & 0xFF000000) >> 24;
    p->fD = (src[1] & 0x00FF0000) >> 16;
    p->fE = (src[1] & 0xFF00) >> 8;
    p->fF = src[1] & 0xFF;   /* the mask burns a temp (as1 drops the andi before sb) */
    p->f10 = ((u8 *)src)[8];
    p->f12 = ((u8 *)src)[12];
}
