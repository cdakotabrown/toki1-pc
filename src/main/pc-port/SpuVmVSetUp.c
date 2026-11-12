/*
 * SpuVmVSetUp -- select the current VAB/program (PC override of
 * src/main/matched/SpuVmVSetUp.c, 0x80091080).
 *
 * Statement for statement the matched function; the divergence is the
 * width of libsnd's per-VAB pointer tables.  `_svm_vab_vh`, `_svm_vab_pg`
 * (_svm_vab_pg) and `_svm_vab_tn` (_svm_vab_tn) are 16-entry tables of
 * 4-byte PS1 pointers, written by SsVabOpenHeadWithMode as s32.  The
 * matched `u8 *_svm_vab_pg[]` indexes them with an 8-byte stride on the
 * host (VAB 2 would read entries 4/5), so all three are read as u32 here.
 * The current-VAB pointers _svm_vh/_svm_pg/_svm_tn are host pointer
 * globals on the PC (pc/Makefile SND_PTR_GLOBALS), holding the same
 * zero-extended PS1 addresses.
 */
#include "types.h"
#include <stdint.h>

extern u8   _svm_vab_used[];
extern s16  kMaxPrograms;
extern u32  _svm_vab_vh[];
extern u32  _svm_vab_pg[];
extern u32  _svm_vab_tn[];
extern u8  *_svm_vh;
extern u8  *_svm_pg;
extern u8  *_svm_tn;
extern u8   libsnd_cur_vab_id;
extern u8   libsnd_cur_program;
extern u8   libsnd_cur_fake_program;

s32 SpuVmVSetUp(s16 slot, s16 n) {
    if (slot >= 16 || slot < 0 || _svm_vab_used[slot] != 1 || n >= kMaxPrograms) {
        return -1;
    }
    _svm_vh = (u8 *)(uintptr_t)_svm_vab_vh[slot];
    _svm_pg = (u8 *)(uintptr_t)_svm_vab_pg[(u32)slot];
    _svm_tn = (u8 *)(uintptr_t)_svm_vab_tn[slot];
    libsnd_cur_vab_id = slot;
    libsnd_cur_program = n;
    libsnd_cur_fake_program = *(s32 *)((_svm_pg + 8) + n * 16);
    return 0;
}
