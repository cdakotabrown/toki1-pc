/*
 * _gcSPU -- coalesce and sort libspu's SPU-RAM allocation list (PC override
 * of src/main/matched/_gcSPU.c, 0x80097770).
 *
 * Statement for statement the matched function.  The matched TU declares
 * the list pointer and its last index as file-`static` (the byte match only
 * needs the relocations, which name the libspu globals); on the PC those
 * statics are separate zero objects, so the list was a NULL pointer and GCC
 * folded the first access into a trap -- reached as soon as a loaded VAB is
 * closed (SsVabClose -> SpuFree).  Here they are the libspu globals
 * themselves: `_spu_AllocLastNum` (0x800E3688) and `_SpuMemList`
 * (0x800EBD34), a 4-byte PS1 pointer that SpuInitMalloc stores as s32 and
 * that is read here as a zero-extended handle.
 */
#include "types.h"
#include <stdint.h>

typedef struct {
    u32 addr;
    u32 size;
} SpuMemEntry;

extern s32 _spu_AllocLastNum;
extern u32 _SpuMemList;          /* PS1 handle of the SpuMemEntry list */
#define LIST ((SpuMemEntry *)(uintptr_t)_SpuMemList)

void _gcSPU(void)
{
    s32 i;
    s32 j;
    SpuMemEntry *p;
    SpuMemEntry *q;
    u32 a;
    u32 s;

    i = 0;
    if (_spu_AllocLastNum < 0) {
        goto z1;
z1:     ;
    } else {
        do {
            if (LIST[i].addr & 0x80000000) {
                for (j = i + 1; 1; j++) {
                    if (LIST[j].addr != 0x2FFFFFFF) {
                        break;
                    }
                }
                if (LIST[j].addr & 0x80000000) {
                    if ((LIST[i].addr & 0x0FFFFFFF) + LIST[i].size
                        == (LIST[j].addr & 0x0FFFFFFF)) {
                        LIST[j].addr = 0x2FFFFFFF;
                        LIST[i].size = LIST[j].size
                                             + LIST[i].size;
                        continue;
                    }
                }
            }
            i++;
        } while (i <= _spu_AllocLastNum);
    }
    i = 0;
    if (_spu_AllocLastNum >= 0) {
        q = LIST;
        do {
            if (q->size == 0) {
                q->addr = 0x2FFFFFFF;
            }
            i++;
            q++;
        } while (i <= _spu_AllocLastNum);
    }
    i = 0;
    if (_spu_AllocLastNum < 0) {
        goto z3;
z3:     ;
    } else {
        do {
            if (LIST[i].addr & 0x40000000) {
                break;
            }
            j = i + 1;
            if (i < _spu_AllocLastNum) {
                p = (SpuMemEntry *)((uintptr_t)_SpuMemList + (j << 3));
                do {
                    if (p->addr & 0x40000000) {
                        break;
                    }
                    if ((p->addr & 0x0FFFFFFF)
                        < (LIST[i].addr & 0x0FFFFFFF)) {
                        a = LIST[i].addr;
                        LIST[i].addr = p->addr;
                        s = LIST[i].size;
                        LIST[i].size = p->size;
                        p->addr = a;
                        p->size = s;
                    }
                    j++;
                    p++;
                } while (j <= _spu_AllocLastNum);
            }
            i++;
        } while (i <= _spu_AllocLastNum);
    }
    for (i = 0; i <= _spu_AllocLastNum; i++) {
        if (LIST[i].addr & 0x40000000) {
            break;
        }
        if (LIST[i].addr == 0x2FFFFFFF) {
            LIST[i].addr = LIST[_spu_AllocLastNum].addr;
            LIST[i].size = LIST[_spu_AllocLastNum].size;
            _spu_AllocLastNum = i;
            break;
        }
    }
    i = _spu_AllocLastNum - 1;
    if (_spu_AllocLastNum <= 0) {
        goto z5;
z5:     ;
    } else {
        do {
            if (LIST[i].addr & 0x80000000) {
                LIST[i].addr = LIST[i].addr & 0x0FFFFFFF;
                LIST[i].addr = LIST[i].addr | 0x40000000;
                LIST[i].size = LIST[_spu_AllocLastNum].size
                                     + LIST[i].size;
                _spu_AllocLastNum = i;
            } else {
                break;
            }
            i--;
        } while (i >= 0);
    }
}
