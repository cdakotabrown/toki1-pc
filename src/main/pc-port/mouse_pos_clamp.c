/* mouse_pos_clamp: host adapter for matched/mouse_pos_clamp.c.
 * Preserve the PS1 four-byte mouse record handles and zero-extend loads.
 * Calls, state writes and branch order follow the matched body.
 * Host pointer width/calling convention are the only divergences.
 */
#include "types.h"

extern u32 mouse_rec_tbl[];
extern s32 mouse_x_acc[];
extern s32 mouse_y_acc[];
extern s32 mouse_x_min;
extern s32 mouse_x_max;
extern s32 mouse_y_min;
extern s32 mouse_y_max;

void mouse_pos_clamp(s32 idx) {
    s8 *rec = (s8 *)(unsigned long)mouse_rec_tbl[idx];
    s32 x;

    if (rec[0] != 0) return;
    if (rec[1] != 18) return;

    x = rec[4] + mouse_x_acc[idx];
    mouse_x_acc[idx] = x;
    mouse_y_acc[idx] = rec[5] + mouse_y_acc[idx];

    if (mouse_x_max < x) {
        mouse_x_acc[idx] = mouse_x_max;
    } else if (x < mouse_x_min) {
        mouse_x_acc[idx] = mouse_x_min;
    }
    if (mouse_y_max < mouse_y_acc[idx]) {
        mouse_y_acc[idx] = mouse_y_max;
        return;
    }
    if (mouse_y_acc[idx] < mouse_y_min) {
        mouse_y_acc[idx] = mouse_y_min;
    }
}
