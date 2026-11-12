/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The animation step of sprite_slot_disp (0x80047A00), for the PC-port
 * override src/main/pc-port/sprite_slot_disp.c.
 *
 * It is the tail of the matched body (src/main/matched/sprite_slot_disp.c,
 * the `!(f2 & 0x40) && (f2 & 1) == 1` block after the tick update), on a
 * class_w slot in its PS1 layout: f2 at +2, f3 at +3, tick (s16) at +8,
 * frame (s16) at +24.  ROW is the slot's animation row anims[anim]:
 * row[0] the frame count, row[fi * 2 + 2] frame fi's duration in ticks;
 * FI is the frame index the slot was drawn with.
 *
 * Kept apart so pc/tests/test_sprite_slot_anim.c can run it.  The override
 * used to carry its own copy, which kept f2 in a local and wrote it back
 * only on the end-of-row arms: a reverse play (f2 0x20, rpg_bat_anim_set's
 * mode 33: RPG_BAT's message window closing) that reached frame 0 never
 * cleared bits 0 and 5, the frame ran on below 0, rpg_bat_rec_flag_check
 * waited forever and the 98/2/28 robot battle stopped at its first line.
 * The copy also skipped the end-of-row check on frames whose tick was
 * below the duration, cleared bit 0 on the ping-pong arm (f2 &= ~0x21
 * where the PS1 clears 0x20 only), and ignored f3 bit 3 when a ping-pong
 * row turns (frame -= 3).
 */
#include "types.h"

void sprite_slot_anim_step(u8 *s, const s16 *row, s32 fi)
{
    s16 *tick = (s16 *)(s + 8);
    s16 *frame = (s16 *)(s + 24);

    if (*tick >= row[fi * 2 + 2]) {
        if ((s[2] & 0x10) == 0x10) {
            *frame = *frame + 1;
            *tick = 0;
            s[2] &= ~0x20;
        } else if ((s[2] & 0x20) == 0x20) {
            *frame = *frame - 1;
            *tick = 0;
            if (*frame <= 0) {
                s[2] &= 0xDE;
            }
        } else {
            *frame = *frame + 1;
            *tick = 0;
        }
    }
    if (*frame >= row[0]) {
        if ((s[2] & 0x2) == 0x2) {
            *frame = 0;
            s[2] &= ~0x3;
        } else if ((s[2] & 0x4) == 0x4) {
            s[2] &= 0xFA;
            *frame = *frame - 1;
        } else if ((s[2] & 0x8) == 0x8) {
            *frame = 0;
            s[2] &= 0xFC;
            s[3] &= ~0x80;
        } else if ((s[2] & 0x10) == 0x10) {
            s[2] &= ~0x10;
            s[2] |= 0x20;
            if (s[3] & 0x8) {
                if (row[0] >= 3) {
                    *frame = *frame - 3;
                } else {
                    *frame = 0;
                }
            } else {
                if (row[0] >= 2) {
                    *frame = *frame - 1;
                } else {
                    *frame = 0;
                }
            }
        } else {
            *frame = 0;
        }
    }
}
