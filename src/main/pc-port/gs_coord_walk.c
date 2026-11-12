/*
 * gs_coord_walk.c -- the GsCOORDINATE2 chain walk of matched GsGetLw,
 * GsGetLws and GsGetLs (src/main/matched/), with the parent link and DSTACK
 * entries kept as 32-bit PS1 handles (pc/include/game/gs_coord.h).  The
 * statement order follows the matched bodies: the same DSTACK fill, the same
 * choice of starting matrix, the same GsMulCoord3 back-propagation that
 * rewrites each ancestor's workm and sets its flg.
 */
#include "types.h"
#include "game/gs_coord.h"

extern u32 DSTACK[];
extern void GsMulCoord3(s32 *a0, s32 *a1);

#define NODE(h) ((s32 *)(unsigned long)(h))

static void copy8(s32 *dst, const s32 *src)
{
    s32 i;
    for (i = 0; i < 8; i++) dst[i] = src[i];
}

void pc_gs_coord_world(u32 node, s32 *out)
{
    s32 counter = 0;
    s32 final_idx = 100;
    u32 next;
    s32 *n;

    for (;;) {
        DSTACK[counter] = node;
        n = NODE(node);
        next = (u32)n[GS_COORD_SUPER];
        if (n[GS_COORD_FLG] == 0) {
            final_idx = counter;
        }
        if (next == 0) {
            break;
        }
        counter++;
        node = next;
    }
    if (counter == 0) {
        copy8(out, NODE(DSTACK[0]) + GS_COORD_COORD);
    } else if (final_idx == 100) {
        counter = 0;
        copy8(out, NODE(DSTACK[0]) + GS_COORD_WORKM);
    } else if (final_idx == counter) {
        copy8(out, NODE(DSTACK[counter]) + GS_COORD_COORD);
    } else {
        counter = final_idx + 1;
        copy8(out, NODE(DSTACK[final_idx + 1]) + GS_COORD_WORKM);
    }
    while (counter > 0) {
        n = NODE(DSTACK[counter - 1]);
        GsMulCoord3(out, n + GS_COORD_COORD);
        copy8(n + GS_COORD_WORKM, out);
        n[GS_COORD_FLG] = 1;
        counter--;
    }
}
