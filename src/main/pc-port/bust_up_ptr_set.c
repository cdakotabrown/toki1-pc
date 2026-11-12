/*
 * bust_up_ptr_set -- PC override of src/main/matched/bust_up_ptr_set.c.
 *
 * Statement for statement the matched function; the divergence is the
 * handle WIDTH (pc/include/game/bustup_handles.h).  The matched `u8 *`
 * stores are 8 bytes on LP64 and the img2 store zeroed bustup_img3, so
 * week_day_bustup_init gave class_w slot 96 (cal_cw) a null header, the
 * slot never animated, cal_cw_mode bit 0 never cleared and every lesson
 * day ran week_day_main0's 601-frame step_counter timeout (retail: 46).
 *
 * @DivergesFrom: matched bust_up_ptr_set -- store width only.
 */
#include "types.h"
#include "bustup_handles.h"

extern u16 bust_up_ptr_set_tbl0[];   /* 3 u16 per entry */

void bust_up_ptr_set(s32 arg0)
{
    u32 idx;
    u16 *row;

    bustup_clut = BUSTUP_BASE;
    idx = (u32)(arg0 % 25);
    row = &bust_up_ptr_set_tbl0[idx * 3];
    BUSTUP_HANDLES_SET(row);
}
