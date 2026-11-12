/*
 * SetPitchBend -- SEQ pitch-bend event (PC override of
 * src/main/matched/SetPitchBend.c, 0x80094594).
 *
 * Statement for statement the matched function; the read pointer at +4 of
 * the 168-byte score record is a 4-byte PS1 handle (the matched
 * `*(u8 **)(rec + 4)` is an 8-byte load on the host that folds the loop
 * start handle at +8 into the pointer).  src/main/pc-port/snd_score.h.
 */
#include "types.h"
#include "snd_score.h"

extern void SpuVmPitchBend(s32, s16, u8, u8);
extern s32 ReadDeltaValue(s32, s32);

void SetPitchBend(s16 ch, s16 vo, u32 arg2)
{
    u8 *rec;
    u32 p;
    u8 *sub;
    u32 k;
    u8 c;

    vo = (vo << 16) >> 16;
    ch = (ch << 16) >> 16;
    rec = SND_SCORE(ch) + vo * 168;
    p = *(u32 *)(rec + 4);
    c = *SND_PTR(p);
    *(u32 *)(rec + 4) = p + 1;
    k = rec[18];
    sub = rec + k;
    SpuVmPitchBend((vo << 8) | ch, *(s16 *)(rec + 74), sub[44], c);
    *(s32 *)(rec + 136) = ReadDeltaValue(ch, vo);
}
