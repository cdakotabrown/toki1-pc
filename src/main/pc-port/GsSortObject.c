/*
 * GsSortObject -- PC pointer-width adapter for matched/GsSortObject.c.
 * Statement-for-statement matched body; only host pointer width diverges.
 * PS1 object and TMD pointer fields are u32 handles at console offsets;
 * dereferences zero-extend them rather than reading fused native pointers.
 * GsOUT_PACKET_P and the lighting dispatch cell remain four-byte globals.
 * See mv_state.h. No rendering/dispatch logic or call order is changed.
 */
#include "types.h"

typedef s32 (*DispatchFunc)(s32, s32, s32, s32);

typedef struct {
    u32 flags;      /* 0x00 */
    u32 unk04[26];  /* 0x04 */
    u32 seq;        /* 0x6C */
} SsVoice;

extern s32 libgs_obj_attr_bits0to2;
extern s32 GsLMODE;
extern s32 GsLIGNR;
extern s32 GsLIOFF;
extern s32 libgs_obj_attr_bit7;
extern s32 libgs_obj_attr_bit8;
extern s32 GsNDIV;
extern s32 GsTRATE;
extern s32 GsTON;
extern s32 GsDISPON;
extern s32 GsLIGHT_FUNC;
extern s32 GsLSMATRIX;
extern s32 GsLIGHT_MODE;
extern s32 GsOUT_PACKET_P;

extern void SetRotMatrix(s32 *);
extern void SetTransMatrix(s32 *);

extern s32 Gslight_normal();
extern s32 Gslight_normal_fog();
extern s32 Gslight_mate();
extern s32 Gslight_mate_fog();

void GsSortObject(SsVoice *arg0, s32 *arg1, s32 arg2) {
    s32 *seq;
    s32 *base;
    u32 *p;
    u32 *q;
    DispatchFunc f;
    s32 count;
    u32 i;
    u32 b;
    s32 sel;
    u32 t;

    seq = (s32 *)(unsigned long)arg0->seq;

    libgs_obj_attr_bits0to2 = arg0->flags & 0x7;
    GsLMODE = (arg0->flags >> 3) & 0x3;
    GsLIGNR = (arg0->flags >> 5) & 0x1;
    GsLIOFF = (arg0->flags >> 6) & 0x1;
    libgs_obj_attr_bit7 = (arg0->flags >> 7) & 0x1;
    libgs_obj_attr_bit8 = (arg0->flags >> 8) & 0x1;
    GsNDIV = (arg0->flags >> 9) & 0x3;
    GsTRATE = (arg0->flags >> 28) & 0x3;
    GsTON = (arg0->flags >> 30) & 0x1;
    GsDISPON = (arg0->flags >> 31) & 0x1;

    SetRotMatrix(&GsLSMATRIX);
    SetTransMatrix(&GsLSMATRIX);

    if (GsDISPON == 1) {
        return;
    }

    if (GsLIGNR == 0) {
        sel = GsLIGHT_MODE;
        if (sel == 1) goto k1;
        if (sel < 2) {
            if (sel == 0) goto k0;
            goto skip;
        }
        if (sel == 2) goto k2;
        goto t3;
    k0:
        f = (DispatchFunc)Gslight_normal;
        goto store;
    k1:
        f = (DispatchFunc)Gslight_normal_fog;
        goto store;
    k2:
        f = (DispatchFunc)Gslight_mate;
        goto store;
    }

    sel = GsLMODE;
    if (sel == 1) goto c1;
    if (sel == 0) goto c0;
    if (sel == 2) goto c2;
t3:
    if (sel == 3) goto c3;
    goto skip;
c0:
    f = (DispatchFunc)Gslight_normal;
    goto store;
c1:
    f = (DispatchFunc)Gslight_normal_fog;
    goto store;
c2:
    f = (DispatchFunc)Gslight_mate;
    goto store;
c3:
    f = (DispatchFunc)Gslight_mate_fog;

store:
    GsLIGHT_FUNC = (s32)(unsigned long)f;

skip:
    f = (DispatchFunc)(unsigned long)(u32)GsLIGHT_FUNC;
    base = (s32 *)(unsigned long)(u32)arg1[1];
    p = (u32 *)(unsigned long)(u32)GsOUT_PACKET_P;
    count = f(seq[4], seq[0], seq[2], seq[5]);

    for (i = 0; i < (u32)count; i++) {
        t = p[0];
        t &= 0xFFFFFF;
        t = (t - arg1[2]) >> arg2;
        t <<= 2;
        t += (u32)(unsigned long)base;
        q = (u32 *)(unsigned long)t;
        b = ((u8 *)p)[3];
        p[0] = (*q & 0xFFFFFF) | (b << 24);
        *q = (*q & 0xFF000000) | ((u32)(unsigned long)p & 0xFFFFFF);
        p = (u32 *)((u8 *)p + (b << 2) + 4);
    }
}
