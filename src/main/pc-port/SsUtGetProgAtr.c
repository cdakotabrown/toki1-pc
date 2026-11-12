/*
 * SsUtGetProgAtr -- PC override of src/main/matched/SsUtGetProgAtr.c
 * (0x80094AD0, libsnd).
 *
 * Statement for statement the matched function except the SpuVmVSetUp
 * call.  The matched C calls `SpuVmVSetUp()` bare; the listing narrows its
 * own arguments in place (sll/sra a0 and a1 by 16) and never writes a0/a1
 * again before the jal 0x80091080, so the console runs
 * SpuVmVSetUp(id, idx): select VAB `id` and check program `idx`, which sets
 * _svm_pg to that VAB's program table before the attributes are read from
 * it.  On x86-64 the bare call passed whatever %edi/%esi held:
 * SpuVmVSetUp returned -1 or selected another VAB, and the attributes came
 * from a stale table.  pc/tools/audit_ps1_call_args.py SHORT-READ.
 *
 * @DivergesFrom: src/main/matched/SsUtGetProgAtr.c -- the arguments the
 * console passes in $a0/$a1 are written out.
 */
#include "types.h"

typedef struct {
    u8 b0;
    u8 b1;
    u8 b2;
    u8 b3;
    u8 b4;
    u8 pad5;
    s16 h6;
    u8 pad8[8];
} Myu;

extern u8 _svm_vab_used[];
extern Myu *_svm_pg;

extern s32 SpuVmVSetUp(s16 slot, s16 n);

s32 SsUtGetProgAtr(s16 id, s16 idx, Myu *out) {
    if (_svm_vab_used[id] == 1) {
        SpuVmVSetUp(id, idx);
        out->b0 = _svm_pg[idx].b0;
        out->b1 = _svm_pg[(u32)idx].b1;
        out->b2 = ((Myu *)((u8 *)_svm_pg + (idx << 4)))->b2;
        out->b3 = ((Myu *)((u8 *)_svm_pg + ((u32)idx << 4)))->b3;
        out->b4 = ((Myu *)((u8 *)_svm_pg + ((idx * 4) << 2)))->b4;
        out->h6 = ((Myu *)((u8 *)_svm_pg + ((idx << 2) * 4)))->h6;
        return 0;
    }
    return -1;
}
