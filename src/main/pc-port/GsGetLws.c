/*
 * GsGetLws -- PC pointer-width adapter for src/main/matched/GsGetLws.c.
 * Local-world (light) and local-screen matrices of one coordinate: the
 * world matrix of the chain, then GsWSMATRIX * world.  The parent link and
 * DSTACK are 32-bit PS1 handles (src/main/pc-port/gs_coord_walk.c); the
 * matched body sign-extends the link into an unmapped host pointer.
 * Intentional divergence from the PS1: none -- pointer width only.
 */
#include "types.h"
#include "game/gs_coord.h"

extern s32 GsWSMATRIX;
extern void GsMulCoord2(s32 *a0, s32 *a1);

void GsGetLws(void *coord, s32 *output, s32 *dest)
{
    s32 i;
    pc_gs_coord_world((u32)(unsigned long)coord, output);
    for (i = 0; i < 8; i++) dest[i] = output[i];
    GsMulCoord2(&GsWSMATRIX, dest);
}
