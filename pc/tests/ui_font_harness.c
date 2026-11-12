/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_font_harness.c -- pc/tools/test_ui_font.py's view of the compiled UI
 * font (pc/hal/ui_font.c + the generated ui_font_data.c):
 *   dump       every glyph as "U+XXXX ADV R0 R1 ... R11" (hex rows), read
 *              back through ui_glyph(), so the tables are checked as the
 *              program sees them
 *   width      each stdin line's ui_text_width()
 *   selftest   the unit tests of pc/tests/test_ui_font.c; exit 1 on a failure
 */
#include <stdio.h>
#include <string.h>

#include "pc_ui.h"

extern const int ui_font_count;
extern const unsigned int ui_font_cp[];

extern int test_ui_utf8_and_glyphs(void);
extern int test_ui_text_measure(void);
extern int test_ui_text_fit(void);
extern int test_ui_text_wrap(void);
extern int test_ui_text_spans(void);

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "";
    if (!strcmp(mode, "dump")) {
        int i, r;
        for (i = 0; i < ui_font_count; i++) {
            const unsigned short *rows;
            int adv = ui_glyph(ui_font_cp[i], &rows);
            printf("U+%04X %d", ui_font_cp[i], adv);
            for (r = 0; r < UI_LINE; r++) printf(" %04X", rows[r]);
            printf("\n");
        }
        return 0;
    }
    if (!strcmp(mode, "width")) {
        char line[1024];
        while (fgets(line, sizeof line, stdin)) {
            line[strcspn(line, "\n")] = 0;
            printf("%d\n", ui_text_width(line));
        }
        return 0;
    }
    if (!strcmp(mode, "selftest")) {
        int bad = 0;
#define T(f) do { int r = f(); printf("%s %s\n", #f, r ? "FAIL" : "PASS"); bad |= r != 0; } while (0)
        T(test_ui_utf8_and_glyphs);
        T(test_ui_text_measure);
        T(test_ui_text_fit);
        T(test_ui_text_wrap);
        T(test_ui_text_spans);
        return bad;
    }
    fprintf(stderr, "usage: %s dump|width|selftest\n", argv[0]);
    return 2;
}
