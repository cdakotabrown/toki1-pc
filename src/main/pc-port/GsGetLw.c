/*
 * GsGetLw -- PC pointer-width adapter for src/main/matched/GsGetLw.c.
 * The matched body reads the GsCOORDINATE2 parent link as `(s32 *)node[18]`,
 * which sign-extends a PS1 handle into an unmapped host pointer as soon as
 * a coordinate has a super; DSTACK entries are 8 bytes on the host.  Same
 * walk with 32-bit handles (src/main/pc-port/gs_coord_walk.c).
 * Intentional divergence from the PS1: none -- pointer width only.
 */
#include "types.h"
#include "game/gs_coord.h"

void GsGetLw(void *coord, s32 *output)
{
    pc_gs_coord_world((u32)(unsigned long)coord, output);
}
