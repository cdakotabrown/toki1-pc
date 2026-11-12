/*
 * GetMetaEvent -- SEQ meta events: tempo (0x51) and end of track (0x2F)
 * (PC override of src/main/matched/GetMetaEvent.c, 0x80094648).
 *
 * Statement for statement the matched function.  Addressing divergences
 * only (src/main/pc-port/snd_score.h): the record's read / loop-start /
 * loop-point pointers (+4/+8/+12) are 4-byte PS1 handles so every later
 * field keeps its PS1 offset, and the score pointer is the u32
 * `_ss_score[sep_no]` (the matched `(s32)_ss_score + sep_no * 4` is that
 * same PS1 address; as a 64-bit host pointer it would sign-extend).
 */
#include "types.h"
#include "snd_score.h"

typedef struct {
    /* 0x00 */ s8 vab_id;
    /* 0x01 */ u8 pad01[3];
    /* 0x04 */ u32 data;           /* PS1 handle */
    /* 0x08 */ u32 loop_start;     /* PS1 handle */
    /* 0x0C */ u32 loop_ptr;       /* PS1 handle */
    /* 0x10 */ u8 pad10[23];
    /* 0x27 */ u8 f39;
    /* 0x28 */ u8 pad28[3];
    /* 0x2B */ u8 f43;
    /* 0x2C */ u8 pad2C[24];
    /* 0x44 */ s16 l_count;
    /* 0x46 */ s16 l_current;
    /* 0x48 */ s16 resolution;
    /* 0x4A */ u8 pad4A[34];
    /* 0x6C */ s16 f108;
    /* 0x6E */ s16 f110;
    /* 0x70 */ u8 pad70[16];
    /* 0x80 */ s32 f128;
    /* 0x84 */ s32 f132;
    /* 0x88 */ s32 f136;
    /* 0x8C */ s32 tempo;
    /* 0x90 */ s32 flags;
    /* 0x94 */ u8 pad94[20];
} SeqStruct;   /* 168 bytes */

typedef char seqstruct_ps1_size[sizeof(SeqStruct) == 168 ? 1 : -1];

extern s32 VBLANK_MINUS;
extern s32 ReadDeltaValue();
extern void Snd_nextseq();
extern void SpuVmSeqKeyOff();

#define SCORE(sep) ((SeqStruct *)SND_SCORE(sep))

void GetMetaEvent(s16 sep_no, s16 seq_no, u8 cmd)
{
    SeqStruct *ch;
    u8 b0, b1, b2;
    s32 tempo;
    s16 res;

    ch = SCORE(sep_no) + seq_no;
    switch ((u32)cmd) {
    case 0x51:
        res = ch->resolution;
        b0 = *SND_PTR(ch->data++);
        b1 = *SND_PTR(ch->data++);
        b2 = *SND_PTR(ch->data++);
        tempo = 60000000 / (s32)(((u32)b0 << 16) | ((u32)b1 << 8) | b2);
        ch->tempo = tempo;
        if ((u32)(res * tempo * 10) < (u32)(VBLANK_MINUS * 60)) {
            ch->f108 = (u32)(VBLANK_MINUS * 600) / (u32)(res * ch->tempo);
            ch->f110 = ch->f108;
        } else {
            ch->f108 = -1;
            ch->f110 = (u32)(res * ch->tempo * 10) / (u32)(VBLANK_MINUS * 60);
            if ((u32)(VBLANK_MINUS * 60) / 2 < (u32)(res * ch->tempo * 10) % (u32)(VBLANK_MINUS * 60)) {
                ch->f110++;
            }
        }
        ch->f136 = ReadDeltaValue(sep_no, seq_no);
        break;
    case 0x2F:
        ch->l_current++;
        if (ch->l_count == 0) {
            ch->f128 = 0;
            ch->f39 = 0;
            ch->f136 = 0;
            ch->data = ch->loop_start;
        } else if (ch->l_count != 0) {
            if (ch->l_current < ch->l_count) {
                ch->f128 = 0;
                ch->f39 = 0;
                ch->f136 = 0;
                ch->data = ch->loop_start;
                ch->loop_ptr = ch->loop_start;
            } else {
                SCORE(sep_no)[seq_no].flags &= ~1;
                SCORE(sep_no)[seq_no].flags &= ~8;
                SCORE(sep_no)[seq_no].flags &= ~2;
                SCORE(sep_no)[seq_no].flags |= 0x200;
                SCORE(sep_no)[seq_no].flags |= 4;
                ch->loop_ptr = ch->loop_start;
                ch->f43 = 0;
                if (ch->vab_id != -1) {
                    Snd_nextseq(ch->vab_id, seq_no);
                }
                SpuVmSeqKeyOff((seq_no << 8) | sep_no, seq_no);
                ch->f136 = ch->f110;
            }
        }
        break;
    }
}
