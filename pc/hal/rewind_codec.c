/* SPDX-License-Identifier: GPL-3.0-or-later */
/* rewind_codec.c -- XOR + zero-run coding of snapshot deltas
 * (format: rewind_codec.h).  A run of fewer than MIN_ZERO equal words
 * inside differing ones stays literal, so a record never costs more than
 * the words it saves. */
#include <string.h>
#include "rewind_codec.h"

#define MIN_ZERO 2

size_t rw_delta_bound(size_t nwords) {
    /* at worst every record is MIN_ZERO zero words + 1 literal word:
     * 8 bytes of header per MIN_ZERO + 1 words, plus the literals */
    return nwords * 8 + (nwords / (MIN_ZERO + 1) + 2) * 8;
}

static unsigned char *put_u32(unsigned char *p, u32w v) {
    memcpy(p, &v, 4);
    return p + 4;
}

size_t rw_delta_encode(const u64w *a, const u64w *b, size_t nwords,
                       unsigned char *out, size_t cap, size_t *changed) {
    size_t i = 0, diff = 0;
    unsigned char *p = out, *end = out + cap;
    if (cap < rw_delta_bound(nwords)) return 0;
    while (i < nwords) {
        size_t z = i, lit, j;
        unsigned char *hdr;
        /* the equal words */
        while (z < nwords && a[z] == b[z]) z++;
        /* the differing words, with short equal runs folded in */
        lit = z;
        while (lit < nwords) {
            size_t e = lit;
            if (a[lit] != b[lit]) { lit++; continue; }
            while (e < nwords && a[e] == b[e] && e - lit < MIN_ZERO) e++;
            if (e == nwords || e - lit >= MIN_ZERO) break;
            lit = e;
        }
        if (z - i > 0xFFFFFFFFu || lit - z > 0xFFFFFFFFu) return 0;   /* > 32 GB images */
        hdr = p;
        if (hdr + 8 + (lit - z) * 8 > end) return 0;
        p = put_u32(p, (u32w)(z - i));
        p = put_u32(p, (u32w)(lit - z));
        for (j = z; j < lit; j++) {
            u64w x = a[j] ^ b[j];
            memcpy(p, &x, 8);
            p += 8;
            diff += x != 0;
        }
        i = lit;
    }
    if (changed) *changed = diff;
    return (size_t)(p - out);
}

int rw_delta_apply(u64w *dst, size_t nwords, const unsigned char *delta, size_t len) {
    size_t i = 0;
    const unsigned char *p = delta, *end = delta + len;
    while (i < nwords) {
        u32w z, lit, k;
        if (end - p < 8) return -1;
        memcpy(&z, p, 4);
        memcpy(&lit, p + 4, 4);
        p += 8;
        if (z > nwords - i || lit > nwords - i - z) return -1;
        if ((size_t)(end - p) < (size_t)lit * 8) return -1;
        i += z;
        for (k = 0; k < lit; k++, i++, p += 8) {
            u64w x;
            memcpy(&x, p, 8);
            dst[i] ^= x;
        }
    }
    return p == end ? 0 : -1;
}
