/*
 * SpuFree -- mark an SPU-RAM allocation free and garbage-collect the list
 * (PC override of src/main/matched/SpuFree.c, 0x800984BC).
 *
 * Statement for statement the matched function; `_SpuMemList` (0x800EBD34)
 * is a 4-byte PS1 pointer global that SpuInitMalloc stores as s32.  The
 * matched `extern u32 *_SpuMemList` is an 8-byte load on the host that
 * folds the neighbouring word into the pointer, so it is read here as a
 * zero-extended handle.  Reached when a loaded VAB is closed (SsVabClose).
 */
#include "types.h"
#include <stdint.h>

extern s32 _spu_AllocBlockNum;
extern u32 _SpuMemList;          /* PS1 handle of the allocation list */
extern void _gcSPU(void);

void SpuFree(u32 value) {
    s32 count;
    s32 i;
    u32 *ptr;
    u32 entry;

    count = _spu_AllocBlockNum;
    i = 0;
    if (count > 0) {
        ptr = (u32 *)(uintptr_t)_SpuMemList;
        do {
            entry = *ptr;
            if (entry & 0x40000000) {
                break;
            }
            if (value == entry) {
                *ptr = entry | (u32)0x80000000;
                break;
            }
            i++;
            ptr += 2;
        } while (i != count);
    }
    _gcSPU();
}
