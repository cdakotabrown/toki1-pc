/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tpage_cache.h -- shared body of the sprite cel texture-page cache.
 *
 * Retail search_tpage (0x80048B68, 4bpp cels, key index id >> 6) and
 * search_tpage8 (0x80048DD0, 8bpp cels, key index id >> 8) are the same
 * routine over the same two tables:
 *   state[64] at screen_w+0x1128 -- rebuilt from the keys by id 0x7FFF
 *   key[64]   at screen_w+0x1228 -- tpage_buf: 0 reserved, -1 free, or the
 *             data handle a page is locked to (load_tpage_buf_lock)
 * A miss takes the first free page and queues its 128x128 upload, so page n
 * lands at VRAM ((n & 31) * 64, n < 32 ? 0 : 128).
 *
 * The one host adaptation: a sprite slot (class_w, 68-byte stride) holds
 * its cel key table as a 32-bit PS1 handle at +52.  The matched C declares
 * it `s32 *` inside a struct, which on a 64-bit host moves it to +56 and
 * widens it to 8 bytes; here it is read as a zero-extended u32.
 *
 * Sources: src/main/matched/search_tpage.c, search_tpage8.c (byte-perfect).
 */
#ifndef TPAGE_CACHE_H
#define TPAGE_CACHE_H

#include <stdint.h>

extern void image_upload_queue_add_flags(s32, u8, u8, s32, s32, s32, s32);
extern u8 screen_w[];
extern u8 class_w[];

#define TPAGE_STATE(i) (((s32 *)(screen_w + 0x1128))[i])
#define TPAGE_KEY(i)   (((s32 *)(screen_w + 0x1228))[i])

static s32 tpage_cache_search(s16 id, s32 shift, s32 chr, u8 mode)
{
    u32 keys;
    s32 key;
    s32 i;
    s32 j;

    if (id == 0x7FFF) {
        for (i = 0; i < 64; i++) {
            if (TPAGE_KEY(i) == -1) {
                TPAGE_STATE(i) = 0x7FFF;
            } else {
                TPAGE_STATE(i) = 0x7FFE;
            }
        }
        return 0;
    }

    keys = *(u32 *)(class_w + chr * 68 + 52);
    key = *(s32 *)(uintptr_t)(keys + (u32)((s32)(s16)(id >> shift) << 2));

    for (i = 15; i >= 5; i--) {
        for (j = 3; j >= 0; j--) {
            if (TPAGE_STATE(i + j * 16) == 0x7FFE
                    && TPAGE_KEY(i + j * 16) == key) {
                return (u16)(i + j * 16);
            }
        }
    }
    for (i = 0; i < 64; i++) {
        if (TPAGE_STATE(i) == key) {
            return (u16)i;
        }
    }
    for (i = 0; i < 64; i++) {
        if (TPAGE_STATE(i) == 0x7FFF) {
            TPAGE_STATE(i) = key;
            if (i < 32) {
                image_upload_queue_add_flags(key, (u8)i, mode, 0, 0, 128, 128);
            } else {
                image_upload_queue_add_flags(key, (u8)i, mode, 0, 128, 128,
                                             128);
            }
            return (u16)i;
        }
        if (i == 63) {
            return 63;
        }
    }
    return 0;
}

#endif /* TPAGE_CACHE_H */
