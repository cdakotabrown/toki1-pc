/* Native layout adapter for matched/strSync.c (0x80056E18).
 * The PS1 BlurState offsets address MvDec fields only with 32-bit pointers.
 * In native MvDec, +0x2c is rect[1].w/h, NOT the completion flag.
 * Preserve the polling, timeout toggle and slice selection; movie_main
 * ignores the original indeterminate return value and declares void. */
#include "mv_state.h"

void strSync(MvDec *dec, s32 mode)
{
    s32 counter;
    s32 *cp;

    (void)mode;
    cp = &counter;
    *cp = 0x800000;

    if (dec->isdone != 0) {
        goto done;
    }

    do {
        *cp = *cp - 1;
        if (*cp == 0) {
            dec->isdone = 1;
            if (dec->rectid != 0) {
                dec->rectid = 0;
            } else {
                dec->rectid = 1;
            }
            dec->slice.x = dec->rect[dec->rectid].x;
            dec->slice.y = dec->rect[dec->rectid].y;
        }
    } while (dec->isdone == 0);

done:
    dec->isdone = 0;
}
