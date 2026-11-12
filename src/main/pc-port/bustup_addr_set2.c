/*
 * bustup_addr_set2 -- PC override of src/main/matched/bustup_addr_set2.c.
 *
 * Statement for statement the matched function; only the handle width
 * differs (pc/include/game/bustup_handles.h: the matched 8-byte `u8 *`
 * stores zero bustup_img3 on LP64).
 *
 * @DivergesFrom: matched bustup_addr_set2 -- store width only.
 */
#include "types.h"
#include "bustup_handles.h"

extern u16 bustup_addr_set2_tbl0[];   /* 3 u16 per entry */

void bustup_addr_set2(s32 a0)
{
    s32 idx;
    u16 *row;

    bustup_clut = BUSTUP_BASE;
    idx = a0 % 32;
    row = &bustup_addr_set2_tbl0[idx * 3];
    BUSTUP_HANDLES_SET(row);
}
