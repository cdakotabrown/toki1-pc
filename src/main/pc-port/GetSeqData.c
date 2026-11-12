/*
 * GetSeqData -- fetch and dispatch the next SEQ event (PC override of
 * src/main/matched/GetSeqData.c, 0x80092BAC).
 *
 * Statement for statement the matched function; the read pointer at +4 of
 * the 168-byte score record is a 4-byte PS1 handle, so `status`, `chan`
 * and `slot` keep their PS1 offsets (src/main/pc-port/snd_score.h).
 */
#include "types.h"
#include "snd_score.h"

typedef struct {
    u8  pad0[4];
    u32 rd;          /* +4   read pointer into the SEQ stream (PS1 handle) */
    u8  pad8[9];
    u8  status;      /* +17  last status byte (0xFF = meta) */
    u8  chan;        /* +18  channel nibble */
    u8  pad19[117];
    s32 slot;        /* +136 delta from ReadDeltaValue */
    u8  pad140[28];
} SeqRec;

typedef char seqrec_ps1_size[sizeof(SeqRec) == 168 ? 1 : -1];

extern s32  ReadDeltaValue();
extern void NoteOn();
extern void SetControlChange();
extern void SetProgramChange();
extern void SetPitchBend();
extern void GetMetaEvent();

#define RD(rec) (*SND_PTR((rec)->rd++))

void GetSeqData(ch, seq)
short ch;
short seq;
{
    SeqRec *rec;
    u8 ev;
    u8 d1;
    u8 d2;
    u8 hi;
    u8 cur;
    u8 st;

    rec = &((SeqRec *)SND_SCORE(ch))[seq];
    ev = RD(rec);
    if (ev & 0x80) {
        hi = ev & 0xF0;
        rec->chan = ev & 0x0F;
        switch (hi) {
        case 0x90:
            rec->status = 0x90;
            d1 = RD(rec);
            d2 = RD(rec);
            rec->slot = ReadDeltaValue(ch, seq);
            NoteOn(ch, seq, d1, d2);
            break;
        case 0xB0:
            rec->status = 0xB0;
            cur = RD(rec);
            SetControlChange(ch, seq, cur);
            break;
        case 0xC0:
            rec->status = 0xC0;
            cur = RD(rec);
            SetProgramChange(ch, seq, cur);
            break;
        case 0xE0:
            rec->status = 0xE0;
            cur = RD(rec);
            SetPitchBend(ch, seq, cur);
            break;
        case 0xF0:
            rec->status = 0xFF;
            cur = RD(rec);
            GetMetaEvent(ch, seq, cur);
            break;
        }
    } else {
        st = rec->status;
        switch (st) {
        case 0x90:
            d2 = RD(rec);
            rec->slot = ReadDeltaValue(ch, seq);
            NoteOn(ch, seq, ev, d2);
            break;
        case 0xB0:
            SetControlChange(ch, seq, ev);
            break;
        case 0xC0:
            SetProgramChange(ch, seq, ev);
            break;
        case 0xE0:
            SetPitchBend(ch, seq, ev);
            break;
        case 0xFF:
            GetMetaEvent(ch, seq, ev);
            break;
        }
    }
}
