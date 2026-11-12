/* SPDX-License-Identifier: GPL-3.0-or-later */
/* rewind_codec.h -- the rewind buffer's delta coding (pc/hal/rewind_codec.c).
 *
 * A delta is the XOR of two equally long images, run-length coded in
 * 64-bit words: records of
 *     u32 zero_words   words the two images agree on
 *     u32 lit_words    words that differ, followed by
 *     u64 xor[lit_words]
 * until the image's length is covered.  Applying a delta XORs it into an
 * image, so the same delta turns either image into the other (rewind.c
 * keeps the newest snapshot whole and every older one as the delta to the
 * next newer).  Pure C, no allocation: pc/tests/test_rewind_codec.c.
 */
#ifndef PC_HAL_REWIND_CODEC_H
#define PC_HAL_REWIND_CODEC_H

#include <stddef.h>
typedef unsigned long long u64w;   /* a 64-bit word (no <stdint.h>: the PS1 include/ shadows it) */
typedef unsigned int u32w;

/* The largest delta of NWORDS words (every word differing, in the worst
 * record pattern). */
size_t rw_delta_bound(size_t nwords);

/* Code A xor B (NWORDS words each) into OUT (capacity CAP bytes, at least
 * rw_delta_bound(nwords)).  Returns the delta's length in bytes, 0 when
 * CAP is too small.  *CHANGED (if not NULL) gets the differing words. */
size_t rw_delta_encode(const u64w *a, const u64w *b, size_t nwords,
                       unsigned char *out, size_t cap, size_t *changed);

/* DST ^= the delta DELTA (LEN bytes) of an image of NWORDS words.
 * Returns 0, or -1 when the delta is malformed or covers another length
 * (DST may then be partly changed). */
int rw_delta_apply(u64w *dst, size_t nwords, const unsigned char *delta, size_t len);

#endif
