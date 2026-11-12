/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_ui_font.c -- the PC layer's UI text (pc/hal/ui_font.c): UTF-8,
 * measuring, cutting with an ellipsis, wrapping and rasterising, against
 * latin12's own advances (tools/patch/font/latin12.txt: A 7, B 7, I 4,
 * space 4, '?' 6, '/' 5; ui_extra12.txt: the ellipsis 6).  The glyph
 * tables themselves are checked bit for bit against the text files by
 * pc/tools/test_ui_font.py, which also runs these tests on mutated
 * copies of ui_font.c (negative controls). */
#include <stdio.h>
#include <string.h>

#include "pc_ui.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "  %s:%d: %s\n", __FILE__, __LINE__, #c); return __LINE__; } } while (0)

#define ELL "\xE2\x80\xA6"

int test_ui_utf8_and_glyphs(void) {
    const char *s;
    const unsigned short *a, *q, *r;
    unsigned c;
    s = "A\xC3\xA9\xE2\x80\xA6\xF0\x9F\x98\x80";
    CHECK(ui_utf8_next(&s) == 'A');
    CHECK(ui_utf8_next(&s) == 0xE9);
    CHECK(ui_utf8_next(&s) == 0x2026);
    CHECK(ui_utf8_next(&s) == 0x1F600);
    CHECK(ui_utf8_next(&s) == 0 && *s == 0);
    s = "\xFF" "B";                         /* a stray byte: one replacement */
    CHECK(ui_utf8_next(&s) == 0xFFFD && ui_utf8_next(&s) == 'B');
    s = "\xC0\x80";                         /* overlong */
    CHECK(ui_utf8_next(&s) == 0xFFFD);
    s = "\xE2\x80";                         /* cut short */
    CHECK(ui_utf8_next(&s) == 0xFFFD && *s == '\x80');
    s = "\xED\xA0\x80";                     /* a surrogate */
    CHECK(ui_utf8_next(&s) == 0xFFFD);

    /* every printable ASCII character is its own glyph, not the '?' */
    ui_glyph('?', &q);
    for (c = 0x21; c < 0x7F; c++) {
        if (c == '?') continue;
        ui_glyph(c, &r);
        CHECK(r != q);
    }
    CHECK(ui_glyph('A', &a) == 7 && a[1] == 0x7800 && a[5] == 0xFC00);
    CHECK(ui_glyph(0x3042, &r) == 6 && r == q);      /* not in the font: '?' */
    CHECK(ui_glyph(0x2665, &r) == 8 && r != q);      /* a PC-only symbol */
    return 0;
}

int test_ui_text_measure(void) {
    CHECK(ui_text_width("") == 0);
    CHECK(ui_text_width("AI") == 11);
    CHECK(ui_text_width("A I") == 15);
    CHECK(ui_text_width("\xE3\x81\x82") == 6);            /* unknown: '?' */
    CHECK(ui_text_width(ELL) == 6);
    CHECK(ui_text_width_n("ABI", 2) == 14);
    CHECK(ui_text_width_n(ELL "A", 3) == 6);
    return 0;
}

int test_ui_text_fit(void) {
    char out[32];
    /* fits: unchanged */
    CHECK(ui_text_fit("ABC", 21, out, sizeof out) == 21 && !strcmp(out, "ABC"));
    /* 8 x 7 = 56 into 30: "ABC" (21) + the ellipsis (6) */
    CHECK(ui_text_fit("ABCDEFGH", 30, out, sizeof out) == 27 && !strcmp(out, "ABC" ELL));
    CHECK(ui_text_fit("ABCDEFGH", 26, out, sizeof out) == 20 && !strcmp(out, "AB" ELL));
    /* no space before the dots */
    CHECK(ui_text_fit("AB CDEFG", 30, out, sizeof out) == 20 && !strcmp(out, "AB" ELL));
    /* never inside a UTF-8 sequence */
    CHECK(ui_text_fit("A" ELL ELL ELL ELL, 20, out, sizeof out) == 19 &&
          !strcmp(out, "A" ELL ELL));
    /* too narrow for anything */
    CHECK(ui_text_fit("ABCDEFGH", 5, out, sizeof out) == 0 && !strcmp(out, ""));
    CHECK(ui_text_fit("ABCDEFGH", 6, out, sizeof out) == 6 && !strcmp(out, ELL));
    /* a small buffer still ends in a whole ellipsis */
    CHECK(ui_text_fit("ABCDEFGH", 50, out, 6) == 20 && !strcmp(out, "AB" ELL));
    return 0;
}

static int check_lines(const char *s, int maxw, const char *const *want, int nwant) {
    UiLine l[16];
    int n = ui_text_wrap(s, maxw, l, 16), i;
    if (n != nwant) return 1000 + n;
    for (i = 0; i < n; i++) {
        if ((int)strlen(want[i]) != l[i].n || strncmp(l[i].s, want[i], (size_t)l[i].n)) return 100 + i;
        if (l[i].w != ui_text_width(want[i]) || l[i].w > maxw) return 200 + i;
    }
    return 0;
}

int test_ui_text_wrap(void) {
    static const char *const two[] = { "AAA BBB", "CCC" };
    static const char *const three[] = { "AAA", "BBB", "CCC" };
    static const char *const nl[] = { "A", "", "B" };
    static const char *const path[] = { "/AA/BB/", "CC" };
    static const char *const hard[] = { "AAAA", "AAAA", "A" };
    static const char *const one[] = { "AAA BBB CCC" };
    UiLine l[200];
    char text[600];
    int maxw, n, i, k;
    /* "AAA BBB" = 6 x 7 + 4 = 46 */
    CHECK(check_lines("AAA BBB CCC", 46, two, 2) == 0);
    CHECK(check_lines("AAA BBB CCC", 45, three, 3) == 0);
    CHECK(check_lines("AAA BBB CCC", 1000, one, 1) == 0);
    CHECK(check_lines("A\n\nB", 100, nl, 3) == 0);
    /* no space: after the last path separator ("/AA/BB/" = 3 x 5 + 4 x 7) */
    CHECK(check_lines("/AA/BB/CC", 50, path, 2) == 0);
    /* no break at all: between characters (4 x 7 = 28) */
    CHECK(check_lines("AAAAAAAAA", 30, hard, 3) == 0);
    /* a column narrower than one character still advances */
    CHECK(ui_text_wrap("AB", 3, l, 64) == 2 && l[0].n == 1 && l[1].n == 1);
    CHECK(ui_text_wrap("ABCD", 100, l, 1) == 1);         /* max lines */

    /* property: every line fits, and the lines are the text minus the
     * spaces dropped at breaks */
    snprintf(text, sizeof text, "%s", "THE GAME DATA GOES TO /home/player/.local/share/tokimeki-pc "
             "BECAUSE THE GAME'S FOLDER IS READ-ONLY. PRESS R TO TRY AGAIN, OR ESC TO QUIT. "
             "Mixed case text, with punctuation: (yes) 100% sure; a-very-long-word-without-spaces.");
    for (maxw = 20; maxw < 400; maxw += 7) {
        char joined[600], squeezed[600];
        int jn = 0, sn = 0;
        n = ui_text_wrap(text, maxw, l, 200);
        CHECK(n > 0 && n < 200);
        for (i = 0; i < n; i++) {
            CHECK(l[i].w <= maxw);
            CHECK(l[i].w == ui_text_width_n(l[i].s, (size_t)l[i].n));
            for (k = 0; k < l[i].n; k++)
                if (l[i].s[k] != ' ') joined[jn++] = l[i].s[k];
        }
        for (k = 0; text[k]; k++)
            if (text[k] != ' ') squeezed[sn++] = text[k];
        CHECK(jn == sn && !memcmp(joined, squeezed, (size_t)sn));
    }
    return 0;
}

static void count_span(void *ctx, int x, int y, int w) {
    int *acc = (int *)ctx;
    acc[0]++;
    acc[1] += w;
    if (x < acc[2]) acc[2] = x;
    if (x + w > acc[3]) acc[3] = x + w;
    if (y > acc[4]) acc[4] = y;
}

int test_ui_text_spans(void) {
    int acc[5] = { 0, 0, 1000, 0, 0 };
    /* 'I': "###" on rows 1 and 9, ".#." on rows 2-8 */
    CHECK(ui_text_spans("I", count_span, acc) == 4);
    CHECK(acc[0] == 9 && acc[1] == 13 && acc[2] == 0 && acc[3] == 3 && acc[4] == 9);
    /* the second glyph starts at the first's advance */
    memset(acc, 0, sizeof acc);
    acc[2] = 1000;
    CHECK(ui_text_spans("II", count_span, acc) == 8);
    CHECK(acc[0] == 18 && acc[1] == 26 && acc[2] == 0 && acc[3] == 7);
    /* 'g' reaches the descender rows */
    memset(acc, 0, sizeof acc);
    acc[2] = 1000;
    ui_text_spans("g", count_span, acc);
    CHECK(acc[4] == 11);
    return 0;
}
