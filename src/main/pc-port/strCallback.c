/* strCallback -- MDEC out-DMA completion: upload the decoded slice, ask for
 * the next (PC override of src/main/matched/strCallback.c).
 *
 * Statement for statement the matched function; the ONE divergence is
 * addressing (src/main/pc-port/mv_state.h): the matched C names the player
 * state through a `SpotState` of pad arrays -- field_16 (dec.imgbuf), the
 * rects at +20 + counter*8 (dec.rect[rectid]), counter (rectid), xywh
 * (slice), field_48 (isdone) -- offsets that assume 4-byte pointers.
 *
 * On the PS1 the MDEC fires this from the DMA interrupt; here
 * pc/hal/libpress.c calls it at the end of DecDCTout, so the DecDCTout()
 * below recurses once per remaining slice (frame width / slice width). */
#include "types.h"
#include "mv_state.h"

extern void StCdInterrupt(void);
extern void LoadImage(s16 *pos, void *data);
extern s32 DrawSync(s32 arg);
extern void DecDCTout(void *buf, s32 nwords);

extern s32 movie_main_tbl1;
extern s32 song_set_tbl0;
extern s32 movie_offset_y;
extern s32 StCdIntrFlag;
extern MvState *movie_state;

void strCallback(void)
{
    MvRect pos;
    s32 flag;
    MvDec *dec = &movie_state->dec;

    flag = movie_main_tbl1 & 1;

    if (flag != 0) {
        if (StCdIntrFlag != 0) {
            StCdInterrupt();
            flag = movie_main_tbl1 & 1;
            StCdIntrFlag = 0;
        }
    }

    pos = dec->slice;

    if (flag != 0) {
        pos.x = pos.x + (song_set_tbl0 * 3 + 2) / 3;
        pos.y = pos.y + (movie_offset_y * 3 + 2) / 3;
    } else {
        pos.x = pos.x + song_set_tbl0;
        pos.y = pos.y + movie_offset_y;
    }

    LoadImage((s16 *)&pos, dec->imgbuf);
    DrawSync(0);

    dec->slice.x = dec->slice.x + dec->slice.w;

    if (dec->slice.x < dec->rect[dec->rectid].w + dec->rect[dec->rectid].x) {
        DecDCTout(dec->imgbuf, ((s32)dec->slice.w * (s32)dec->slice.h) / 2);
        return;
    }

    dec->isdone = 1;

    if (dec->rectid != 0) {
        dec->rectid = 0;
    } else {
        dec->rectid = 1;
    }

    dec->slice.x = dec->rect[dec->rectid].x;
    dec->slice.y = dec->rect[dec->rectid].y;
}
