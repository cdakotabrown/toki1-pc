/* SPDX-License-Identifier: GPL-3.0-or-later */
/* guide_view.c -- the GUIDE page of the settings menu: how the game works,
 * from the game's own code and tables (pc/hal/guide_model.c builds the
 * text, pc/hal/guide_rules.c holds the rules).  A pc_menu custom page,
 * read only, in both profiles: it is reference text, it reads PS1 RAM for
 * the tables and for the live notes and never writes it; the menu holds the
 * game thread in the presenter's pause loop while the page is open.
 *
 * Four tabs (LEFT / RIGHT): Activities, Girls (a list; OK opens a girl,
 * LEFT / RIGHT then step through the girls, BACK returns to the list),
 * Events, Rules.  UP / DOWN scroll.
 *
 * TOKI_GUIDE=0 removes the page.  TOKI_GUIDE_LOG=PATH (tests): every page
 * shown is appended to PATH as text.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "pc_menu.h"
#include "pc_ui.h"
#include "game_state_view.h"
#include "guide_model.h"

/* the game's font in RAM (pc/hal/backlog_capture.c; weak: harnesses) */
extern int backlog_font_glyph(u16 code, u8 bits[18]) __attribute__((weak));

#define TOP_Y 15
#define ROW UI_ROW_TIGHT
#define GROW 13
#define SCROLL_STEP 4

static GsvState g_s;
static GuideDoc g_doc;
static int g_page = -1;
static int g_tab;
static int g_girl = -1;         /* the girls tab: -1 list, else the girl shown */
static int g_sel;               /* the list's selection */
static int g_scroll;            /* first doc line shown */
static int g_built_tab = -1, g_built_girl = -2, g_built_w = -1;
static int g_view_rows = 12;    /* doc lines that fit (set at draw) */

int guide_enabled(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("TOKI_GUIDE");
        on = !(e && e[0] == '0');
    }
    return on;
}

int guide_page(void) { return guide_enabled() ? g_page : -1; }

/* ---- the log (tests) ---- */

static void log_doc(void) {
    static char buf[160000];
    const char *path = getenv("TOKI_GUIDE_LOG");
    FILE *f;
    if (!path || !*path) return;
    guide_doc_text(&g_doc, buf, sizeof buf);
    f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "--- page %s girl %d in_game %d\n%s", guide_tab_name(g_tab), g_girl, g_s.in_game,
            buf);
    fclose(f);
}

static void rebuild(int width) {
    if (g_built_tab == g_tab && g_built_girl == g_girl && g_built_w == width) return;
    guide_build(&g_doc, g_tab, g_girl, &g_s, width);
    g_built_tab = g_tab;
    g_built_girl = g_girl;
    g_built_w = width;
    g_scroll = 0;
    log_doc();
}

/* ---- drawing ---- */

static void sjis_cell(const PcMenuCanvas *c, int x, int y, unsigned col, const u8 bits[18]) {
    int gx, gy;
    for (gy = 0; gy < 12; gy++)
        for (gx = 0; gx < 12; gx++) {
            int bit = gy * 12 + gx, run = 1;
            if (!(bits[bit >> 3] & (0x80 >> (bit & 7)))) continue;
            while (gx + run < 12) {
                int b2 = gy * 12 + gx + run;
                if (!(bits[b2 >> 3] & (0x80 >> (b2 & 7)))) break;
                run++;
            }
            c->fill(c->ctx, x + gx, y + gy, run, 1, col);
            gx += run - 1;
        }
}

/* the name in the game's 12x12 glyphs (font in RAM), else -1 */
static int sjis(const PcMenuCanvas *c, int x, int y, unsigned col, const char *s, int max) {
    u8 bits[18];
    int n, i, pass;
    const unsigned char *p = (const unsigned char *)s;
    if (!s || !backlog_font_glyph) return -1;
    for (i = 0, n = 0; p[i] && p[i + 1] && n < max; i += 2, n++)
        if (!backlog_font_glyph((u16)(p[i] << 8 | p[i + 1]), bits)) return -1;
    if (!n) return -1;
    for (pass = 0; pass < 2; pass++)
        for (i = 0, n = 0; p[i] && p[i + 1] && n < max; i += 2, n++) {
            backlog_font_glyph((u16)(p[i] << 8 | p[i + 1]), bits);
            sjis_cell(c, x + n * 12 + (pass ? 0 : 1), y + (pass ? 0 : 1),
                      pass ? col : UI_C_SHADOW, bits);
        }
    return n * 12;
}

static void tab_bar(const PcMenuCanvas *c) {
    int i, x = 0;
    for (i = 0; i < GUIDE_TABS; i++) {
        const char *t = guide_tab_name(i);
        int w = ui_text_width(t) + 10;
        if (i == g_tab) {
            c->fill(c->ctx, x, 0, w, UI_ROW, UI_C_SEL);
            c->fill(c->ctx, x, UI_ROW, w, 1, UI_C_HEAD);
        }
        c->text(c->ctx, x + 5, 0, i == g_tab ? UI_C_HEAD : UI_C_DIM, t);
        x += w + 2;
    }
    {
        const char *r = "Read only";
        c->text(c->ctx, c->w - ui_text_width(r), 0, UI_C_DIM, r);
    }
    c->fill(c->ctx, 0, UI_ROW + 1, c->w, 1, UI_C_RULE);
}

static void draw_line(const PcMenuCanvas *c, const GuideLine *l, int y) {
    char buf[GUIDE_TEXT];
    int k, x = l->indent, limit = c->w - x;
    if (l->kind == GL_GAP) return;
    if (l->kind == GL_ROW && l->ncell) limit = l->cx[0] - 26 - x;
    ui_text_fit(l->text, limit, buf, sizeof buf);
    c->text(c->ctx, x, y, l->col, buf);
    for (k = 0; k < l->ncell; k++) {
        const char *t = l->cell[k];
        c->text(c->ctx, l->cx[k] - ui_text_width(t), y, l->ccol[k] ? l->ccol[k] : l->col, t);
    }
    if (l->kind == GL_HEAD) c->fill(c->ctx, 0, y + UI_LINE - 1, ui_text_width(buf), 1, UI_C_RULE);
}

static int line_h(const GuideLine *l) { return l->kind == GL_GAP ? ROW / 2 : ROW; }

static void draw_doc(const PcMenuCanvas *c, int top) {
    int y = top, i, rows = 0, total = g_doc.n;
    int bottom = c->h;
    if (g_scroll > total - 1) g_scroll = total > 0 ? total - 1 : 0;
    if (g_scroll < 0) g_scroll = 0;
    for (i = g_scroll; i < total; i++) {
        int h = line_h(&g_doc.l[i]);
        if (y + h > bottom) break;
        draw_line(c, &g_doc.l[i], y);
        y += h;
        rows++;
    }
    g_view_rows = rows > 1 ? rows : 1;
    /* the scroll bar */
    if (g_scroll > 0 || i < total) {
        int H = bottom - top, bh = H * g_view_rows / (total ? total : 1), by;
        if (bh < 6) bh = 6;
        by = top + (H - bh) * g_scroll / (total - g_view_rows > 0 ? total - g_view_rows : 1);
        if (by + bh > bottom) by = bottom - bh;
        c->fill(c->ctx, c->w - 2, top, 2, H, UI_C_TROUGH);
        c->fill(c->ctx, c->w - 2, by, 2, bh, UI_C_BAR);
    }
}

static void draw_list(const PcMenuCanvas *c) {
    int i, y = TOP_Y;
    c->text(c->ctx, 2, y, UI_C_DIM, "How to meet her, what she likes, her ending:");
    y += ROW + 2;
    for (i = 0; i < GUIDE_NGIRLS; i++) {
        char note[96];
        int w;
        if (y + GROW > c->h) break;
        if (i == g_sel) c->fill(c->ctx, 0, y - 1, c->w, GROW, UI_C_SEL);
        /* her name in the game's glyphs, then romanised (only the latter
         * when the game's font is not available) */
        w = sjis(c, 2, y, i == g_sel ? UI_C_HEAD : UI_C_TEXT, gsv_girl_sjis(i), 6);
        c->text(c->ctx, w < 0 ? 2 : 78, y, i == g_sel ? UI_C_HEAD : UI_C_TEXT,
                guide_girl_name(i));
        guide_girl_note(i, &g_s, note, sizeof note);
        if (note[0]) {
            char fit[96];
            ui_text_fit(note, c->w - 170, fit, sizeof fit);
            c->text(c->ctx, c->w - 2 - ui_text_width(fit), y,
                    strstr(note, "qualify") ? UI_C_GOOD : UI_C_DIM, fit);
        }
        y += GROW;
    }
}

static void draw(void *ctx, const PcMenuCanvas *c) {
    (void)ctx;
    tab_bar(c);
    if (g_tab == GUIDE_GIRLS && g_girl < 0) {
        draw_list(c);
        return;
    }
    rebuild(c->w - 4);
    draw_doc(c, TOP_Y);
}

/* ---- input ---- */

/* TOKI_GUIDE_LOG (tests): the 2 MB PS1 RAM at each opening, PATH.N.ram,
 * so the harness can check the guide against what the game holds */
static void log_ram(void) {
    static int count;
    const char *path = getenv("TOKI_GUIDE_LOG");
    char ram[600];
    FILE *f;
    if (!path || !*path) return;
    snprintf(ram, sizeof ram, "%s.%d.ram", path, count++);
    f = fopen(ram, "wb");
    if (!f) return;
    fwrite((const void *)(unsigned long)0x80000000ul, 1, 0x200000ul, f);
    fclose(f);
}

static void enter(void *ctx) {
    (void)ctx;
    memset(&g_s, 0, sizeof g_s);
    gsv_read(&g_s);             /* read only: the game is held */
    g_built_tab = -1;           /* the live notes may have changed */
    printf("[GUIDE] open (%s)\n", g_s.in_game ? "in game" : "no game");
    fflush(stdout);
    log_ram();
}

static int input(void *ctx, int in) {
    int doc = !(g_tab == GUIDE_GIRLS && g_girl < 0);
    (void)ctx;
    switch (in) {
    case PC_MENU_LEFT:
    case PC_MENU_RIGHT:
        if (g_tab == GUIDE_GIRLS && g_girl >= 0) {
            g_girl = (g_girl + (in == PC_MENU_LEFT ? GUIDE_NGIRLS - 1 : 1)) % GUIDE_NGIRLS;
            g_sel = g_girl;
        } else {
            g_tab = (g_tab + (in == PC_MENU_LEFT ? GUIDE_TABS - 1 : 1)) % GUIDE_TABS;
        }
        return 1;
    case PC_MENU_UP:
        if (doc) g_scroll = g_scroll > SCROLL_STEP ? g_scroll - SCROLL_STEP : 0;
        else g_sel = (g_sel + GUIDE_NGIRLS - 1) % GUIDE_NGIRLS;
        return 1;
    case PC_MENU_DOWN:
        if (doc) {
            if (g_scroll + g_view_rows < g_doc.n) g_scroll += SCROLL_STEP;
        } else {
            g_sel = (g_sel + 1) % GUIDE_NGIRLS;
        }
        return 1;
    case PC_MENU_OK:
        if (g_tab == GUIDE_GIRLS && g_girl < 0) g_girl = g_sel;
        else if (doc && g_scroll + g_view_rows < g_doc.n) g_scroll += g_view_rows - 1;
        return 1;
    case PC_MENU_BACK:
        if (g_tab == GUIDE_GIRLS && g_girl >= 0) {
            g_girl = -1;
            return 1;
        }
        return 0;
    default:
        return 0;
    }
}

static const char *help(void *ctx, int line) {
    (void)ctx;
    if (line) return g_tab == GUIDE_GIRLS && g_girl >= 0 ? "Esc back to the list"
                                                          : "Esc close";
    if (g_tab == GUIDE_GIRLS && g_girl < 0)
        return "\xE2\x86\x90\xE2\x86\x92 tab   \xE2\x86\x91\xE2\x86\x93 girl   Enter open";
    if (g_tab == GUIDE_GIRLS)
        return "\xE2\x86\x90\xE2\x86\x92 girl   \xE2\x86\x91\xE2\x86\x93 scroll   Enter page down";
    return "\xE2\x86\x90\xE2\x86\x92 tab   \xE2\x86\x91\xE2\x86\x93 scroll   Enter page down";
}

/* ---- registration (the presenter, once the menu exists) ---- */

void guide_menu_register(void) {
    static int done;
    PcMenuCustom ops;
    PcMenuItem it;
    if (done || !guide_enabled()) return;
    done = 1;
    memset(&ops, 0, sizeof ops);
    ops.enter = enter;
    ops.input = input;
    ops.draw = draw;
    ops.help = help;
    g_page = pc_menu_custom_page("How the game works", &ops);
    if (g_page < 0) {
        fprintf(stderr, "[GUIDE] settings menu full: no page\n");
        return;
    }
    memset(&it, 0, sizeof it);
    it.type = PC_MENU_HEADER;
    it.label = "Guide";
    pc_menu_add(&it);
    memset(&it, 0, sizeof it);
    it.type = PC_MENU_PAGE;
    it.label = "How the game works";
    it.target = g_page;
    if (pc_menu_add(&it) < 0) fprintf(stderr, "[GUIDE] settings menu full: entry not added\n");
    if (getenv("TOKI_GUIDE_LOG"))
        printf("[GUIDE] registered: page %d, menu items %d\n", g_page, pc_menu_count());
}

/* tests: open the page as the menu does, with the state read */
void guide_test_enter(int tab, int girl) {
    enter(NULL);
    g_tab = tab;
    g_girl = girl;
}
