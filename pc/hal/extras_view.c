/* SPDX-License-Identifier: GPL-3.0-or-later */
/* extras_view.c -- the extras' pages in the settings menu (Ctrl+O):
 * ENDINGS, GALLERY, ACHIEVEMENTS (pc/include/pc_menu.h custom pages; the
 * record is pc/hal/extras.c's).  SDL-free: the presenter draws the panel
 * and hands each page a canvas; the menu holds the game in its pause loop,
 * so nothing here runs game code -- the pages only read the record and
 * the gallery's PNG files.  The record is shown under either profile;
 * only the tracking needs the enhanced one (TOKI_EXTRAS).
 *
 * Statics are the player's session (dbg_state.c KEEP_FILES). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "extras.h"
#include "pc_menu.h"
#include "pc_settings.h"
#include "pc_ui.h"

/* Layout in canvas units (the page body, 284 x 168): a list on the left
 * (UI_ROW a row) and the selected entry on the right of a copper line
 * (latin12, UI_ROW_TIGHT a line).  Colours: the settings menu's. */
#define ROW_H UI_ROW_TIGHT
#define LIST_W 140
#define INFO_X 149
#define C_TEXT UI_C_TEXT
#define C_DIM UI_C_DIM
#define C_HEAD UI_C_HEAD
#define C_SEL UI_C_SEL
#define C_FRAME UI_C_RULE
#define C_GOOD UI_C_GOOD
#define C_LOCK UI_C_LOCK

static int g_page_endings = -1, g_page_gallery = -1, g_page_ach = -1;

/* text cut to w units (an ellipsis where it was cut) */
static void ftext(const PcMenuCanvas *c, int x, int y, unsigned col, int w, const char *s) {
    char buf[96];
    ui_text_fit(s, w, buf, sizeof buf);
    c->text(c->ctx, x, y, col, buf);
}

/* text word-wrapped to w units, at most max lines; returns the height used */
static int wtext(const PcMenuCanvas *c, int x, int y, unsigned col, int w, int max, const char *s) {
    UiLine l[8];
    char buf[96];
    int n = ui_text_wrap(s, w, l, max < 8 ? max : 8), i;
    for (i = 0; i < n; i++) {
        snprintf(buf, sizeof buf, "%.*s", l[i].n, l[i].s);
        c->text(c->ctx, x, y + i * ROW_H, col, buf);
    }
    return n * ROW_H;
}

/* a list row's mark: a heart when it is had, a dim dash when not */
static void mark(const PcMenuCanvas *c, int y, int had) {
    c->text(c->ctx, 2, y, had ? UI_C_HEART : C_LOCK, had ? "\xE2\x99\xA5" : "-");
}

/* "NAME VALUE" with the value right-aligned at x + w */
static void stat_cell(const PcMenuCanvas *c, int x, int y, int w, const char *name, int v) {
    char num[16];
    snprintf(num, sizeof num, "%d", v);
    c->text(c->ctx, x, y, C_DIM, name);
    c->text(c->ctx, x + w - ui_text_width(num), y, C_TEXT, num);
}

static const char *const k_stat_short[9] = {
    "STA", "HUM", "SCI", "ART", "SPT", "TRV", "LKS", "GUT", "STR",
};

static void stamp(long long t, char *out, size_t cap) {
    time_t tt = (time_t)t;
    struct tm tmv;
    if (t <= 0 || !localtime_r(&tt, &tmv)) {
        snprintf(out, cap, "-");
        return;
    }
    strftime(out, cap, "%Y-%m-%d %H:%M", &tmv);
}

static const char *off_note(void) {
    return extras_active() ? NULL : "Enhanced profile only";
}

/* ---------------------------------------------------------------- endings */

static int g_end_sel;

static void endings_enter(void *ctx) {
    (void)ctx;
    extras_store_ensure();
    g_end_sel = 0;
}

static int endings_input(void *ctx, int in) {
    (void)ctx;
    switch (in) {
    case PC_MENU_UP: g_end_sel = (g_end_sel + EX_ENDINGS - 1) % EX_ENDINGS; return 1;
    case PC_MENU_DOWN: g_end_sel = (g_end_sel + 1) % EX_ENDINGS; return 1;
    case PC_MENU_LEFT: case PC_MENU_RIGHT: case PC_MENU_OK: return 1;
    default: return 0;
    }
}

static void endings_draw(void *ctx, const PcMenuCanvas *c) {
    int e, i, x = INFO_X, y = 0, w = c->w - INFO_X;
    char line[48], t[24];
    const ExEndingRun *last = NULL;
    const char *note = off_note();
    (void)ctx;
    for (e = 0; e < EX_ENDINGS; e++) {
        int n = ex_ending_seen(e, NULL), ry = e * UI_ROW, nw = 0;
        if (e == g_end_sel) c->fill(c->ctx, 0, ry, LIST_W, UI_ROW, C_SEL);
        mark(c, ry, n);
        if (n) {
            snprintf(line, sizeof line, "%d", n);
            nw = ui_text_width(line);
            c->text(c->ctx, LIST_W - 3 - nw, ry, C_DIM, line);
        }
        ftext(c, 12, ry, n ? C_TEXT : C_DIM, LIST_W - 12 - nw - 5, ex_ending_name(e));
    }
    c->fill(c->ctx, LIST_W + 3, 0, 1, c->h, C_FRAME);
    e = g_end_sel;
    i = ex_ending_seen(e, &last);
    y += wtext(c, x, y, C_HEAD, w, 2, ex_ending_name(e)) + 2;
    if (!i) {
        c->text(c->ctx, x, y, C_DIM, "Not seen yet");
    } else {
        snprintf(line, sizeof line, "Seen %d time%s", i, i == 1 ? "" : "s");
        c->text(c->ctx, x, y, C_TEXT, line);
        y += ROW_H;
        stamp(last->unix_time, t, sizeof t);
        snprintf(line, sizeof line, "Last %s", t);
        ftext(c, x, y, C_DIM, w, line);
        y += ROW_H + 4;
        c->text(c->ctx, x, y, C_HEAD, "That playthrough");
        y += ROW_H;
        snprintf(line, sizeof line, "Girls met %d", last->met);
        c->text(c->ctx, x, y, C_TEXT, line);
        y += ROW_H;
        if (e < 13) {
            snprintf(line, sizeof line, "Heart %d  Friend %d", last->tokimeki, last->yuukou);
            ftext(c, x, y, C_TEXT, w, line);
            y += ROW_H;
            snprintf(line, sizeof line, "Bomb %d", last->bomb);
            c->text(c->ctx, x, y, C_TEXT, line);
            y += ROW_H;
        }
        y += 2;
        for (i = 0; i < 9; i++) {
            stat_cell(c, x + (i % 2) * 66, y, 44, k_stat_short[i], last->params[i]);
            if (i % 2) y += ROW_H;
        }
    }
    if (note) ftext(c, x, c->h - ROW_H, C_DIM, w, note);
}

static const char *endings_help(void *ctx, int line) {
    static char buf[48];
    (void)ctx;
    if (!line) return "\xE2\x86\x91\xE2\x86\x93 Select";
    snprintf(buf, sizeof buf, "Esc Back   %d of %d endings seen", ex_endings_distinct(), EX_ENDINGS);
    return buf;
}

/* ---------------------------------------------------------------- gallery */

#define GCOLS 3
#define GROWS 3
#define TW 64                   /* thumbnail cell, pixels = menu units */
#define TH 48
#define GRID_X 0
#define GRID_Y 15
#define GINFO_X (GRID_X + GCOLS * TW + 6)

static struct {
    int sel, top, full;
    unsigned long key;          /* the grid image's content */
    unsigned char grid[GCOLS * TW * GROWS * TH * 3];
    unsigned char big[320 * 256 * 3];
    int big_ok, big_index;
} G;

static void gallery_enter(void *ctx) {
    (void)ctx;
    extras_store_ensure();
    G.sel = 0;
    G.top = 0;
    G.full = 0;
    G.key = 0;
    G.big_index = -1;
}

static int gallery_input(void *ctx, int in) {
    int n = ex_picture_count();
    (void)ctx;
    if (G.full) {
        switch (in) {
        case PC_MENU_BACK: case PC_MENU_OK: G.full = 0; return 1;
        case PC_MENU_LEFT: case PC_MENU_UP: if (G.sel > 0) G.sel--; return 1;
        case PC_MENU_RIGHT: case PC_MENU_DOWN: if (G.sel + 1 < n) G.sel++; return 1;
        default: return 1;
        }
    }
    switch (in) {
    case PC_MENU_LEFT: if (G.sel > 0) G.sel--; return 1;
    case PC_MENU_RIGHT: if (G.sel + 1 < n) G.sel++; return 1;
    case PC_MENU_UP: if (G.sel >= GCOLS) G.sel -= GCOLS; return 1;
    case PC_MENU_DOWN: if (G.sel + GCOLS < n) G.sel += GCOLS; else if (n) G.sel = n - 1; return 1;
    case PC_MENU_OK: if (n) G.full = 1; return 1;
    default: return 0;
    }
}

/* box-average P's PNG into the cell (cx, cy) of the grid, letterboxed */
static void thumb_into(const ExPicture *p, int cx, int cy) {
    int w, h, ox, oy, x, y, c;
    unsigned char *dst;
    if (ex_picture_read(p, G.big)) return;
    w = TW;
    h = p->h * TW / p->w;
    if (h > TH) {
        h = TH;
        w = p->w * TH / p->h;
    }
    ox = cx * TW + (TW - w) / 2;
    oy = cy * TH + (TH - h) / 2;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            int sx0 = x * p->w / w, sx1 = (x + 1) * p->w / w, sy0 = y * p->h / h,
                sy1 = (y + 1) * p->h / h, sx, sy;
            unsigned sum[3] = { 0, 0, 0 }, k = 0;
            for (sy = sy0; sy < sy1; sy++)
                for (sx = sx0; sx < sx1; sx++, k++)
                    for (c = 0; c < 3; c++) sum[c] += G.big[(sy * p->w + sx) * 3 + c];
            dst = &G.grid[((oy + y) * GCOLS * TW + ox + x) * 3];
            for (c = 0; c < 3; c++) dst[c] = (unsigned char)(k ? sum[c] / k : 0);
        }
    G.big_index = -1;           /* G.big now holds a thumbnail's source */
}

static void gallery_draw(void *ctx, const PcMenuCanvas *c) {
    int n = ex_picture_count(), i, y;
    char line[64];
    const ExPicture *p;
    const char *note = off_note();
    (void)ctx;
    if (G.sel >= n) G.sel = n ? n - 1 : 0;
    if (G.full && n) {
        int w = c->w, h;
        p = ex_picture(G.sel);
        if (G.big_index != G.sel) {
            G.big_ok = ex_picture_read(p, G.big) == 0;
            G.big_index = G.sel;
        }
        h = w * p->h / p->w;
        if (h > c->h) {
            h = c->h;
            w = h * p->w / p->h;
        }
        if (G.big_ok)
            c->image(c->ctx, (c->w - w) / 2, (c->h - h) / 2, w, h, G.big, p->w, p->h,
                     0x9A11E000ul + (unsigned long)G.sel);
        else
            c->text(c->ctx, 4, 4, C_DIM, "The picture file is missing");
        return;
    }
    snprintf(line, sizeof line, "Pictures %d/%d   Event scenes %d/%d", ex_pictures_seen(EX_PIC_STILL),
             ex_pictures_total(EX_PIC_STILL), ex_pictures_seen(EX_PIC_EVENT),
             ex_pictures_total(EX_PIC_EVENT));
    c->text(c->ctx, 0, 0, C_HEAD, line);
    if (!n) {
        y = GRID_Y + 10;
        y += wtext(c, 0, y, C_DIM, c->w, 3,
                   "Nothing yet: pictures the game shows you are kept here as you play.");
        if (note) c->text(c->ctx, 0, y + 10, C_DIM, note);
        return;
    }
    snprintf(line, sizeof line, "%d of %d", G.sel + 1, n);
    c->text(c->ctx, c->w - ui_text_width(line), 0, C_DIM, line);
    if (G.sel < G.top) G.top = G.sel - G.sel % GCOLS;
    if (G.sel >= G.top + GCOLS * GROWS) G.top = G.sel - G.sel % GCOLS - GCOLS * (GROWS - 1);
    {
        unsigned long key = 1469598103ul ^ (unsigned long)G.top * 31ul ^ (unsigned long)n * 7919ul;
        if (key != G.key) {
            memset(G.grid, 0x10, sizeof G.grid);
            for (i = 0; i < GCOLS * GROWS && G.top + i < n; i++)
                thumb_into(ex_picture(G.top + i), i % GCOLS, i / GCOLS);
            G.key = key;
        }
        /* the grid as one picture, one menu unit per pixel */
        c->image(c->ctx, GRID_X, GRID_Y, GCOLS * TW, GROWS * TH, G.grid, GCOLS * TW, GROWS * TH,
                 key);
    }
    /* the selection frame */
    i = G.sel - G.top;
    {
        int fx = GRID_X + (i % GCOLS) * TW, fy = GRID_Y + (i / GCOLS) * TH;
        c->fill(c->ctx, fx, fy, TW, 1, C_HEAD);
        c->fill(c->ctx, fx, fy + TH - 1, TW, 1, C_HEAD);
        c->fill(c->ctx, fx, fy, 1, TH, C_HEAD);
        c->fill(c->ctx, fx + TW - 1, fy, 1, TH, C_HEAD);
    }
    p = ex_picture(G.sel);
    {
        const char *base = strrchr(p->source, '/');
        char t[24];
        int x = GINFO_X, w = c->w - GINFO_X;
        y = GRID_Y;
        ftext(c, x, y, C_TEXT, w, base ? base + 1 : p->source);
        y += ROW_H;
        c->text(c->ctx, x, y, C_DIM, p->kind == EX_PIC_EVENT ? "Event" : "Picture");
        y += ROW_H;
        if (ex_girl_short(p->girl)) {
            ftext(c, x, y, C_TEXT, w, ex_girl_short(p->girl));
            y += ROW_H;
        }
        if (p->year >= 1995) {
            snprintf(line, sizeof line, "%04d-%02d-%02d", p->year, p->month, p->day);
            ftext(c, x, y, C_DIM, w, line);
            y += ROW_H;
        }
        stamp(p->unix_time, t, sizeof t);
        t[10] = 0;
        c->text(c->ctx, x, y + 6, C_DIM, "Seen");
        ftext(c, x, y + 6 + ROW_H, C_DIM, w, t);
        if (note) wtext(c, x, GRID_Y + GROWS * TH - 2 * ROW_H, C_DIM, w, 2, note);
    }
}

static const char *gallery_help(void *ctx, int line) {
    (void)ctx;
    if (G.full) return line ? "Esc/Enter Back" : "\xE2\x86\x90\xE2\x86\x92 Previous/next";
    return line ? "Esc Back" : "Arrows Select   Enter View";
}

/* ---------------------------------------------------------- achievements */

#define AROWS 14                /* list rows: the body's 168 units of UI_ROW */

static int g_ach_sel, g_ach_top;

static void ach_enter(void *ctx) {
    (void)ctx;
    extras_store_ensure();
    g_ach_sel = 0;
    g_ach_top = 0;
}

static int ach_input(void *ctx, int in) {
    int n = ex_achievement_count();
    (void)ctx;
    switch (in) {
    case PC_MENU_UP: g_ach_sel = (g_ach_sel + n - 1) % n; return 1;
    case PC_MENU_DOWN: g_ach_sel = (g_ach_sel + 1) % n; return 1;
    case PC_MENU_LEFT: g_ach_sel = g_ach_sel > AROWS ? g_ach_sel - AROWS : 0; return 1;
    case PC_MENU_RIGHT: g_ach_sel = g_ach_sel + AROWS < n ? g_ach_sel + AROWS : n - 1; return 1;
    case PC_MENU_OK: return 1;
    default: return 0;
    }
}

static void ach_draw(void *ctx, const PcMenuCanvas *c) {
    int n = ex_achievement_count(), i, x = INFO_X, y = 0, w = c->w - INFO_X;
    char t[24];
    const ExAchievement *a;
    long long when;
    const char *note = off_note();
    (void)ctx;
    if (g_ach_sel < g_ach_top) g_ach_top = g_ach_sel;
    if (g_ach_sel >= g_ach_top + AROWS) g_ach_top = g_ach_sel - AROWS + 1;
    for (i = g_ach_top; i < n && i < g_ach_top + AROWS; i++) {
        int ry = (i - g_ach_top) * UI_ROW, got = ex_achievement_time(i) != 0;
        a = ex_achievement(i);
        if (i == g_ach_sel) c->fill(c->ctx, 0, ry, LIST_W, UI_ROW, C_SEL);
        mark(c, ry, got);
        ftext(c, 12, ry, got ? C_TEXT : C_DIM, LIST_W - 14, a->name);
    }
    /* the copper line, and the scroll bar on it */
    c->fill(c->ctx, LIST_W + 3, 0, 1, c->h, C_FRAME);
    if (n > AROWS) {
        int bh = c->h * AROWS / n, by = c->h * g_ach_top / n;
        c->fill(c->ctx, LIST_W + 2, by, 3, bh, C_HEAD);
    }
    a = ex_achievement(g_ach_sel);
    when = ex_achievement_time(g_ach_sel);
    y += wtext(c, x, y, C_HEAD, w, 2, a->name) + 2;
    y += wtext(c, x, y, C_TEXT, w, 8, a->how);
    y += 4;
    if (when) {
        stamp(when, t, sizeof t);
        c->text(c->ctx, x, y, C_GOOD, "Unlocked");
        ftext(c, x, y + ROW_H, C_DIM, w, t);
    } else {
        c->text(c->ctx, x, y, C_LOCK, "Locked");
    }
    if (note) ftext(c, x, c->h - ROW_H, C_DIM, w, note);
}

static const char *ach_help(void *ctx, int line) {
    static char buf[48];
    (void)ctx;
    if (!line) return "\xE2\x86\x91\xE2\x86\x93 Select   \xE2\x86\x90\xE2\x86\x92 Page";
    snprintf(buf, sizeof buf, "Esc Back   %d of %d unlocked", ex_achievements_unlocked(),
             ex_achievement_count());
    return buf;
}

/* ---------------------------------------------------------------- pages */

/* the opt-in switch ([enhanced] achievements, default off) */
static int ach_get_on(void *p) { (void)p; return pc_settings_mut()->achievements; }
static void ach_set_on(void *p, int v) { (void)p; pc_settings_mut()->achievements = v != 0; }

static void add_page_item(const char *label, int target) {
    PcMenuItem it;
    memset(&it, 0, sizeof it);
    it.type = PC_MENU_PAGE;
    it.label = label;
    it.target = target;
    if (pc_menu_add(&it) < 0) fprintf(stderr, "[EXTRAS] the settings menu is full: %s\n", label);
}

void extras_view_register(void) {
    PcMenuCustom ops;
    PcMenuItem it;
    memset(&ops, 0, sizeof ops);
    ops.enter = endings_enter;
    ops.input = endings_input;
    ops.draw = endings_draw;
    ops.help = endings_help;
    g_page_endings = pc_menu_custom_page("Endings", &ops);
    ops.enter = gallery_enter;
    ops.input = gallery_input;
    ops.draw = gallery_draw;
    ops.help = gallery_help;
    g_page_gallery = pc_menu_custom_page("Gallery", &ops);
    ops.enter = ach_enter;
    ops.input = ach_input;
    ops.draw = ach_draw;
    ops.help = ach_help;
    g_page_ach = pc_menu_custom_page("Achievements", &ops);
    if (g_page_endings < 0 || g_page_gallery < 0 || g_page_ach < 0) {
        fprintf(stderr, "[EXTRAS] the settings menu has no room for the extras' pages\n");
        return;
    }
    memset(&it, 0, sizeof it);
    it.type = PC_MENU_HEADER;
    it.label = "Extras";
    pc_menu_add(&it);
    add_page_item("Endings", g_page_endings);
    add_page_item("Gallery", g_page_gallery);
    add_page_item("Achievements", g_page_ach);
    memset(&it, 0, sizeof it);
    it.type = PC_MENU_TOGGLE;
    it.label = "Track achievements";
    it.key = "enhanced.achievements";
    it.get = ach_get_on;
    it.set = ach_set_on;
    pc_menu_add(&it);
}

int extras_view_page(int which) {
    return which == 0 ? g_page_endings : which == 1 ? g_page_gallery : g_page_ach;
}
