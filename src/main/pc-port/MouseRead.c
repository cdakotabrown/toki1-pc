/* MouseRead: host adapter for matched/MouseRead.c.
 * Preserve the PS1 four-byte mouse record handles and zero-extend loads.
 * Calls, state writes and branch order follow the matched body.
 * Pass the index explicitly: retail keeps a0 live across the clamp call;
 * the host calling convention does not preserve an undeclared argument.
 * Host pointer width/calling convention are the only divergences.
 */
#include "types.h"

extern u32 mouse_rec_tbl[];
extern u32 mouse_x_acc[];
extern u32 mouse_y_acc[];
extern u16 libapi_mouse_sense_x;
extern u16 libapi_mouse_sense_y;
extern void mouse_pos_clamp(s32 idx);

void MouseRead(s32 a0, s32 *a1) {
    s8 *ptr;

    mouse_pos_clamp(a0);

    a1[0] = mouse_x_acc[a0] / libapi_mouse_sense_x;
    a1[1] = mouse_y_acc[a0] / libapi_mouse_sense_y;
    a1[2] = -256;

    ptr = (s8 *)(unsigned long)mouse_rec_tbl[a0];
    if (ptr[0] == 0) {
        if (ptr[1] == 18) {
            a1[2] = ~ptr[3] & 0xC;
        }
    }
}
