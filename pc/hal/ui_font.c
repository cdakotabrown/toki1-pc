/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_font.c -- the PC layer's UI font: lookup, measuring, cutting,
 * wrapping and rasterising text (pc/include/pc_ui.h).  SDL-free; the
 * glyph tables are generated at build time from tools/patch/font/latin12.txt
 * and pc/font/ui_extra12.txt (pc/tools/gen_ui_font.py -> ui_font_data.c). */
#include "pc_ui.h"

#include <stdio.h>
#include <string.h>

extern const int ui_font_count;
extern const unsigned int ui_font_cp[];
extern const unsigned char ui_font_adv[];
extern const unsigned short ui_font_rows[][12];

unsigned ui_utf8_next(const char **sp) {
    const unsigned char *s = (const unsigned char *)*sp;
    unsigned c = s[0], cp;
    int n, i;
    if (!c) return 0;
    if (c < 0x80) { *sp += 1; return c; }
    if (c >= 0xC2 && c <= 0xDF) { n = 1; cp = c & 0x1F; }
    else if (c >= 0xE0 && c <= 0xEF) { n = 2; cp = c & 0x0F; }
    else if (c >= 0xF0 && c <= 0xF4) { n = 3; cp = c & 0x07; }
    else { *sp += 1; return 0xFFFD; }
    for (i = 1; i <= n; i++) {
        if ((s[i] & 0xC0) != 0x80) { *sp += 1; return 0xFFFD; }
        cp = cp << 6 | (s[i] & 0x3F);
    }
    if ((n == 2 && cp < 0x800) || (n == 3 && (cp < 0x10000 || cp > 0x10FFFF)) ||
        (cp >= 0xD800 && cp <= 0xDFFF)) {
        *sp += 1;
        return 0xFFFD;
    }
    *sp += n + 1;
    return cp;
}

static int find(unsigned cp) {
    int lo = 0, hi = ui_font_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (ui_font_cp[mid] == cp) return mid;
        if (ui_font_cp[mid] < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

int ui_glyph(unsigned cp, const unsigned short **rows) {
    int i = find(cp);
    if (i < 0) i = find('?');
    if (rows) *rows = ui_font_rows[i];
    return ui_font_adv[i];
}

int ui_text_width_n(const char *s, size_t bytes) {
    const char *end = s + bytes, *p = s;
    int w = 0;
    unsigned cp;
    while (p < end && (cp = ui_utf8_next(&p)) != 0) w += ui_glyph(cp, NULL);
    return w;
}

int ui_text_width(const char *s) { return ui_text_width_n(s, strlen(s)); }

#define ELLIPSIS "\xE2\x80\xA6"         /* U+2026 */

int ui_text_fit(const char *s, int maxw, char *out, size_t cap) {
    const char *p = s, *keep = s;
    int w = 0, ell = ui_glyph(0x2026, NULL);
    size_t n;
    unsigned cp;
    if (!cap) return 0;
    if (ui_text_width(s) <= maxw) {
        snprintf(out, cap, "%s", s);
        return ui_text_width(out);
    }
    /* the longest prefix that leaves room for the ellipsis */
    while ((cp = ui_utf8_next(&p)) != 0) {
        int a = ui_glyph(cp, NULL);
        if (w + a + ell > maxw) break;
        w += a;
        keep = p;
    }
    n = (size_t)(keep - s);
    while (n > 0 && s[n - 1] == ' ') n--;          /* no space before the dots */
    if (ell > maxw) n = 0;
    if (n + sizeof(ELLIPSIS) > cap) n = cap > sizeof(ELLIPSIS) ? cap - sizeof(ELLIPSIS) : 0;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;   /* a whole sequence */
    memcpy(out, s, n);
    out[n] = 0;
    if (ell <= maxw && n + sizeof(ELLIPSIS) <= cap) strcat(out, ELLIPSIS);
    return ui_text_width(out);
}

int ui_text_wrap(const char *s, int maxw, UiLine *lines, int max) {
    int n = 0;
    while (*s && n < max) {
        const char *p = s, *space = NULL, *sep = NULL;
        int w = 0;
        for (;;) {
            const char *at = p;
            unsigned cp = ui_utf8_next(&p);
            int a;
            if (cp == 0 || cp == '\n') { p = at; break; }
            a = ui_glyph(cp, NULL);
            if (cp == ' ') space = at;
            if (w + a > maxw && at > s) {
                /* break: at the last space, else after the last separator,
                 * else before this character */
                if (space && space > s) p = space;
                else if (sep) p = sep;
                else p = at;
                break;
            }
            w += a;
            if (cp == '/' || cp == '\\') sep = p;
        }
        {
            const char *e = p;
            while (e > s && e[-1] == ' ') e--;
            lines[n].s = s;
            lines[n].n = (int)(e - s);
            lines[n].w = ui_text_width_n(s, (size_t)(e - s));
            n++;
        }
        s = p;
        if (*s == '\n') s++;
        else while (*s == ' ') s++;
    }
    return n;
}

int ui_text_spans(const char *s, UiSpanFn fn, void *ctx) {
    int x = 0;
    unsigned cp;
    while ((cp = ui_utf8_next(&s)) != 0) {
        const unsigned short *rows;
        int adv = ui_glyph(cp, &rows), r;
        for (r = 0; r < UI_LINE; r++) {
            unsigned bits = rows[r];
            int c = 0;
            while (bits & 0xFFFFu) {
                int start, len = 0;
                while (!(bits & 0x8000u)) { bits = (bits << 1) & 0xFFFFu; c++; }
                start = c;
                while (bits & 0x8000u) { bits = (bits << 1) & 0xFFFFu; c++; len++; }
                fn(ctx, x + start, r, len);
            }
        }
        x += adv;
    }
    return x;
}
