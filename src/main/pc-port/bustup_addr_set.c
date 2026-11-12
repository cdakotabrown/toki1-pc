/*
 * bustup_addr_set -- PC override of src/main/matched/bustup_addr_set.c.
 *
 * Statement for statement the matched function; only the handle width
 * differs (pc/include/game/bustup_handles.h: the matched 8-byte `u8 *`
 * stores zero bustup_img3 on LP64).
 *
 * @DivergesFrom: matched bustup_addr_set -- store width only.
 */
#include "types.h"
#include "bustup_handles.h"

extern u16 bustup_addr_set_tbl0[];   /* 13 entries of 3 u16 offsets */

void bustup_addr_set(s32 a0)
{
    u32 idx;
    u16 *p;

    bustup_clut = BUSTUP_BASE;
    idx = (u32)(a0 % 13);
    p = &bustup_addr_set_tbl0[idx * 3];
    BUSTUP_HANDLES_SET(p);
}
