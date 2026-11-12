/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pc_ui.h -- the PC layer's UI text and look.
 *
 * Every screen the PC layer draws itself -- the settings menu and its
 * pages (save browser, status, extras, licences), the backlog's frame, the
 * notes and the slot list, the first-run setup screen -- writes its Latin
 * text in latin12: the English patch's hand-authored 12-px proportional
 * pixel font (tools/patch/font/latin12.txt, made for this project to sit
 * beside the game's lettering), plus a few PC-only symbols
 * (pc/font/ui_extra12.txt: ellipsis, arrows, the choice pointers, a
 * heart).  pc/tools/gen_ui_font.py compiles both text files into the
 * binary at build time.  Japanese (the girls' names on the status page,
 * the backlog's pages) is drawn from the game's own font, read at run
 * time from the player's game (pc/hal/backlog_capture.c); none of it is
 * in the binary.
 *
 * Text is UTF-8; a code point the font lacks draws as '?'.  One unit is a
 * game pixel: the presenter scales every unit by the same whole factor as
 * the picture (nearest, no smoothing).  A glyph box is UI_LINE units high:
 * row 0 empty, rows 1-9 the cap height (UI_BASE the baseline), 10-11
 * descenders.  pc/hal/ui_font.c is SDL-free (unit-tested by
 * pc/tests/test_ui_font.c); pc/hal/ui_draw.c draws it with SDL.
 */
#ifndef PC_UI_H
#define PC_UI_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_LINE 12              /* glyph box height (units) */
#define UI_BASE 9               /* the baseline row */
#define UI_ROW 12               /* a list row */
#define UI_ROW_TIGHT 11         /* dense text: descenders touch the next row's empty top */

/* The look, after the game's own message window (sampled from its frames:
 * the dark-red / copper / cream bevel and the green body, white text with
 * a black shadow one pixel down and right as disp_string draws it).
 * Colours are 0xRRGGBBAA. */
#define UI_C_TEXT   0xF8F8F8FFu
#define UI_C_DIM    0xB4CCA8FFu     /* secondary text */
#define UI_C_HEAD   0xF8D860FFu     /* titles, headers (the game's yellow) */
#define UI_C_KEY    0xC8E8F8FFu     /* key help, counters */
#define UI_C_WARN   0xFFB060FFu
#define UI_C_BAD    0xFF7878FFu
#define UI_C_GOOD   0x88E0FFFFu     /* sky blue: stands apart from the green */
#define UI_C_LIT    0xFF5050FFu
#define UI_C_LOCK   0x7C9870FFu     /* locked, unavailable */
#define UI_C_HEART  0xF890B8FFu     /* the selection mark */
#define UI_C_RULE   0xC07030FFu     /* rules and thin frames: the copper */
#define UI_C_SEL    0x00000060u     /* the selection band: the body, darker */
#define UI_C_BOX    0x0C3C00F8u     /* a popup's body */
#define UI_C_TROUGH 0x082800FFu     /* an empty bar */
#define UI_C_BAR    0xF0D098FFu     /* a filled bar: the cream */
#define UI_C_SHADOW 0x000000FFu

/* text styles */
enum { UI_PLAIN = 0, UI_SHADOW = 1, UI_OUTLINE = 2 };

/* The next code point of UTF-8 text at *s (advancing *s), or 0 at the end;
 * a malformed byte is U+FFFD (one byte consumed). */
unsigned ui_utf8_next(const char **s);

/* A code point's advance (box width + 1); *rows (may be NULL) gets its 12
 * rows, bit 15 the leftmost pixel.  Unknown code points are '?'. */
int ui_glyph(unsigned cp, const unsigned short **rows);

/* Width in units of the text (of its first `bytes` bytes). */
int ui_text_width(const char *s);
int ui_text_width_n(const char *s, size_t bytes);

/* `s` cut to at most maxw units, ending in an ellipsis when it had to be
 * cut (never inside a UTF-8 sequence); returns the width of `out`. */
int ui_text_fit(const char *s, int maxw, char *out, size_t cap);

/* Word-wrap into lines of at most maxw units: at spaces, else after a path
 * separator ('/' or '\\'), else between characters; '\n' always breaks.
 * Each line points into `s` (n bytes, spaces at the break dropped).
 * Returns the number of lines (at most max). */
typedef struct { const char *s; int n; int w; } UiLine;
int ui_text_wrap(const char *s, int maxw, UiLine *lines, int max);

/* The text's lit pixels as horizontal runs (x, y in units from the box's
 * top-left, w units long), left to right, row by row within each glyph.
 * Returns the width. */
typedef void (*UiSpanFn)(void *ctx, int x, int y, int w);
int ui_text_spans(const char *s, UiSpanFn fn, void *ctx);

#ifdef __cplusplus
}
#endif
#endif /* PC_UI_H */
