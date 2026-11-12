/* MouseState: host adapter for matched/MouseState.c.
 * Preserve the PS1 four-byte mouse record handles and zero-extend loads.
 * Calls, state writes and branch order follow the matched body.
 * Host pointer width/calling convention are the only divergences.
 */
#include "types.h"

extern u32 mouse_rec_tbl[];

s32 MouseState(s32 idx, s8 *out) {
    s32 r = 0;

    if (((s8 *)(unsigned long)mouse_rec_tbl[idx])[0] != 0) {
        out[3] = 0;
        out[0] = 0;
        out[1] = 0;
        out[2] = 0;
        return -1;
    }
    out[0] = ((s8 *)(unsigned long)mouse_rec_tbl[idx])[4];
    out[1] = ((s8 *)(unsigned long)mouse_rec_tbl[idx])[5];
    out[2] = ((s8 *)(unsigned long)mouse_rec_tbl[idx])[3];
    out[3] = ((s8 *)(unsigned long)mouse_rec_tbl[idx])[1];
    return r;
}
