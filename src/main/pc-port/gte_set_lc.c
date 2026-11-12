/*
 * gte_set_lc -- PC calling-convention adapter for matched/gte_set_lc.c.
 *
 * Retail copies the eight words of the light-colour matrix into _LC and
 * then calls SetColorMatrix with NO C-level argument: $a0 still holds the
 * source pointer, so the GTE's LCM is loaded from it.  On the host the
 * matched call relied on the compiler leaving %rdi untouched (it happened
 * to).  This adapter keeps the same copy (same order, same words) and
 * passes that pointer explicitly.  The resulting LCM was measured against
 * a retail TACO save state: LCM == _LC.
 */
#include "types.h"
#include "hal.h"

extern s32 _LC[];

void gte_set_lc(s32 *src)
{
    volatile s32 *dst = _LC;
    volatile s32 *vsrc = src;
    s32 i;

    for (i = 0; i < 8; i++) dst[i] = vsrc[i];
    SetColorMatrix((MATRIX *)src);
}
