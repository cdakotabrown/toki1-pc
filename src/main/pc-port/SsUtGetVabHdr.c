/*
 * SsUtGetVabHdr -- copy a VAB header's fixed fields (PC override of
 * src/main/matched/SsUtGetVabHdr.c, 0x8008FD70).
 *
 * Statement for statement the matched function; `_svm_vab_vh` is libsnd's
 * 16-entry table of 4-byte PS1 header pointers (written as s32 by
 * SsVabOpenHeadWithMode).  The matched `Voice *_svm_vab_vh[]` indexes it
 * with an 8-byte stride on the host, so seq_start's master-volume read for
 * VAB 2 took entries 4/5.  Read as u32 here.  _svm_vh is a host pointer
 * global on the PC (pc/Makefile SND_PTR_GLOBALS).
 */
#include "types.h"
#include <stdint.h>

typedef struct {
    s32 w0;
    s32 w4;
    s32 w8;
    u8 pad0C[6];
    u16 h12;
    u16 h14;
    u16 h16;
    u8 b18;
    u8 b19;
    u8 b1A;
    u8 b1B;
} Voice;

extern u8 _svm_vab_used[];
extern u32 _svm_vab_vh[];
extern Voice *_svm_vh;

s32 SsUtGetVabHdr(s16 voice, Voice *dst)
{
    if (_svm_vab_used[voice] == 1) {
    _svm_vh = (Voice *)(uintptr_t)_svm_vab_vh[voice];
    dst->w0 = _svm_vh->w0;
    dst->w8 = _svm_vh->w8;
    dst->w4 = _svm_vh->w4;
    dst->h12 = _svm_vh->h12;
    dst->h14 = _svm_vh->h14;
    dst->h16 = _svm_vh->h16;
    dst->b18 = _svm_vh->b18;
    dst->b19 = _svm_vh->b19;
    dst->b1A = _svm_vh->b1A;
    dst->b1B = _svm_vh->b1B;
    return 0;
    }
    return -1;
}
