/*
 * SetControlChange -- SEQ control-change dispatch (PC override of
 * src/main/matched/SetControlChange.c, 0x80092F8C).
 *
 * Statement for statement the matched function; the read pointer at +4 of
 * the 168-byte score record is a 4-byte PS1 handle, so `chan`, `pan`,
 * `prog`, `bend`, `vol` and `slot` keep their PS1 offsets
 * (src/main/pc-port/snd_score.h).
 */
#include "types.h"
#include "snd_score.h"

typedef struct {
    u8  pad0[4];
    u32 rd;          /* +4 (PS1 handle) */
    u8  pad8[10];
    u8  chan;        /* +18 */
    u8  pad19[4];
    u8  pan[21];     /* +23 */
    u8  prog[21];    /* +44 */
    u8  pad65[9];
    s16 bend;        /* +74 */
    s16 vol[21];     /* +76 */
    u8  pad118[18];
    s32 slot;        /* +136 */
    u8  pad140[28];
} SeqRec;

typedef char seqrec_ps1_size[sizeof(SeqRec) == 168 ? 1 : -1];

extern s32  ReadDeltaValue();
extern void ContModulation();
extern void ContPortaTime();
extern void ContDataEntry();
extern void ContPortamento();
extern void ContNrpn1();
extern void ContNrpn2();
extern void ContRpn1();
extern void ContRpn2();
extern void ContResetAll();
extern s32  SpuVmGetProgVol();
extern void SsUtSetReverbDepth();
extern void SpuVmDamperOff();
extern void SpuVmDamperOn();

void SetControlChange(ch, seq, ctl)
short ch;
short seq;
u8 ctl;
{
    SeqRec *rec;
    short idx;
    u8 d;
    short t;

    rec = &((SeqRec *)SND_SCORE(ch))[seq];
    idx = rec->chan;
    d = *SND_PTR(rec->rd++);
    switch (ctl) {
    case 0:
        rec->bend = d;
        rec->slot = ReadDeltaValue(ch, seq);
        break;
    case 1:
        ContModulation(ch, seq, d);
        break;
    case 5:
        ContPortaTime(ch, seq, d);
        break;
    case 6:
        ContDataEntry(ch, seq, d);
        break;
    case 7:
        rec->vol[idx] = d;
        rec->slot = ReadDeltaValue(ch, seq);
        break;
    case 10:
        rec->pan[idx] = d;
        rec->slot = ReadDeltaValue(ch, seq);
        break;
    case 11:
        t = SpuVmGetProgVol(rec->bend, rec->prog[idx]);
        rec->vol[idx] = (t * d) / 127;
        rec->slot = ReadDeltaValue(ch, seq);
        break;
    case 64:
        if (d < 64) {
            SpuVmDamperOff();
        } else {
            SpuVmDamperOn();
        }
        rec->slot = ReadDeltaValue(ch, seq);
        break;
    case 65:
        ContPortamento(ch, seq, d);
        break;
    case 91:
        SsUtSetReverbDepth((short)d, (short)d);
        rec->slot = ReadDeltaValue(ch, seq);
        break;
    case 98:
        ContNrpn1(ch, seq, d);
        break;
    case 99:
        ContNrpn2(ch, seq, d);
        break;
    case 100:
        ContRpn1(ch, seq, d);
        break;
    case 101:
        ContRpn2(ch, seq, d);
        break;
    case 121:
        ContResetAll(ch, seq, d);
        break;
    default:
        rec->slot = ReadDeltaValue(ch, seq);
        break;
    }
}
