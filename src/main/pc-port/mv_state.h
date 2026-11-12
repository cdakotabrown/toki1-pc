/* mv_state.h -- the movie player's state structs, as src/main/matched/
 * movie_main.c and strSetDefDecEnv.c declare them.
 *
 * On the PS1 every pointer is 4 bytes, so the matched libstr helpers
 * (strNext, strNextVlc, strSync, strCallback) could reach the same fields
 * through hand-written byte offsets (`movie_state + 60`, `field_16`,
 * `arg0[arg0[2]]`, a BlurState with pad arrays) and agree with the typed
 * access in movie_main.  On a 64-bit host MvDec holds three 8-byte pointers,
 * the offsets move, and the two spellings read different bytes.  The PC
 * overrides of those four helpers therefore use these typedefs -- same
 * statements, same calls, same order as the matched C; the pointer width is
 * the only divergence, and it is a property of the host, not of the game. */
#ifndef PC_MV_STATE_H
#define PC_MV_STATE_H
#include "types.h"

typedef struct {
    s16 x, y, w, h;
} MvRect;

typedef struct {
    u32 *vlcbuf[2];     /* PS1 +4  (two VLC output buffers) */
    s32 vlcid;          /* PS1 +12 */
    u16 *imgbuf;        /* PS1 +16 (one 16-px column slice, decoded) */
    MvRect rect[2];     /* PS1 +20 (the two display rectangles) */
    s32 rectid;         /* PS1 +36 */
    MvRect slice;       /* PS1 +40 (current slice position / size) */
    s32 isdone;         /* PS1 +48 (frame fully uploaded) */
} MvDec;

typedef struct {
    u8 loc[4];          /* PS1 0  (CdlLOC of the stream start) */
    MvDec dec;          /* PS1 4 */
    s32 field_52;       /* PS1 52 (1 = the stream reached movie_end_frame) */
    s32 pad56;
    s32 width;          /* PS1 60 */
    s32 height;         /* PS1 64 */
} MvState;

#endif
