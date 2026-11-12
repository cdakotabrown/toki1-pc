/*
 * GsSetLsMatrix -- PC port override (argument-shape divergence).
 *
 * Retail (@0x8009B65C, Psy-Q GCC 2.6.0) keeps the incoming matrix pointer in
 * $s0 and re-loads it into $a0 before each callee:
 *
 *     move  $s0, $a0
 *     move  $a0, $s0          (for the SetRotMatrix call)
 *     jal   SetRotMatrix
 *     move  $a0, $s0          (for the SetTransMatrix call)
 *     jal   SetTransMatrix
 *
 * The matched C describes that as `SetRotMatrix(); SetTransMatrix(arg0);` --
 * faithful to the instruction stream (the register moves are real), but on the
 * host the first call's argument has no C-level carrier, so `SetRotMatrix`
 * receives an unrelated register value.  Measured consequence on scene 18: the
 * GTE rotation matrix never received the object's Ls matrix, every vertex
 * projected with SZ = 0 (`flag = 0x00020000`, divide overflow) and the title's
 * quads smeared across the whole screen.
 *
 * Handing the same pointer to both callees is the console's data flow with a
 * C signature; the matched PS1 source stays untouched for the byte-exact build.
 */
#include "types.h"
#include "hal.h"

extern void SetRotMatrix(MATRIX *m);
extern void SetTransMatrix(MATRIX *m);

void GsSetLsMatrix(MATRIX *m)
{
    SetRotMatrix(m);
    SetTransMatrix(m);
}