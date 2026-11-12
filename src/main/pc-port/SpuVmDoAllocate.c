/*
 * SpuVmDoAllocate -- stage a newly allocated voice's start address, ADSR
 * and pan in the register buffer (PC override of
 * src/main/matched/SpuVmDoAllocate.c, 0x8008CE94).
 *
 * Statement for statement the matched function.  The matched C reads the
 * current program table (_svm_pg, 0x8013161C) and tone table (_svm_tn,
 * 0x80131620) as `vag` / `tbl` members of one record laid over
 * 0x801315F4..0x80131623.  On the PC those two PS1 pointer globals are
 * host pointer globals (pc/Makefile SND_PTR_GLOBALS) -- on a 64-bit host
 * the record would put `tbl` at +0x30 and both loads would fold
 * neighbouring words -- so the two pointers are read by name here; every
 * other member keeps its PS1 offset.
 */
#include "types.h"

typedef struct {
    u8  pad[6];
    s16 f6;
    u8  pad2[16];
} V24;
typedef struct {
    u8 pad[12];
    u16 f12;
    u16 f14;
} VagEnt;
typedef struct {
    u8  p0[3];
    s8  bank;       /* 0x801315F7 */
    u8  p1[4];
    s8  prog;       /* 0x801315FC */
    u8  p2[11];
    s16 tone;       /* 0x80131608 */
    s16 voice;      /* 0x8013160A */
    s16 voff;       /* 0x8013160C */
    s16 slot;       /* 0x8013160E */
    u16 pan;        /* 0x80131610 */
} SndState;

extern SndState D_801315F4;
extern VagEnt *_svm_pg;          /* 0x8013161C on the console */
extern u8 (*_svm_tn)[32];        /* 0x80131620 on the console */
extern V24 _svm_voice[];
extern u32 _svm_envx_hist[];
extern s16 _svm_sreg_buf[];
extern u8  _svm_sreg_dirty[];

void SpuVmDoAllocate(void)
{
    s16 voice;
    s32 vsh;
    u32 mask;
    s32 i;
    s16 *vp;
    u8 *fp;
    u8 *ap;
    u16 pan;

    voice = D_801315F4.voice;
    D_801315F4.voff = voice * 8;
    vsh = (s8)D_801315F4.bank << 4;
    D_801315F4.slot = (s32)vsh + (s32)(s8)D_801315F4.prog;
    mask = ~(1 << voice);
    _svm_voice[voice].f6 = 32767;
    i = 0;
    while (i < 16) {
        _svm_envx_hist[i] &= mask;
        i++;
    }
    if ((D_801315F4.tone & 1) > 0) {
        vp = &_svm_sreg_buf[D_801315F4.voff];
        vp[3] = _svm_pg[(D_801315F4.tone - 1) / 2].f12;
        fp = &_svm_sreg_dirty[D_801315F4.voice];
        *fp |= 8;
    } else {
        vp = &_svm_sreg_buf[D_801315F4.voff];
        vp[3] = _svm_pg[(D_801315F4.tone - 1) / 2].f14;
        fp = &_svm_sreg_dirty[D_801315F4.voice];
        *fp |= 8;
    }
    /* the console's tn + (vsh + prog) * 32, as one offset: indexing the
     * u8[32] rows by vsh and then prog << 5 overran the row (UBSan) */
    ap = (u8 *)_svm_tn + (((s32)vsh + (s8)D_801315F4.prog) << 5);
    vp[4] = *(u16 *)(ap + 16);
    pan = *(u16 *)(ap + 18) + D_801315F4.pan;
    *fp |= 0x30;
    vp[5] = pan;
}
