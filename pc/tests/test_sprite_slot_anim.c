/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sprite_slot_disp's animation step (pc/src/sprite_slot_anim.c) against the
 * matched body's rules (src/main/matched/sprite_slot_disp.c).  The old
 * override copy failed the reverse, ping-pong and every-frame end checks
 * below: RPG_BAT's message window (mode 33) never closed. */
#include <string.h>
#include "../src/sprite_slot_anim.c"

static void slot(u8 *s, u8 f2, u8 f3, s16 tick, s16 frame)
{
    memset(s, 0, 68);
    s[2] = f2;
    s[3] = f3;
    *(s16 *)(s + 8) = tick;
    *(s16 *)(s + 24) = frame;
}

#define FRAME(s) (*(s16 *)((s) + 24))
#define TICK(s) (*(s16 *)((s) + 8))

int test_sprite_slot_anim(void)
{
    /* a 4-frame row, 2 ticks per frame */
    static const s16 row[] = {4, 0, 2, 0, 2, 0, 2, 0, 2, 0};
    u8 s[68];

    /* reverse play (rpg_bat_anim_set mode 33 = 0x21) from frame 0: one step
     * stops it, clearing bits 0 and 5 (rpg_bat_rec_flag_check passes) */
    slot(s, 0x21, 0x80, 2, 0);
    sprite_slot_anim_step(s, row, 0);
    if (FRAME(s) != -1 || s[2] != 0x00 || TICK(s) != 0) return __LINE__;
    /* reverse play from frame 2: 1, then 0 stops it */
    slot(s, 0x21, 0x80, 2, 2);
    sprite_slot_anim_step(s, row, 2);
    if (FRAME(s) != 1 || s[2] != 0x21) return __LINE__;
    TICK(s) = 2;
    sprite_slot_anim_step(s, row, 1);
    if (FRAME(s) != 0 || s[2] != 0x00) return __LINE__;
    /* tick below the duration: the frame holds, the end check still runs
     * (a frame index past the row resets it) */
    slot(s, 0x01, 0x80, 0, 5);
    sprite_slot_anim_step(s, row, 0);
    if (FRAME(s) != 0 || s[2] != 0x01) return __LINE__;
    /* forward play (mode 5 = 0x05): stops on the last frame, bits 0 and 2
     * cleared */
    slot(s, 0x05, 0x80, 2, 3);
    sprite_slot_anim_step(s, row, 3);
    if (FRAME(s) != 3 || s[2] != 0x00) return __LINE__;
    /* loop once (0x03): back to 0, bits 0..1 cleared */
    slot(s, 0x03, 0x80, 2, 3);
    sprite_slot_anim_step(s, row, 3);
    if (FRAME(s) != 0 || s[2] != 0x00) return __LINE__;
    /* play and hide (0x09): f3's visible bit cleared */
    slot(s, 0x09, 0x80, 2, 3);
    sprite_slot_anim_step(s, row, 3);
    if (FRAME(s) != 0 || s[2] != 0x08 || s[3] != 0x00) return __LINE__;
    /* ping-pong (0x11): forward clears 0x20 only, the turn sets 0x20 and
     * steps back one frame, or three with f3 bit 3 */
    slot(s, 0x31, 0x80, 2, 1);
    sprite_slot_anim_step(s, row, 1);
    if (FRAME(s) != 2 || s[2] != 0x11) return __LINE__;
    slot(s, 0x11, 0x80, 2, 3);
    sprite_slot_anim_step(s, row, 3);
    if (FRAME(s) != 3 || s[2] != 0x21) return __LINE__;
    slot(s, 0x11, 0x88, 2, 3);
    sprite_slot_anim_step(s, row, 3);
    if (FRAME(s) != 1 || s[2] != 0x21) return __LINE__;
    return 0;
}
