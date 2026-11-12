/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gs_coord.h -- PS1-width GsCOORDINATE2 chain walk shared by the port's
 * GsGetLw / GsGetLws / GsGetLs adapters (src/main/pc-port/).
 *
 * A GsCOORDINATE2 is { u32 flg; MATRIX coord; MATRIX workm; u32 param;
 * u32 super; } -- the parent link is a 32-bit PS1 handle at +72.  The
 * matched C reads it as `(s32 *)node[18]` (GsGetLw/GsGetLws) or as an
 * 8-byte `struct Node *next` at +72 (GsGetLs): on the host the first
 * sign-extends 0x8xxxxxxx to an unmapped pointer and the second also takes
 * the next record's first word as the pointer's high half.  Both are fine
 * only while every super is 0.  DSTACK holds 32-bit handles, as on the PS1.
 */
#ifndef GAME_GS_COORD_H
#define GAME_GS_COORD_H

#include "types.h"

#define GS_COORD_FLG    0   /* word index: u32 flg */
#define GS_COORD_COORD  1   /* MATRIX coord, 8 words */
#define GS_COORD_WORKM  9   /* MATRIX workm, 8 words */
#define GS_COORD_SUPER  18  /* u32 super */
#define GS_COORD_DEPTH  100 /* DSTACK capacity (GsGetLw/GsGetLws extern) */

/* Composes the chain from `node` up to the root into out[8] (a MATRIX),
 * reusing and refreshing each ancestor's cached workm exactly like the
 * retail walk (flg == 0 marks a node whose workm is stale). */
void pc_gs_coord_world(u32 node, s32 *out);

#endif
