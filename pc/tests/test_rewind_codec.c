/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * test_rewind_codec.c -- the rewind buffer's delta coding
 * (pc/hal/rewind_codec.c) and the read-dialogue set (pc/hal/seen_text.c).
 *
 * Codec: random images with sparse, dense and edge-of-image changes code
 * and apply back exactly in both directions (XOR is its own inverse); a
 * chain of backward deltas from the newest image walks back to every
 * older one; equal images code to one record; the bound holds for the
 * worst pattern.  Negative controls: a flipped byte in a delta, a
 * truncated delta and a delta applied to an image of another length are
 * rejected or give a different image.
 *
 * Seen set: add / has / growth past the first table, the file round trip
 * (magic, appended hashes, reload) and a foreign file refused; the page
 * hash ignores the speaker's name and positions but not the text.
 *
 * Settings: the [enhanced] rewind / skip keys and [hotkeys] rewind --
 * defaults, parsing, and out-of-range values refused.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../hal/rewind_codec.h"
#include "../hal/seen_text.h"
#include "pc_settings.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "  %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static u64w rng = 0x9E3779B97F4A7C15ull;
static u64w next(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return rng;
}

static int roundtrip(const u64w *a, const u64w *b, size_t n, size_t *len_out) {
    size_t cap = rw_delta_bound(n), len, changed = 0, i, diff = 0;
    unsigned char *d = malloc(cap);
    u64w *x = malloc(n * 8 + 8);
    CHECK(d && x);
    len = rw_delta_encode(a, b, n, d, cap, &changed);
    CHECK(len > 0 && len <= cap);
    for (i = 0; i < n; i++) diff += a[i] != b[i];
    CHECK(changed == diff);
    memcpy(x, b, n * 8);
    CHECK(rw_delta_apply(x, n, d, len) == 0);
    CHECK(memcmp(x, a, n * 8) == 0);           /* b -> a */
    CHECK(rw_delta_apply(x, n, d, len) == 0);
    CHECK(memcmp(x, b, n * 8) == 0);           /* and back */
    if (len_out) *len_out = len;
    free(d);
    free(x);
    return 0;
}

int test_rewind_codec(void) {
    enum { N = 50000 };
    u64w *a = malloc(N * 8), *b = malloc(N * 8);
    size_t len, i, k;
    CHECK(a && b);
    for (i = 0; i < N; i++) a[i] = i % 7 ? next() : 0;
    /* equal: one record (8 bytes) */
    memcpy(b, a, N * 8);
    CHECK(roundtrip(a, b, N, &len) == 0);
    CHECK(len == 8);
    /* sparse changes, the first and the last word */
    for (k = 0; k < 40; k++) b[next() % N] ^= next() | 1;
    b[0] ^= 1;
    b[N - 1] ^= 0x8000000000000000ull;
    CHECK(roundtrip(a, b, N, &len) == 0);
    CHECK(len < 43 * 2 * 16);
    /* dense: every word */
    for (i = 0; i < N; i++) b[i] = a[i] + 1;
    CHECK(roundtrip(a, b, N, &len) == 0);
    CHECK(len == 8 + N * 8);
    /* the worst record pattern: 2 equal, 1 different, repeated */
    for (i = 0; i < N; i++) b[i] = i % 3 == 2 ? a[i] ^ 4 : a[i];
    CHECK(roundtrip(a, b, N, &len) == 0);
    CHECK(len <= rw_delta_bound(N));
    /* one equal word between differing ones stays literal */
    for (i = 0; i < N; i++) b[i] = i % 2 ? a[i] ^ 4 : a[i];
    CHECK(roundtrip(a, b, N, &len) == 0);
    CHECK(len <= 8 + 8 + N * 8);
    /* tiny images */
    CHECK(roundtrip(a, b, 1, NULL) == 0);
    CHECK(roundtrip(a, a, 1, NULL) == 0);
    CHECK(roundtrip(a, b, 2, NULL) == 0);
    /* too small a buffer is refused */
    {
        unsigned char small[16];
        CHECK(rw_delta_encode(a, b, N, small, sizeof small, NULL) == 0);
    }
    free(a);
    free(b);
    return 0;
}

/* The rewind chain: keep the newest whole and older ones as backward
 * deltas; walking back reproduces every older image. */
int test_rewind_chain(void) {
    enum { N = 4096, S = 12 };
    u64w *img[S], *head = malloc(N * 8);
    unsigned char *delta[S];
    size_t len[S], i, s;
    CHECK(head);
    for (s = 0; s < S; s++) {
        img[s] = malloc(N * 8);
        CHECK(img[s]);
        if (!s) for (i = 0; i < N; i++) img[s][i] = next();
        else {
            memcpy(img[s], img[s - 1], N * 8);
            for (i = 0; i < 1 + s * 30; i++) img[s][next() % N] = next();
        }
    }
    for (s = 1; s < S; s++) {                   /* delta s-1: turns img[s] into img[s-1] */
        delta[s - 1] = malloc(rw_delta_bound(N));
        len[s - 1] = rw_delta_encode(img[s - 1], img[s], N, delta[s - 1], rw_delta_bound(N), NULL);
        CHECK(len[s - 1]);
    }
    memcpy(head, img[S - 1], N * 8);
    for (s = S - 1; s-- > 0;) {
        CHECK(rw_delta_apply(head, N, delta[s], len[s]) == 0);
        CHECK(memcmp(head, img[s], N * 8) == 0);
    }
    /* negative controls: a flipped byte of literal data gives another
     * image; a truncated delta and a wrong length are refused */
    memcpy(head, img[1], N * 8);
    delta[0][8] ^= 0x40;            /* the first literal word */
    CHECK(rw_delta_apply(head, N, delta[0], len[0]) == 0);
    CHECK(memcmp(head, img[0], N * 8) != 0);
    delta[0][8] ^= 0x40;            /* the first literal word */
    memcpy(head, img[1], N * 8);
    CHECK(rw_delta_apply(head, N, delta[0], len[0] - 8) != 0);
    CHECK(rw_delta_apply(head, N - 1, delta[0], len[0]) != 0);
    CHECK(rw_delta_apply(head, N + 64, delta[0], len[0]) != 0);
    for (s = 0; s < S; s++) free(img[s]);
    for (s = 0; s + 1 < S; s++) free(delta[s]);
    free(head);
    return 0;
}

/* [enhanced] rewind / rewind_seconds / rewind_interval_frames / skip_intro /
 * skip_seen / quick_text and [hotkeys] rewind: defaults, parsing, range checks. */
int test_assist_settings(void) {
    PcSettings s;
    int w;
    pc_settings_defaults(&s);
    CHECK(s.rewind == 1 && s.rewind_seconds == 30 && s.rewind_interval == 10);
    CHECK(s.skip_intro == 0 && s.skip_seen == 0 && s.quick_text == 1);
    CHECK(!strcmp(s.hotkey[PC_HK_REWIND], "Delete, Ctrl+Z"));
    CHECK(!strcmp(pc_hotkey_names[PC_HK_REWIND], "rewind"));
    w = pc_settings_parse(&s, "[enhanced]\nrewind = false\nrewind_seconds = 60\n"
                              "rewind_interval_frames = 5\nskip_intro = true\nskip_seen = yes\n"
                              "quick_text = false\n"
                              "[hotkeys]\nrewind = End\n", "test");
    CHECK(w == 0);
    CHECK(s.rewind == 0 && s.rewind_seconds == 60 && s.rewind_interval == 5);
    CHECK(s.skip_intro == 1 && s.skip_seen == 1 && s.quick_text == 0);
    CHECK(!strcmp(s.hotkey[PC_HK_REWIND], "End"));
    /* negative control: out of range is refused, the value kept */
    w = pc_settings_parse(&s, "[enhanced]\nrewind_seconds = 1\nrewind_interval_frames = 600\n", "test");
    CHECK(w == 2);
    CHECK(s.rewind_seconds == 60 && s.rewind_interval == 5);
    return 0;
}

static void page(BacklogPage *p, const char *speaker, const char *text, int x0) {
    int k = 0, n;
    memset(p, 0, sizeof *p);
    p->speaker = -1;
    if (speaker) {
        p->speaker = 0;
        p->seg[0].x = (s16)(x0 - 121);
        p->seg[0].first = 0;
        for (n = 0; speaker[n]; n++) p->g[k++].code = (u16)(0x8200 + (u8)speaker[n]);
        p->seg[0].count = (u16)n;
        p->nseg = 1;
    }
    p->seg[p->nseg].x = (s16)(x0 - 86);
    p->seg[p->nseg].first = (u16)k;
    for (n = 0; text[n]; n++) {
        p->g[k].x = (s16)(x0 + n * 12);
        p->g[k++].code = (u16)(0x8200 + (u8)text[n]);
    }
    p->seg[p->nseg].count = (u16)n;
    p->nseg++;
    p->nglyph = (u16)k;
}

int test_seen_text_set(void) {
    SeenSet s;
    BacklogPage *p = malloc(sizeof *p), *q = malloc(sizeof *q);
    unsigned long long i, h;
    char path[] = "/tmp/seen_test_XXXXXX";
    int fd = mkstemp(path);
    CHECK(p && q && fd >= 0);
    close(fd);
    unlink(path);
    memset(&s, 0, sizeof s);
    CHECK(!seen_set_has(&s, 5));
    for (i = 1; i <= 10000; i++) CHECK(seen_set_add(&s, i * 0x9E3779B97F4A7C15ull) == 1);
    CHECK(seen_set_add(&s, 3 * 0x9E3779B97F4A7C15ull) == 0);
    CHECK(s.n == 10000);
    for (i = 1; i <= 10000; i++) CHECK(seen_set_has(&s, i * 0x9E3779B97F4A7C15ull));
    CHECK(!seen_set_has(&s, 12345));
    seen_set_free(&s);
    /* the file: magic + appended hashes, read back */
    CHECK(seen_file_append(path, 0x1122334455667788ull) == 0);
    CHECK(seen_file_append(path, 42) == 0);
    CHECK(seen_set_load(&s, path) == 0);
    CHECK(s.n == 2 && seen_set_has(&s, 0x1122334455667788ull) && seen_set_has(&s, 42));
    seen_set_free(&s);
    {
        FILE *f = fopen(path, "rb");
        unsigned char b[24];
        CHECK(f && fread(b, 1, 24, f) == 24);
        fclose(f);
        CHECK(!memcmp(b, "TKSEEN01", 8) && b[8] == 0x88 && b[15] == 0x11 && b[16] == 42);
    }
    /* a foreign file is refused, a missing one is empty */
    {
        FILE *f = fopen(path, "wb");
        CHECK(f);
        fputs("not a seen file", f);
        fclose(f);
        CHECK(seen_set_load(&s, path) < 0);
        seen_set_free(&s);
        unlink(path);
        CHECK(seen_set_load(&s, path) == 0 && s.n == 0);
    }
    /* the page hash: the text decides, not the speaker's name or position */
    page(p, "AI", "hello there", 0);
    h = seen_text_page_hash(p);
    page(q, "BO", "hello there", 30);
    CHECK(seen_text_page_hash(q) == h);
    page(q, NULL, "hello there", 0);
    CHECK(seen_text_page_hash(q) == h);
    page(q, "AI", "hello where", 0);
    CHECK(seen_text_page_hash(q) != h);
    page(q, "AI", "hello ther", 0);
    CHECK(seen_text_page_hash(q) != h);
    /* a glyph without a code is hashed by its bitmap */
    page(p, NULL, "ab", 0);
    p->g[1].code = 0;
    p->g[1].bits[3] = 1;
    h = seen_text_page_hash(p);
    p->g[1].bits[3] = 2;
    CHECK(seen_text_page_hash(p) != h);
    free(p);
    free(q);
    return 0;
}
