/*
 * GsGetLs -- PC pointer-width adapter for src/main/matched/GsGetLs.c.
 * Local-screen matrix: the chain's world matrix, then GsWSMATRIX * world.
 * The matched body declares the parent link as an 8-byte `struct Node *`
 * at +72, so on the host it also takes the following record's first word
 * as the pointer's high half.  32-bit handles here
 * (src/main/pc-port/gs_coord_walk.c).
 * Intentional divergence from the PS1: none -- pointer width only.
 */
#include "types.h"
#include "game/gs_coord.h"

extern s32 GsWSMATRIX;
extern void GsMulCoord2(s32 *a0, s32 *a1);

void GsGetLs(void *coord, s32 *output)
{
    pc_gs_coord_world((u32)(unsigned long)coord, output);
    GsMulCoord2(&GsWSMATRIX, output);
}
