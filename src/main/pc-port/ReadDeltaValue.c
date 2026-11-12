/*
 * ReadDeltaValue -- read a SEQ variable-length delta time (PC override of
 * src/main/matched/ReadDeltaValue.c, 0x80094A1C).
 *
 * Statement for statement the matched function; the read pointer at +4 of
 * the 168-byte score record is a 4-byte PS1 handle and `time` stays at its
 * PS1 offset +0x80 (src/main/pc-port/snd_score.h).
 */
#include "types.h"
#include "snd_score.h"

typedef struct {
    s32 f0;
    u32 ptr;            /* +4 (PS1 handle) */
    u8 pad8[120];
    s32 time;           /* +0x80 */
    u8 pad84[36];
} Chan;

typedef char chan_ps1_size[sizeof(Chan) == 168 ? 1 : -1];

s32 ReadDeltaValue(s16 port, s16 ch)
{
    Chan *c;
    s32 v;
    s32 b;
    s32 d;

    c = &((Chan *)SND_SCORE(port))[ch];
    v = *SND_PTR(c->ptr++);
    if (v == 0) {
        return v;
    }
    if (v & 0x80) {
        v &= 0x7F;
        do {
            b = *SND_PTR(c->ptr);
            v = (v << 7) + (b & 0x7F);
            c->ptr++;
        } while (b & 0x80);
    }
    d = v * 10;
    c->time += d;
    return d;
}
