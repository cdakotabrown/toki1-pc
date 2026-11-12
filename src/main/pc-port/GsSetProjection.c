/*
 * GsSetProjection -- PC port override (argument-shape divergence).
 *
 * Retail (@0x8009CB90, 32 bytes, Psy-Q GCC 2.6.0) is a tail call:
 *
 *     GsSetProjection:
 *         jr      $ra
 *         move    $a0, $a0        <- the argument just rides through $a0
 *
 * The matched C in src/main/matched/GsSetProjection.c describes that shape
 * faithfully as `void GsSetProjection(void) { SetGeomScreen(); }` because the
 * decompiler had no prototype for the callee.  On the MIPS ABI that is
 * correct: SetGeomScreen reads $a0 and gets the caller's value.
 *
 * On x86-64 it is not.  The C prototype discards the argument, so the HAL's
 * SetGeomScreen() receives whatever happens to be in the first argument
 * register -- and the projection distance (H) becomes garbage.  Symptom,
 * measured on scene 18: the title's quads projected to screen-filling
 * polygons instead of the logo.
 *
 * Every caller passes 1000 (`title_init_View`, `initView`), and on the console
 * that value reaches GTE control register 26.  Passing it through explicitly
 * is the whole difference: same instructions, same data flow, and the matched
 * PS1 source is left untouched for the byte-perfect build.
 */
#include "types.h"

extern void SetGeomScreen(s32 h);

void GsSetProjection(s32 h)
{
    SetGeomScreen(h);
}