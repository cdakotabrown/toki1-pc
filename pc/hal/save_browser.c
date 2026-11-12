/* SPDX-License-Identifier: GPL-3.0-or-later */
/* save_browser.c -- the save browser: a page of the settings menu
 * (pc/include/pc_menu.h custom page) listing every save of the saves folder
 * (pc/hal/quicksave.h entries) with its thumbnail, in-game date, week and
 * stats and when it was saved; LOAD / SAVE HERE / DELETE on the chosen one,
 * NEW SAVE for an unlimited saveNNNN entry.  Also the "continue where you
 * left off" prompt the launch shows (pc/hal/save_auto.c) and the SAVE
 * OPTIONS page ([saves] in settings.ini).
 *
 * SDL-free: the presenter (pc/hal/present.c) draws the panel and gives the
 * page a canvas; the menu holds the game in the pause loop, so nothing here
 * runs game code.  Loads and saves are REQUESTS, acted on at the next
 * main-loop boundary once the menu has closed (quicksave.c); a delete and
 * the listing touch only files.  Statics are the player's session: kept
 * out of a state's restore (pc/hal/dbg_state.c KEEP_FILES).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pc_menu.h"
#include "pc_settings.h"
#include "pc_ui.h"
#include "quicksave.h"

/* Layout in canvas units (the page body, 284 x 168): the list on the left
 * (UI_ROW a row), the picture and the details on the right (latin12 text,
 * UI_ROW_TIGHT a line). */
#define MAX_ENTRIES 512
#define ROWS 14                 /* list rows shown */
#define ROW_H UI_ROW_TIGHT
#define LIST_W 134
#define ROW_TAG 48              /* a row's date / scene, after its tag */
#define PANEL_X 142
#define THUMB_W 128
#define THUMB_H 96

/* colours: the settings menu's (pc/include/pc_ui.h) */
#define C_TEXT UI_C_TEXT
#define C_DIM UI_C_DIM
#define C_HEAD UI_C_HEAD
#define C_SEL UI_C_SEL
#define C_FRAME UI_C_RULE
#define C_WARN UI_C_WARN
#define C_BOX UI_C_BOX

enum { POP_NONE, POP_ACTIONS, POP_CONFIRM, POP_MESSAGE };
enum { ACT_LOAD, ACT_SAVE, ACT_DELETE, ACT_CANCEL, ACT_YES, ACT_NO,
       ACT_CONTINUE, ACT_FRESH };

typedef struct {
    int resume;                 /* the launch prompt, not the browser */
    QsEntry e[MAX_ENTRIES];
    int n;
    int sel;                    /* browser: 0 = NEW SAVE, k = e[k - 1] */
    int top;
    int popup, psel, nacts;
    int acts[5];
    char msg[2][40];
    unsigned char thumb[QS_THUMB_W * QS_THUMB_H * 3];
    char thumb_id[QS_ID_MAX];
    long long thumb_when;           /* saved_ms of the picture read */
    int thumb_ok;
} Browser;

static Browser g_b;
static int g_page_browser = -1, g_page_resume = -1;

static const char *act_label(int a) {
    switch (a) {
    case ACT_LOAD: return "Load";
    case ACT_SAVE: return "Save here";
    case ACT_DELETE: return "Delete";
    case ACT_CANCEL: return "Cancel";
    case ACT_YES: return "Yes, delete";
    case ACT_NO: return "No";
    case ACT_CONTINUE: return "Continue";
    case ACT_FRESH: return "Start normally";
    default: return "?";
    }
}

static const QsEntry *chosen(void) {
    if (g_b.resume) return g_b.n ? &g_b.e[0] : NULL;
    return g_b.sel > 0 && g_b.sel <= g_b.n ? &g_b.e[g_b.sel - 1] : NULL;
}

static void refresh(void) {
    char keep[QS_ID_MAX] = "";
    const QsEntry *c = chosen();
    int i;
    if (c) snprintf(keep, sizeof keep, "%s", c->id);
    g_b.n = qs_entries(g_b.e, MAX_ENTRIES);
    if (g_b.resume) {
        /* the prompt shows the resume entry only */
        int k = -1;
        for (i = 0; i < g_b.n; i++)
            if (g_b.e[i].kind == QS_KIND_RESUME) k = i;
        if (k > 0) g_b.e[0] = g_b.e[k];
        g_b.n = k >= 0 ? 1 : 0;
        return;
    }
    for (i = 0; i < g_b.n; i++)
        if (keep[0] && !strcmp(g_b.e[i].id, keep)) { g_b.sel = i + 1; return; }
    if (g_b.sel > g_b.n) g_b.sel = g_b.n;
}

static void enter(void *ctx) {
    g_b.resume = ctx != NULL;
    g_b.popup = POP_NONE;
    g_b.sel = 0;
    g_b.top = 0;
    g_b.thumb_id[0] = 0;
    refresh();
    if (g_b.resume) {
        g_b.popup = POP_ACTIONS;
        g_b.psel = 0;
        g_b.nacts = 2;
        g_b.acts[0] = ACT_CONTINUE;
        g_b.acts[1] = ACT_FRESH;
    }
}

static void message(const char *a, const char *b) {
    g_b.popup = POP_MESSAGE;
    snprintf(g_b.msg[0], sizeof g_b.msg[0], "%s", a);
    snprintf(g_b.msg[1], sizeof g_b.msg[1], "%s", b ? b : "");
}

static void open_actions(const QsEntry *e) {
    g_b.popup = POP_ACTIONS;
    g_b.psel = 0;
    g_b.nacts = 0;
    if (e->has_state) g_b.acts[g_b.nacts++] = ACT_LOAD;
    if (e->kind == QS_KIND_QUICK || e->kind == QS_KIND_SAVE) g_b.acts[g_b.nacts++] = ACT_SAVE;
    g_b.acts[g_b.nacts++] = ACT_DELETE;
    g_b.acts[g_b.nacts++] = ACT_CANCEL;
}

static void act(int a) {
    const QsEntry *e = chosen();
    char id[QS_ID_MAX];
    switch (a) {
    case ACT_LOAD:
    case ACT_CONTINUE:
        if (!e) { g_b.popup = POP_NONE; return; }
        if (e->has_index && !e->same_build) {
            message("Saved by another build", "of the game: cannot load");
            return;
        }
        qs_request_load_id(e->id);
        pc_menu_close();
        return;
    case ACT_SAVE:
        if (!e) return;
        qs_request_save_id(e->id);
        pc_menu_close();
        return;
    case ACT_DELETE:
        g_b.popup = POP_CONFIRM;
        g_b.psel = 0;
        g_b.nacts = 2;
        g_b.acts[0] = ACT_NO;
        g_b.acts[1] = ACT_YES;
        return;
    case ACT_YES:
        if (e) {
            snprintf(id, sizeof id, "%s", e->id);
            qs_delete_id(id);
            qs_notify("Save deleted");
        }
        g_b.popup = POP_NONE;
        refresh();
        return;
    case ACT_FRESH:
        pc_menu_close();
        return;
    default:
        g_b.popup = POP_NONE;
        return;
    }
}

static int input(void *ctx, int in) {
    int rows = g_b.n + 1;
    (void)ctx;
    if (g_b.popup == POP_MESSAGE) {
        if (in == PC_MENU_OK || in == PC_MENU_BACK) g_b.popup = g_b.resume ? POP_ACTIONS : POP_NONE;
        return 1;
    }
    if (g_b.popup == POP_ACTIONS || g_b.popup == POP_CONFIRM) {
        switch (in) {
        case PC_MENU_UP: g_b.psel = (g_b.psel + g_b.nacts - 1) % g_b.nacts; return 1;
        case PC_MENU_DOWN: g_b.psel = (g_b.psel + 1) % g_b.nacts; return 1;
        case PC_MENU_OK: act(g_b.acts[g_b.psel]); return 1;
        case PC_MENU_BACK:
            if (g_b.resume) return 0;             /* closes: start normally */
            g_b.popup = g_b.popup == POP_CONFIRM ? POP_ACTIONS : POP_NONE;
            if (g_b.popup == POP_ACTIONS && chosen()) open_actions(chosen());
            return 1;
        default: return 1;
        }
    }
    switch (in) {
    case PC_MENU_UP: g_b.sel = (g_b.sel + rows - 1) % rows; return 1;
    case PC_MENU_DOWN: g_b.sel = (g_b.sel + 1) % rows; return 1;
    case PC_MENU_LEFT:
        g_b.sel = g_b.sel > ROWS ? g_b.sel - ROWS : 0;
        return 1;
    case PC_MENU_RIGHT:
        g_b.sel = g_b.sel + ROWS < rows ? g_b.sel + ROWS : rows - 1;
        return 1;
    case PC_MENU_OK:
        if (g_b.sel == 0) {
            char id[QS_ID_MAX];
            if (!qs_new_save_id(id, sizeof id)) { message("No free save number", "Delete some saves"); return 1; }
            qs_request_save_id(id);
            pc_menu_close();
        } else if (chosen()) {
            open_actions(chosen());
        }
        return 1;
    default:
        return 0;
    }
}

/* ---- drawing ---- */

static const char *const k_stat_short[9] = {
    "STA", "HUM", "SCI", "ART", "SPT", "TRV", "LKS", "GUT", "STR",
};

/* A list row: its tag ("SLOT 3", "AUTO 2", ...) and what it holds. */
static void row_text(const QsEntry *e, char *tag, size_t tcap, char *out, size_t cap) {
    switch (e->kind) {
    case QS_KIND_QUICK: snprintf(tag, tcap, "Slot %d", e->number); break;
    case QS_KIND_UNDO: snprintf(tag, tcap, "Undo"); break;
    case QS_KIND_SAVE: snprintf(tag, tcap, "Save %d", e->number); break;
    case QS_KIND_AUTO: snprintf(tag, tcap, "Auto %d", e->number); break;
    case QS_KIND_ENDING: snprintf(tag, tcap, "Ending"); break;
    case QS_KIND_RESUME: snprintf(tag, tcap, "Resume"); break;
    default: snprintf(tag, tcap, "?"); break;
    }
    if (e->in_progress)
        snprintf(out, cap, "%02d/%02d/%02d %s", e->year % 100, e->month, e->day, e->weekday);
    else if (e->has_index)
        snprintf(out, cap, "Scene %s", e->scene);
    else
        snprintf(out, cap, "(no index)");
}

/* text cut to w units (an ellipsis where it was cut) */
static void ftext(const PcMenuCanvas *c, int x, int y, unsigned col, int w, const char *s) {
    char buf[96];
    ui_text_fit(s, w, buf, sizeof buf);
    c->text(c->ctx, x, y, col, buf);
}

static void details(const PcMenuCanvas *c, const QsEntry *e, int x, int y) {
    char line[48], name[32];
    int i, w = c->w - x;
    qs_id_name(e->id, name, sizeof name);
    if (e->kind == QS_KIND_UNDO) snprintf(name, sizeof name, "Undo (before last load)");
    if (e->label[0]) snprintf(name, sizeof name, "%.30s", e->label);
    ftext(c, x, y, C_HEAD, w, name);
    y += ROW_H;
    if (e->in_progress) snprintf(line, sizeof line, "%d/%02d/%02d %s  week %d", e->year, e->month,
                                 e->day, e->weekday, e->week);
    else if (e->has_index) snprintf(line, sizeof line, "Not in a game (%s)", e->scene);
    else snprintf(line, sizeof line, "No index file");
    ftext(c, x, y, C_TEXT, w, line);
    y += ROW_H;
    if (e->saved[0]) snprintf(line, sizeof line, "Saved %.16s", e->saved);
    else snprintf(line, sizeof line, "Saved -");
    ftext(c, x, y, C_DIM, w, line);
    y += ROW_H;
    if (e->has_index && !e->same_build) {
        c->text(c->ctx, x, y, C_WARN, "Another build of the");
        c->text(c->ctx, x, y + ROW_H, C_WARN, "game: cannot load");
        return;
    }
    if (!e->in_progress) return;
    /* the nine parameters, three columns of name and right-aligned value */
    for (i = 0; i < 9; i++) {
        int cx = x + (i % 3) * 47, cy = y + (i / 3) * ROW_H;
        snprintf(line, sizeof line, "%d", e->stats[i]);
        c->text(c->ctx, cx, cy, C_DIM, k_stat_short[i]);
        c->text(c->ctx, cx + 45 - ui_text_width(line), cy, C_TEXT, line);
    }
}

static void thumbnail(const PcMenuCanvas *c, const QsEntry *e, int x, int y) {
    c->fill(c->ctx, x - 1, y - 1, THUMB_W + 2, THUMB_H + 2, C_FRAME);
    c->fill(c->ctx, x, y, THUMB_W, THUMB_H, 0x000000FFu);
    if (!e) return;
    if (strcmp(g_b.thumb_id, e->id) || g_b.thumb_when != e->saved_ms) {
        snprintf(g_b.thumb_id, sizeof g_b.thumb_id, "%s", e->id);
        g_b.thumb_when = e->saved_ms;
        g_b.thumb_ok = qs_thumb_read(e->id, g_b.thumb);
    }
    if (g_b.thumb_ok) {
        unsigned long key = 5381;
        const char *p;
        for (p = e->id; *p; p++) key = key * 33 + (unsigned char)*p;
        key ^= (unsigned long)e->saved_ms * 2654435761ul;
        c->image(c->ctx, x, y, THUMB_W, THUMB_H, g_b.thumb, QS_THUMB_W, QS_THUMB_H, key);
    } else {
        c->text(c->ctx, x + (THUMB_W - ui_text_width("No picture")) / 2, y + (THUMB_H - UI_LINE) / 2,
                C_DIM, "No picture");
    }
}

/* A popup: one copper unit round a dark body, sized to its text. */
static void box(const PcMenuCanvas *c, int x, int y, int w, int h) {
    c->fill(c->ctx, x - 2, y - 2, w + 4, h + 4, 0x300000FFu);
    c->fill(c->ctx, x - 1, y - 1, w + 2, h + 2, C_FRAME);
    c->fill(c->ctx, x, y, w, h, C_BOX);
}

static void popup(const PcMenuCanvas *c) {
    int i, w = 0, h, x = 10, y;
    if (g_b.popup == POP_NONE) return;
    if (g_b.popup == POP_MESSAGE) {
        for (i = 0; i < 2; i++)
            if (ui_text_width(g_b.msg[i]) > w) w = ui_text_width(g_b.msg[i]);
        w += 14;
        if (w > c->w - 2 * x) w = c->w - 2 * x;
        h = 3 * ROW_H + 10;
        y = 50;
        box(c, x, y, w, h);
        ftext(c, x + 7, y + 4, C_WARN, w - 14, g_b.msg[0]);
        ftext(c, x + 7, y + 4 + ROW_H, C_WARN, w - 14, g_b.msg[1]);
        c->text(c->ctx, x + 7, y + 6 + 2 * ROW_H, C_DIM, "Enter OK");
        return;
    }
    for (i = 0; i < g_b.nacts; i++)
        if (ui_text_width(act_label(g_b.acts[i])) > w) w = ui_text_width(act_label(g_b.acts[i]));
    if (g_b.popup == POP_CONFIRM && ui_text_width("Delete this save?") > w)
        w = ui_text_width("Delete this save?");
    w += 26;
    h = g_b.nacts * ROW_H + 8 + (g_b.popup == POP_CONFIRM ? ROW_H + 2 : 0);
    y = g_b.resume ? 70 : 40;
    box(c, x, y, w, h);
    if (g_b.popup == POP_CONFIRM) {
        c->text(c->ctx, x + 7, y + 4, C_WARN, "Delete this save?");
        y += ROW_H + 2;
    }
    for (i = 0; i < g_b.nacts; i++) {
        int ry = y + 4 + i * ROW_H;
        if (i == g_b.psel) {
            c->fill(c->ctx, x + 2, ry, w - 4, ROW_H, C_SEL);
            c->text(c->ctx, x + 4, ry, UI_C_HEART, "\xE2\x99\xA5");
        }
        c->text(c->ctx, x + 14, ry, C_TEXT, act_label(g_b.acts[i]));
    }
}

static void draw(void *ctx, const PcMenuCanvas *c) {
    const QsEntry *e = chosen();
    int i, rows = g_b.n + 1;
    char line[48], tag[16];
    (void)ctx;
    if (g_b.resume) {
        c->text(c->ctx, 4, 4, C_HEAD, "Continue where you");
        c->text(c->ctx, 4, 4 + ROW_H, C_HEAD, "left off?");
        c->text(c->ctx, 4, 8 + 2 * ROW_H, C_DIM, "The game as it was when");
        c->text(c->ctx, 4, 8 + 3 * ROW_H, C_DIM, "you last quit.");
        thumbnail(c, e, PANEL_X + 1, 1);
        if (e) details(c, e, PANEL_X, THUMB_H + 5);
        popup(c);
        return;
    }
    /* keep the selection in view */
    if (g_b.sel < g_b.top) g_b.top = g_b.sel;
    if (g_b.sel >= g_b.top + ROWS) g_b.top = g_b.sel - ROWS + 1;
    if (g_b.top > rows - ROWS) g_b.top = rows - ROWS;
    if (g_b.top < 0) g_b.top = 0;
    for (i = g_b.top; i < rows && i < g_b.top + ROWS; i++) {
        int y = (i - g_b.top) * UI_ROW;
        unsigned col = C_TEXT;
        if (i == g_b.sel) c->fill(c->ctx, 0, y, LIST_W, UI_ROW, C_SEL);
        if (i == 0) {
            c->text(c->ctx, 3, y, C_HEAD, "+ New save");
            continue;
        }
        row_text(&g_b.e[i - 1], tag, sizeof tag, line, sizeof line);
        if (g_b.e[i - 1].has_index && !g_b.e[i - 1].same_build) col = C_DIM;
        ftext(c, 3, y, col, ROW_TAG - 5, tag);
        ftext(c, ROW_TAG, y, i == g_b.sel ? col : C_DIM, LIST_W - ROW_TAG - 2, line);
    }
    if (rows > ROWS) {
        /* a scroll bar beside the list */
        int bh = c->h * ROWS / rows, by = c->h * g_b.top / rows;
        c->fill(c->ctx, LIST_W + 2, 0, 2, c->h, UI_C_TROUGH);
        c->fill(c->ctx, LIST_W + 2, by, 2, bh > 4 ? bh : 4, C_FRAME);
    }
    if (!g_b.n) c->text(c->ctx, 3, UI_ROW + 6, C_DIM, "No saves yet");
    if (e) {
        thumbnail(c, e, PANEL_X + 1, 1);
        details(c, e, PANEL_X, THUMB_H + 5);
    } else {
        c->text(c->ctx, PANEL_X, 2, C_HEAD, "New save");
        c->text(c->ctx, PANEL_X, 4 + 2 * ROW_H, C_TEXT, "Save the game as it");
        c->text(c->ctx, PANEL_X, 4 + 3 * ROW_H, C_TEXT, "is now, as a new entry.");
        c->text(c->ctx, PANEL_X, 4 + 5 * ROW_H, C_DIM, "F5 / F9: quick slots");
        c->text(c->ctx, PANEL_X, 4 + 6 * ROW_H, C_DIM, "1-9 ([ and ] select).");
        c->text(c->ctx, PANEL_X, 4 + 8 * ROW_H, C_DIM, "Memory card saves are");
        c->text(c->ctx, PANEL_X, 4 + 9 * ROW_H, C_DIM, "made in the game itself.");
    }
    popup(c);
}

static const char *help(void *ctx, int line) {
    static char buf[48];
    (void)ctx;
    if (g_b.resume)
        return line ? "Esc Start normally" : "\xE2\x86\x91\xE2\x86\x93 Choose   Enter OK";
    if (g_b.popup != POP_NONE) return line ? "Esc Back" : "\xE2\x86\x91\xE2\x86\x93 Choose   Enter OK";
    if (!line) return "\xE2\x86\x91\xE2\x86\x93 Select   \xE2\x86\x90\xE2\x86\x92 Page";
    snprintf(buf, sizeof buf, "Enter Choose   Esc Back   %d save%s", g_b.n, g_b.n == 1 ? "" : "s");
    return buf;
}

/* ---- registration ---- */

static int get_int(void *p) { return *(int *)p; }
static void set_int(void *p, int v) { *(int *)p = v; }
static const char *auto_note(void *p) {
    (void)p;
    return pc_settings_get()->harness ? "Off: automated run" : "";
}

static void add(int page, int type, const char *label, const char *key, int *field,
                int lo, int hi) {
    PcMenuItem it;
    memset(&it, 0, sizeof it);
    it.page = page;
    it.type = type;
    it.label = label;
    it.key = key;
    it.ctx = field;
    it.min = lo;
    it.max = hi;
    it.step = 1;
    it.get = get_int;
    it.set = set_int;
    it.note = auto_note;
    pc_menu_add(&it);
}

void save_browser_register(void) {
    PcMenuCustom ops;
    PcMenuItem it;
    PcSettings *s = pc_settings_mut();
    int opts;
    memset(&ops, 0, sizeof ops);
    ops.enter = enter;
    ops.input = input;
    ops.draw = draw;
    ops.help = help;
    ops.ctx = NULL;
    g_page_browser = pc_menu_custom_page("Saves", &ops);
    ops.ctx = &g_b;                     /* non-NULL: the resume prompt */
    g_page_resume = pc_menu_custom_page("Welcome back", &ops);
    opts = pc_menu_page("Save options");

    memset(&it, 0, sizeof it);
    it.type = PC_MENU_HEADER;
    it.label = "Saves";
    pc_menu_add(&it);
    memset(&it, 0, sizeof it);
    it.type = PC_MENU_PAGE;
    it.label = "Browse saves";
    it.target = g_page_browser;
    pc_menu_add(&it);
    memset(&it, 0, sizeof it);
    it.type = PC_MENU_PAGE;
    it.label = "Save options";
    it.target = opts;
    pc_menu_add(&it);

    add(opts, PC_MENU_TOGGLE, "Autosave", "saves.autosave", &s->autosave, 0, 1);
    add(opts, PC_MENU_SLIDER, "Autosaves kept", "saves.autosave_slots", &s->autosave_slots, 1, 9);
    add(opts, PC_MENU_TOGGLE, "Autosave at ending", "saves.autosave_ending", &s->autosave_ending, 0, 1);
    add(opts, PC_MENU_TOGGLE, "Resume after quit", "saves.resume", &s->resume, 0, 1);
}

void save_browser_resume_prompt(void) {
    if (g_page_resume > 0) pc_menu_open_page(g_page_resume);
}

int save_browser_page(void) { return g_page_browser; }
