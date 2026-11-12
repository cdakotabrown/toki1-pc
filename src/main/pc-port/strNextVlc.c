/* Native layout adapter for matched/strNextVlc.c (0x80056B7C).
 * PS1 arg0[2] is vlcid; on a 64-bit host it aliases vlcbuf[1].
 * Keep stream data and VLC buffers as full pointers, with the original
 * polling, toggle, decode and ring-release order. */
#include <stddef.h>
#include <stdint.h>
#include "mv_state.h"

extern u8 *strNext(MvDec *dec);
extern s32 StFreeRing(s32 address);
extern void DecDCTvlc(void *data, void *buffer);

s32 strNextVlc(MvDec *dec)
{
    u8 *result;
    s32 counter;

    counter = 0x800000;
    result = strNext(dec);

    if (result == NULL) {
        do {
            counter--;
            if (counter == 0) {
                return -1;
            }
            StFreeRing((s32)(uintptr_t)result);
            result = strNext(dec);
        } while (result == NULL);
    }

    if (dec->vlcid != 0) {
        dec->vlcid = 0;
    } else {
        dec->vlcid = 1;
    }

    DecDCTvlc(result, dec->vlcbuf[dec->vlcid]);
    /* The host ring release is a no-op with the existing PS1 integer ABI. */
    StFreeRing((s32)(uintptr_t)result);

    return 0;
}
