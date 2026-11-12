/*
 * Snd_nextseq -- start the chained next sequence (PC override of
 * src/main/matched/Snd_nextseq.c, 0x80095040).
 *
 * Statement for statement the matched function; the score pointer is the
 * u32 `_ss_score[a0]`.  The matched `*(u8 **)((s32)_ss_score + a0 * 4)`
 * names the same PS1 address, but as a host pointer the (s32) cast
 * sign-extends it out of the mapped window and the load is 8 bytes wide.
 * src/main/pc-port/snd_score.h.
 */
#include "types.h"
#include "snd_score.h"

extern void SpuVmSetSeqVol(s16, s32, s32, s32);

void Snd_nextseq(s16 a0, s16 a1)
{
    u8 *entry;

    entry = SND_SCORE(a0) + a1 * 168;
    *(s16 *)(entry + 68) = 1;
    *(s16 *)(entry + 70) = 0;
    SpuVmSetSeqVol(a0, 127, 127, a1);
    *(s32 *)(SND_SCORE(a0) + a1 * 168 + 144) &= ~0x100;
    *(s32 *)(SND_SCORE(a0) + a1 * 168 + 144) &= ~0x8;
    *(s32 *)(SND_SCORE(a0) + a1 * 168 + 144) &= ~0x2;
    *(s32 *)(SND_SCORE(a0) + a1 * 168 + 144) &= ~0x4;
    *(s32 *)(SND_SCORE(a0) + a1 * 168 + 144) &= ~0x200;
    entry[43] = 1;
    *(s32 *)(SND_SCORE(a0) + a1 * 168 + 144) |= 0x1;
}
