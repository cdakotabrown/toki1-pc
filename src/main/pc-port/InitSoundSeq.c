/*
 * InitSoundSeq -- reset a sequence's score record and parse its SEQ header
 * (PC override of src/main/matched/InitSoundSeq.c, 0x80091160).
 *
 * Statement for statement the matched function.  The divergences are
 * addressing only (src/main/pc-port/snd_score.h):
 *  - the record's three stream pointers (+4 read, +8 loop start, +12 loop
 *    point) are 4-byte PS1 handles, as on the console, so the other score
 *    fields keep their PS1 offsets (division +0x48, w88 +0x88, ...) which
 *    the raw-offset readers (SeqPlay, SsSeqClose, ...) use;
 *  - `_ss_score[arg0]` is read from the u32 table;
 *  - the SEQ address arrives as the 32-bit value SsSeqOpen passes
 *    (unprototyped call, `s32 arg0` in the caller), not as a host pointer.
 *
 * One PC-only exit (the only behavioural divergence): a zero tempo returns
 * before the tempo divisions.  It is reachable only through the kernel RAM
 * the PC does not model.  seq_start opens whatever gSDSeqAdrs holds; when
 * the scene's SEQ table is not loaded (seen under SCENE_NAV=130) that is
 * address 0, and retail does the same (the regtest host, -tracepc:
 * SsSeqOpen(0, 1) -> InitSoundSeq(0, 1, 0)).  On the console address 0 is
 * BIOS kernel RAM (FF 41 FF FF 80 ..), so the "tempo" is 0xFFFF80 and the
 * game continues; on the PC that RAM is zero (no BIOS image), where the
 * matched code would divide by zero (the console code checks the divisor
 * with break 7; an x86 divide traps).  The record keeps the reset fields
 * set above, and the sequencer reads RAM from there as the console would.
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
} SeqAttr;

typedef char seq_attr_ps1_offsets[(sizeof(SeqAttr) == 0x90 &&
                                   (int)(uintptr_t)&((SeqAttr *)0)->division == 0x48)
                                  ? 1 : -1];

extern u32 VBLANK_MINUS;
extern u8 libsnd_not_seq_data_msg[];
extern u8 libsnd_old_seq_format_msg[];
extern s32 ReadDeltaValue();
extern void printf();

/* Read the byte at the record's read handle and advance it. */
#define NEXT(vs) (*SND_PTR((vs)->ptr++))

void InitSoundSeq(arg0, arg1, arg2)
    s16 arg0;
    s16 arg1;
    u32 arg2;
{
    SeqAttr *vs;
    s32 i;
    u32 a;
    u32 ver;
    u32 b;
    u32 c;
    u32 d;
    u32 e;
    s32 r;
    s32 t;
    u32 prod;

    vs = (SeqAttr *)SND_SCORE(arg0);
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
    vs->h4A = arg1;
    ((volatile SeqAttr *)vs)->b10 = 0;
    ((volatile SeqAttr *)vs)->b11 = 0;
    for (i = 0; i < 16; i++) {
        vs->prog[i] = i;
        vs->pan[i] = 0x40;
        vs->vol[i] = 0x7F;
    }
    vs->h6C = 1;
    vs->h76 = 0x7F;
    vs->h78 = 0x7F;
    vs->ptr = arg2;
    if (*SND_PTR(arg2) == 'S' || *SND_PTR(arg2) == 'p') {
        vs->ptr += 7;
        ver = NEXT(vs);
        if (ver != 1) {
            printf(libsnd_not_seq_data_msg);
            return;
        }
    } else {
        printf(libsnd_old_seq_format_msg);
    }
    a = NEXT(vs);
    b = NEXT(vs);
    vs->division = (a << 8) | b;
    c = NEXT(vs);
    d = NEXT(vs);
    e = NEXT(vs);
    t = (c << 16) | (d << 8) | e;
    vs->tempo = t;
    if (t == 0) {
        /* PC-only exit, see the header: a zero tempo cannot be read on
         * the console here, where the divide below would raise break 7. */
        return;
    }
    if (60000000 % t > t / 2) {
        vs->tempo = 60000000 / vs->tempo + 1;
    } else {
        vs->tempo = 60000000 / vs->tempo;
    }
    vs->w8C = vs->tempo;
    vs->ptr += 2;
    r = ReadDeltaValue(arg0, 0);
    vs->w7C = r;
    vs->w88 = r;
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
}
