/* strNext -- next video frame of the stream (PC override of src/main/matched/strNext.c).
 * Address: 0x8005A6B0 (libstr sample code linked into the game)
 *
 * Statement for statement the matched function; the ONE divergence is
 * addressing (see src/main/pc-port/mv_state.h): the matched C reaches the
 * player state through PS1 byte offsets (`movie_state + 52/60/64` for
 * field_52 / width / height, the MvDec as an `s16 *` with rect/slice fields
 * at [10],[11],[14],[15],[21]) that assume 4-byte pointers inside MvDec.
 * The header the frame comes with is read exactly as the matched code reads
 * it (STR sector header: +8 frame number, +16 width, +18 height, +20/+24 the
 * first two words of the frame data, which the data must repeat). */
#include <stdint.h>
#include "types.h"
#include "mv_state.h"

extern s32 StGetNext(s32 *outA, s32 *outB);
extern s32 StFreeRing(s32 addr);
extern void ClearImage(s16 *, s32, s32, s32);
extern u32 movie_end_frame;
extern u32 movie_main_tbl0;
extern u32 movie_main_tbl1;
extern MvState *movie_state;

u8 *strNext(MvDec *dec) {
    u8 *sp54;
    u8 *sp50;
    s16 rect[4];
    s32 var_s0;
    s32 resW;
    s32 v1;

    var_s0 = 0x800000;
    while (StGetNext((s32 *)&sp54, (s32 *)&sp50) != 0) {
        var_s0--;
        if (var_s0 == 0) {
            return (u8 *)0;
        }
    }

    if (*(u32 *)(sp50 + 8) >= movie_end_frame) {
        movie_state->field_52 = 1;
    }

    if (*(s32 *)(sp50 + 20) != *(s32 *)(sp54 + 0) ||
        *(s32 *)(sp50 + 24) != *(s32 *)(sp54 + 4)) {
        StFreeRing((s32)(uintptr_t)sp54);
        return (u8 *)0;
    }

    movie_main_tbl0 = *(u32 *)(sp50 + 8);

    resW = movie_state->width;
    if (*(u16 *)(sp50 + 16) != (u32)resW ||
        *(u16 *)(sp50 + 18) != (u32)movie_state->height) {
        rect[0] = 0;
        rect[1] = 0;
        rect[2] = 640;
        rect[3] = 480;
        ClearImage(rect, 0, 0, 0);

        movie_state->width = *(u16 *)(sp50 + 16);
        movie_state->height = *(u16 *)(sp50 + 18);

        resW = movie_state->width;
    }

    if (movie_main_tbl1 & 1) {
        v1 = (resW * 3) / 2;
        dec->rect[1].w = (s16)v1;
        dec->rect[0].w = (s16)v1;
    } else {
        dec->rect[1].w = (s16)resW;
        dec->rect[0].w = (s16)resW;
    }

    v1 = (s16)movie_state->height;
    dec->rect[1].h = (s16)v1;
    dec->rect[0].h = (s16)v1;

    dec->slice.h = (s16)movie_state->height;

    return sp54;
}
