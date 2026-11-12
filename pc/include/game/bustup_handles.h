/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * bustup_handles.h -- the bust-up picture handles at their PS1 width.
 *
 * bustup_clut / bustup_img / bustup_img2 / bustup_img3 are four 32-bit
 * PS1 handles at 0x8012DD14 / DD18 / DD1C / DD20 (the globals model links
 * them at those absolute addresses, pc/ld/psx_globals.syms).  The matched
 * writers (bust_up_ptr_set, bustup_addr_set, bustup_addr_set2,
 * bustup_addr_set3) declare them `u8 *`, which is 8 bytes on LP64: each
 * store also writes the NEXT handle's word, and in the matched order
 * (clut, img3, img, img2) the last store (img2 at DD1C..DD23) zeroes
 * bustup_img3 and the img3 store zeroes D_8012DD24.  Every reader
 * (week_day_bustup_init, holiday_bustup_init, vacation_day_bustup_init,
 * ETC) declares them s32, as the console stores them.
 *
 * The overrides store the same values, in the same order, as u32.
 */
#ifndef BUSTUP_HANDLES_H
#define BUSTUP_HANDLES_H

#include "types.h"

extern u32 bustup_clut;
extern u32 bustup_img;
extern u32 bustup_img2;
extern u32 bustup_img3;

#define BUSTUP_BASE 0x801E0000u

/* row = the table entry: three u16 offsets (img3, img, img2). */
#define BUSTUP_HANDLES_SET(row) do {                  \
        bustup_img3 = BUSTUP_BASE + (u32)(row)[0];   \
        bustup_img = BUSTUP_BASE + (u32)(row)[1];    \
        bustup_img2 = BUSTUP_BASE + (u32)(row)[2];   \
    } while (0)

#endif
