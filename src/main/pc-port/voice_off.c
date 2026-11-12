/*
 * voice_off -- PC override of src/main/matched/voice_off.c (0x8007B65C).
 *
 * Statement for statement the matched function; the divergence is the
 * arguments.  The matched C is `void voice_off(void) { SsVoKeyOff(); }`
 * and matches because the listing never touches $a0/$a1 (addiu sp; sw ra;
 * jal 0x80090B50): the console runs SsVoKeyOff(a0, a1) with voice_off's own
 * arguments, which its callers pass -- cmd_voice5..8 call
 * voice_off(color, yoff).  SsVoKeyOff(xy, wh) keys off the voice
 * (xy >> 8, xy & 0xFF, wh >> 8, wh & 0xFF).  On x86-64 the bare call passed
 * whatever %edi/%esi held, so the wrong voice (or none) was keyed off.
 * pc/tools/audit_ps1_call_args.py SHORT-READ.
 *
 * @DivergesFrom: src/main/matched/voice_off.c -- the arguments the console
 * passes in $a0/$a1 are written out.
 */
#include "types.h"

extern void SsVoKeyOff(s32 xy, s32 wh);

void voice_off(s32 xy, s32 wh) {
    SsVoKeyOff(xy, wh);
}
