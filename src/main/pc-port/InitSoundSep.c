/*
 * InitSoundSep -- reset one SEP slot's score record and parse its stream
 * header (PC override of src/main/matched/InitSoundSep.c, 0x80091570).
 *
 * Statement for statement the matched function.  The divergences are
 * addressing only (src/main/pc-port/snd_score.h, the InitSoundSeq.c
 * doctrine):
 *  - the record's three stream pointers (+4 read, +8 loop start, +12 loop
 *    point) are 4-byte PS1 handles, as on the console, so every later
 *    field keeps its PS1 offset (division +0x48, w88 +0x88, ...) and the
 *    record keeps its 168-byte stride (`&_ss_score[ch][vo]`);
 *  - `_ss_score[ch]` is read from the u32 table (4 bytes per sequence; the
 *    matched `SeqAttr *_ss_score[]` strides 8 on LP64).
 * The stream address arrives as SsSepOpen's `u8 *` (an address in the
 * mapped PS1 RAM window) and is kept as its handle.
 */
#include "types.h"
#include "snd_score.h"

typedef struct {
    u8 pad0[4];
    u32 ptr;            /* 0x04 (PS1 handle) */
    u32 ptr2;           /* 0x08 (PS1 handle) */
    u32 ptr3;           /* 0x0C (PS1 handle) */
    u8 b10;             /* 0x10 */
    u8 b11;
    u8 b12;
    u8 b13;
    u8 b14;
    u8 b15;
    u8 b16;
    u8 pan[16];         /* 0x17 */
    u8 b27;             /* 0x27 */
    u8 b28;
    u8 b29;
    u8 b2A;
    u8 b2B;
    u8 prog[16];        /* 0x2C */
    u8 pad3C[10];       /* 0x3C */
    s16 h46;            /* 0x46 */
    s16 division;       /* 0x48 */
    s16 h4A;            /* 0x4A */
    s16 vol[16];        /* 0x4C */
    s16 h6C;            /* 0x6C */
    s16 h6E;            /* 0x6E */
    s16 h70;            /* 0x70 */
    u8 pad72[4];        /* 0x72 */
    u16 h76;            /* 0x76 */
    u16 h78;            /* 0x78 */
    u8 pad7A[2];
    s32 w7C;            /* 0x7C */
    s32 w80;            /* 0x80 */
    s32 tempo;          /* 0x84 */
    s32 w88;            /* 0x88 */
    s32 w8C;            /* 0x8C */
    u8 pad90[24];       /* 0x90 */
} SeqAttr;

typedef char sep_attr_ps1_offsets[(sizeof(SeqAttr) == 168 &&
                                   (int)(uintptr_t)&((SeqAttr *)0)->division == 0x48 &&
                                   (int)(uintptr_t)&((SeqAttr *)0)->w8C == 0x8C)
                                  ? 1 : -1];

extern u32 VBLANK_MINUS;

extern s32 ReadDeltaValue();

/* Read the byte at the record's read handle and advance it. */
#define NEXT(vs) (*SND_PTR((vs)->ptr++))

s32 InitSoundSep(ch, vo, arg2, data)
    s16 ch;
    s16 vo;
    s16 arg2;
    u8 *data;
{
    SeqAttr *vs;
    s32 i;
    s32 n;
    u32 a;
    u32 b;
    u32 c;
    u32 d;
    u32 e;
    u8 c0;
    u8 c1;
    u8 c2;
    u8 c3;
    s32 t;
    u32 prod;

    vs = &((SeqAttr *)SND_SCORE(ch))[vo];
    ((volatile SeqAttr *)vs)->division = 0;
    ((volatile SeqAttr *)vs)->b13 = 0;
    ((volatile SeqAttr *)vs)->b14 = 0;
    ((volatile SeqAttr *)vs)->b29 = 0;
    ((volatile SeqAttr *)vs)->b15 = 0;
    ((volatile SeqAttr *)vs)->b16 = 0;
    ((volatile SeqAttr *)vs)->b2A = 0;
    ((volatile SeqAttr *)vs)->b12 = 0;
    ((volatile SeqAttr *)vs)->w7C = 0;
    ((volatile SeqAttr *)vs)->w80 = 0;
    ((volatile SeqAttr *)vs)->tempo = 0;
    ((volatile SeqAttr *)vs)->h70 = 0;
    ((volatile SeqAttr *)vs)->h46 = 0;
    ((volatile SeqAttr *)vs)->b2B = 0;
    ((volatile SeqAttr *)vs)->w88 = 0;
    ((volatile SeqAttr *)vs)->b27 = 0;
    ((volatile SeqAttr *)vs)->b28 = 0;
    ((volatile SeqAttr *)vs)->b10 = 0;
    ((volatile SeqAttr *)vs)->b11 = 0;
    ((volatile SeqAttr *)vs)->h4A = arg2;
    for (i = 0; i < 16; i++) {
        vs->prog[i] = i;
        vs->pan[i] = 0x40;
        vs->vol[i] = 0x7F;
    }
    vs->ptr = SND_HANDLE(data);
    vs->h6C = 1;
    vs->h76 = 0x7F;
    vs->h78 = 0x7F;
    if (vo == 0) {
        vs->ptr += 8;
        n = 8;
    } else {
        vs->ptr += 2;
        n = 2;
    }
    a = NEXT(vs);
    b = NEXT(vs);
    vs->division = ((s16)a << 8) | b;
    n += 2;
    c = NEXT(vs);
    d = NEXT(vs);
    e = NEXT(vs);
    n += 3;
    t = (c << 16) | (d << 8) | e;
    vs->tempo = t;
    if (60000000 % t > t / 2) {
        vs->tempo = 60000000 / vs->tempo + 1;
    } else {
        vs->tempo = 60000000 / vs->tempo;
    }
    vs->ptr += 2;
    vs->w8C = vs->tempo;
    n += 2;
    c0 = NEXT(vs);
    c1 = NEXT(vs);
    c2 = NEXT(vs);
    c3 = NEXT(vs);
    n += 4;
    vs->w7C = ReadDeltaValue(ch, vo);
    vs->w88 = vs->w7C;
    vs->ptr2 = vs->ptr;
    vs->ptr3 = vs->ptr;
    prod = vs->division * vs->tempo;
    if (prod * 10 < VBLANK_MINUS * 60) {
        vs->h6C = (VBLANK_MINUS * 600) / prod;
        vs->h6E = vs->h6C;
    } else {
        vs->h6C = -1;
        vs->h6E = (prod * 10) / (VBLANK_MINUS * 60);
        if ((prod * 10) % (VBLANK_MINUS * 60) > (VBLANK_MINUS * 60) >> 1) {
            vs->h6E++;
        }
    }
    vs->h70 = vs->h6E;
    t = (c0 << 24) + (c1 << 16) + (c2 << 8) + c3;
    return n + t;
}
