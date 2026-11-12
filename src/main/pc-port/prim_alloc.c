/*
 * prim_alloc -- bump-allocate from the stride-12 screen_w record (PC
 * override of src/main/matched/prim_alloc.c).
 *
 * Statement for statement the matched function; the ONE divergence is
 * addressing (src/main/pc-port/mv_state.h doctrine -- the pointer width is
 * a property of the host, not of the game): the bank base at p+36 is a
 * 4-byte PS1 pointer (prim_buf_init stores `buf` as s32 there).  The native
 * 8-byte `*(s32 **)(p + 36)` load folds the neighbouring record's size
 * field (p+40: 0x10C00) into the pointer's high half, so every allocation
 * returns a wild address and the prim setters SIGSEGV on it (measured:
 * the title's white-out, _sprite_set_box_shade -> SetPolyG4 with the
 * garbage prim; scene-matrix first-fail class).  Read as u32 and
 * zero-extended -- the address the console's `lw` produces.
 * The retail cursor update is also required: omitting it aliases successive
 * packets and makes repeated AddPrim submissions self-link.
 */
#include "types.h"

extern u8 screen_w[];

s32 *prim_alloc(s32 n, s32 slot) {
    u8 *p = &screen_w[slot * 12];
    s32 *r = (s32 *)(unsigned long)*(u32 *)(p + 36) + *(s32 *)(p + 32);
    *(s32 *)(p + 32) += ((n + 3) / 4) * 4;
    return r;
}
