/*
 * voice_restart -- PC override of src/main/matched/voice_restart.c
 * (0x8007B5F8).
 *
 * Statement for statement the matched function except the SsVoKeyOff
 * call.  The matched C calls `SsVoKeyOff()` bare; the listing homes a0/a1
 * around the jal without writing them (sw a0,40(sp); jal 0x80090B50;
 * sw a1,44(sp)), so the console runs SsVoKeyOff(arg0, arg1) -- the voice it
 * then keys on again with SsVoKeyOn(arg0, arg1, ...).  On x86-64 the bare
 * call passed whatever %edi/%esi held.  pc/tools/audit_ps1_call_args.py
 * SHORT-READ.
 *
 * @DivergesFrom: src/main/matched/voice_restart.c -- the arguments the
 * console passes in $a0/$a1 are written out.
 */

extern void SsVoKeyOff(int, int);
extern void SsSetMVol(int, int);
extern void SsVoKeyOn(int, int, int, int);

extern int gSDInfo;
extern unsigned short voice_vol_l;

void voice_restart(int arg0, int arg1)
{
    SsVoKeyOff(arg0, arg1);
    gSDInfo = gSDInfo & 0xFFEFFFFF;
    SsSetMVol(127, 127);
    SsVoKeyOn(arg0, arg1, voice_vol_l, voice_vol_l);
}
