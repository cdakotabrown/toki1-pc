/* SPDX-License-Identifier: GPL-3.0-or-later */
/* controls_view.c -- the CONTROLS page: which keyboard key and which
 * controller input each PS1 pad button is bound to right now.
 *
 * A pc_menu custom page, read only, in both profiles (reference UI, like
 * the guide, pc/hal/guide_view.c).  It is reached from the settings menu
 * (GUIDE > Controls) and straight with [hotkeys] controls (F2): the menu
 * holds the game thread in the presenter's pause loop while it is open, so
 * the game sees no input and resumes exactly where it was.  The rows come
 * from the input HAL's resolved keymaps (pad_live_bindings, pc/hal/pad.c)
 * through pc/hal/controls_text.c, never from a table of defaults.
 *
 * Also the one-time hint: the first launch of an install shows the note
 * "Press F2 to see the controls" once and records it as controls_hint
 * in the [state] section of settings.ini (never in an automated run).
 *
 * TOKI_CONTROLS=0 removes the page, the hotkey and the hint.
 * TOKI_CONTROLS_LOG=PATH (tests): every legend shown is appended to PATH.
 * TOKI_CONTROLS_HINT=PATH (tests): the hint's marker file, also in an
 * automated run.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "types.h"
#include "pc_menu.h"
#include "pc_ui.h"
#include "pc_settings.h"
#include "pc_controls.h"
#include "pad_map.h"
#include "pc_fs.h"

extern void pad_live_bindings(const PcKeymap **keys, const PcKeymap **pad, int *controllers_on,
                              int *connected);
extern int guide_page(void) __attribute__((weak));
/* pc/hal/pad.c: player 1's controller family and name (PAD_FAMILY_*) */
extern int pad_controller_family(char *name, size_t n) __attribute__((weak));
extern void pc_present_note(const char *text, unsigned ms) __attribute__((weak));

#define TOP_Y 15
#define ROW UI_ROW_TIGHT
#define COL1 86
#define COL2 190
#define MAX_VLINES 160
#define SCROLL_STEP 3

static int g_page = -1;
static ControlsLegend g_legend;
static char g_notes[6][PC_SETTINGS_PATH_MAX + 64];
static int g_nnotes;
static int g_scroll;
static int g_view_rows = 12;

/* one line on screen: up to three cells (a row), or one full-width note */
typedef struct {
    UiLine cell[3];
    int has[3];
    unsigned col;
    int gap;
    int after;                  /* extra units below (the end of a row / note) */
    int span;                   /* on a row's first line: its lines (kept together) */
} VLine;
static VLine g_vl[MAX_VLINES];
static int g_nvl, g_built_w = -1;

int controls_enabled(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("TOKI_CONTROLS");
        on = !(e && e[0] == '0');
    }
    return on;
}

int controls_page(void) { return controls_enabled() ? g_page : -1; }

/* the hotkey's first key, as the settings file spells it, or NULL */
static const char *hotkey_label(int hk) {
    static char out[2][64];
    static int k;
    char tok[8][64];
    int i, n = pc_bind_tokens(pc_settings_get()->hotkey[hk], tok, 8);
    for (i = 0; i < n; i++)
        if (SDL_strcasecmp(tok[i], "none")) {
            k ^= 1;
            snprintf(out[k], sizeof out[k], "%s", tok[i]);
            return out[k];
        }
    return NULL;
}

static const char *key_name(int code) {
    const char *n = SDL_GetScancodeName((SDL_Scancode)code);
    return n && *n ? n : NULL;
}
static const char *button_name(int code) {
    return SDL_GameControllerGetStringForButton((SDL_GameControllerButton)code);
}
static const char *axis_name(int code) {
    return SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)code);
}

/* the legend and its notes, from the live bindings */
static void read_bindings(void) {
    const PcSettings *s = pc_settings_get();
    const PcKeymap *keys = NULL, *pad = NULL;
    int on = 1, connected = 0, family = PAD_FAMILY_XBOX;
    char name[96] = "";
    const char *south = " controller_layout = confirm_south: the bottom button confirms.";
    pad_live_bindings(&keys, &pad, &on, &connected);
    if (on && connected && pad_controller_family) family = pad_controller_family(name, sizeof name);
    controls_legend_build_family(&g_legend, keys, on ? pad : NULL, key_name, button_name,
                                 axis_name, family);
    if (s->controller_layout != PC_LAYOUT_CONFIRM_SOUTH) south = "";
    g_nnotes = 0;
    if (!on)
        snprintf(g_notes[g_nnotes++], sizeof g_notes[0],
                 "Controllers are off ([input] controller = false).");
    else if (!connected)
        snprintf(g_notes[g_nnotes++], sizeof g_notes[0],
                 "Controller (0 connected): Xbox names, by position. On a PlayStation "
                 "pad A = Cross, B = Circle, X = Square, Y = Triangle; on a Nintendo pad "
                 "the bottom button (B here) is B and the right one is A.%s",
                 south);
    else if (family == PAD_FAMILY_GENERIC)
        snprintf(g_notes[g_nnotes++], sizeof g_notes[0],
                 "Controller (%d connected): %s, on the default mapping for an unknown "
                 "joystick: buttons by number. Change it in gamecontrollerdb.txt (the log "
                 "has its line).%s",
                 connected, name[0] ? name : "joystick", south);
    else
        snprintf(g_notes[g_nnotes++], sizeof g_notes[0],
                 "Controller (%d connected): %s, %s names; the buttons go by position, as "
                 "on a PS1 pad.%s",
                 connected, name[0] ? name : "controller", controls_family_name(family), south);
    if (s->mouse_enabled)
        snprintf(g_notes[g_nnotes++], sizeof g_notes[0],
                 "Mouse: where the game shows its hand, it follows the pointer; left click "
                 "confirms, right click cancels (the game's own option swaps them).");
    else
        snprintf(g_notes[g_nnotes++], sizeof g_notes[0], "Mouse: off ([input] mouse = false).");
    snprintf(g_notes[g_nnotes++], sizeof g_notes[0],
             "A key pressed with Ctrl, Alt or GUI held never presses a game button.");
    if (s->file_path[0])
        snprintf(g_notes[g_nnotes++], sizeof g_notes[0],
                 "Change them in [keyboard] and [controller] of %s", s->file_path);
    else
        snprintf(g_notes[g_nnotes++], sizeof g_notes[0],
                 "Change them in [keyboard] and [controller] of settings.ini.");
    g_built_w = -1;
}

/* ---- the log (tests) ---- */

static void log_legend(void) {
    static char buf[8192];
    const char *path = getenv("TOKI_CONTROLS_LOG");
    FILE *f;
    int i;
    if (!path || !*path) return;
    controls_legend_text(&g_legend, buf, sizeof buf);
    f = pc_fs_fopen(path, "a");
    if (!f) return;
    fprintf(f, "--- controls\n%s", buf);
    for (i = 0; i < g_nnotes; i++) fprintf(f, "note: %s\n", g_notes[i]);
    fclose(f);
}

/* ---- layout ---- */

static void layout(int w) {
    int i, k;
    if (g_built_w == w) return;
    g_built_w = w;
    g_nvl = 0;
    for (i = 0; i < g_legend.n && g_nvl < MAX_VLINES; i++) {
        const ControlsRow *r = &g_legend.row[i];
        const char *cells[3];
        UiLine wl[3][6];
        int nl[3], most = 0, j;
        static const int x[4] = { 0, COL1, COL2, 0 };
        cells[0] = r->ps1;
        cells[1] = r->keys;
        cells[2] = r->pad;
        for (k = 0; k < 3; k++) {
            int cw = (k < 2 ? x[k + 1] : w) - x[k] - 6;
            nl[k] = ui_text_wrap(cells[k], cw, wl[k], 6);
            if (nl[k] > most) most = nl[k];
        }
        for (j = 0; j < most && g_nvl < MAX_VLINES; j++) {
            VLine *v = &g_vl[g_nvl++];
            memset(v, 0, sizeof *v);
            v->col = UI_C_TEXT;
            for (k = 0; k < 3; k++)
                if (j < nl[k]) {
                    v->cell[k] = wl[k][j];
                    v->has[k] = 1;
                }
            if (j == 0) v->span = most;
            if (j == most - 1) v->after = 2;
        }
    }
    if (g_nvl < MAX_VLINES) {
        memset(&g_vl[g_nvl], 0, sizeof g_vl[0]);
        g_vl[g_nvl++].gap = 1;
    }
    for (i = 0; i < g_nnotes; i++) {
        UiLine wl[8];
        int n = ui_text_wrap(g_notes[i], w - 4, wl, 8), j;
        for (j = 0; j < n && g_nvl < MAX_VLINES; j++) {
            VLine *v = &g_vl[g_nvl++];
            memset(v, 0, sizeof *v);
            v->cell[0] = wl[j];
            v->has[0] = 1;
            v->col = UI_C_DIM;
            if (j == n - 1) v->after = 3;
        }
    }
}

/* ---- drawing ---- */

static void cell_text(const PcMenuCanvas *c, int x, int y, unsigned col, const UiLine *l) {
    char buf[CONTROLS_CELL + 8];
    int n = l->n < (int)sizeof buf - 1 ? l->n : (int)sizeof buf - 1;
    if (n > 0 && l->s[0] && (unsigned char)l->s[0] < 0x20) return;
    memcpy(buf, l->s, (size_t)n);
    buf[n] = 0;
    c->text(c->ctx, x, y, col, buf);
}

static void draw(void *ctx, const PcMenuCanvas *c) {
    int y = TOP_Y, i, rows = 0, bottom = c->h;
    static const int x[3] = { 0, COL1, COL2 };
    (void)ctx;
    c->text(c->ctx, 0, 0, UI_C_HEAD, "PS1 button");
    c->text(c->ctx, COL1, 0, UI_C_HEAD, "Keyboard");
    c->text(c->ctx, COL2, 0, UI_C_HEAD, "Controller");
    c->fill(c->ctx, 0, UI_ROW + 1, c->w, 1, UI_C_RULE);
    layout(c->w - 4);
    if (g_scroll > g_nvl - 1) g_scroll = g_nvl > 0 ? g_nvl - 1 : 0;
    if (g_scroll < 0) g_scroll = 0;
    for (i = g_scroll; i < g_nvl; i++) {
        const VLine *v = &g_vl[i];
        int h = (v->gap ? ROW / 2 : ROW) + v->after, k;
        if (y + h > bottom) break;
        /* a row is shown whole or not at all (the scroll bar says more) */
        if (v->span > 1 && rows && y + v->span * ROW + 2 > bottom) break;
        for (k = 0; k < 3; k++)
            if (v->has[k])
                cell_text(c, x[k], y, k == 0 && v->col == UI_C_TEXT ? UI_C_KEY : v->col,
                          &v->cell[k]);
        y += h;
        rows++;
    }
    g_view_rows = rows > 1 ? rows : 1;
    if (g_scroll > 0 || i < g_nvl) {
        int H = bottom - TOP_Y, bh = H * g_view_rows / (g_nvl ? g_nvl : 1), by;
        if (bh < 6) bh = 6;
        by = TOP_Y + (H - bh) * g_scroll / (g_nvl - g_view_rows > 0 ? g_nvl - g_view_rows : 1);
        if (by + bh > bottom) by = bottom - bh;
        c->fill(c->ctx, c->w - 2, TOP_Y, 2, H, UI_C_TROUGH);
        c->fill(c->ctx, c->w - 2, by, 2, bh, UI_C_BAR);
    }
}

/* ---- input ---- */

static void enter(void *ctx) {
    (void)ctx;
    read_bindings();
    g_scroll = 0;
    printf("[CONTROLS] open\n");
    fflush(stdout);
    log_legend();
}

static int input(void *ctx, int in) {
    (void)ctx;
    switch (in) {
    case PC_MENU_UP:
        g_scroll = g_scroll > SCROLL_STEP ? g_scroll - SCROLL_STEP : 0;
        return 1;
    case PC_MENU_DOWN:
    case PC_MENU_OK:
        if (g_scroll + g_view_rows < g_nvl) {
            g_scroll += in == PC_MENU_OK ? g_view_rows - 1 : SCROLL_STEP;
            /* stop with the last line at the bottom */
            if (g_scroll > g_nvl - g_view_rows) g_scroll = g_nvl - g_view_rows;
        }
        return 1;
    case PC_MENU_LEFT:
    case PC_MENU_RIGHT:
        return 1;
    default:
        return 0;
    }
}

static const char *help(void *ctx, int line) {
    static char buf[96];
    const char *hk = hotkey_label(PC_HK_CONTROLS);
    (void)ctx;
    if (line) {
        if (hk) snprintf(buf, sizeof buf, "Esc or %s close", hk);
        else snprintf(buf, sizeof buf, "Esc close");
        return buf;
    }
    return "\xE2\x86\x91\xE2\x86\x93 scroll   Enter page down";
}

static const char *open_note(void *p) {
    const char *hk = hotkey_label(PC_HK_CONTROLS);
    (void)p;
    return hk ? hk : "";
}

/* ---- registration (the presenter, once the menu exists) ---- */

void controls_menu_register(void) {
    static int done;
    PcMenuCustom ops;
    PcMenuItem it;
    if (done || !controls_enabled()) return;
    done = 1;
    memset(&ops, 0, sizeof ops);
    ops.enter = enter;
    ops.input = input;
    ops.draw = draw;
    ops.help = help;
    g_page = pc_menu_custom_page("Controls", &ops);
    if (g_page < 0) {
        fprintf(stderr, "[CONTROLS] settings menu full: no page\n");
        return;
    }
    /* under the guide's header when the guide is there */
    if (!(guide_page && guide_page() > 0)) {
        memset(&it, 0, sizeof it);
        it.type = PC_MENU_HEADER;
        it.label = "Guide";
        pc_menu_add(&it);
    }
    memset(&it, 0, sizeof it);
    it.type = PC_MENU_PAGE;
    it.label = "Controls";
    it.target = g_page;
    it.note = open_note;
    if (pc_menu_add(&it) < 0) fprintf(stderr, "[CONTROLS] settings menu full: entry not added\n");
    if (getenv("TOKI_CONTROLS_LOG"))
        printf("[CONTROLS] registered: page %d, menu items %d\n", g_page, pc_menu_count());
}

/* ---- the one-time hint ---- */

void controls_first_run_hint(void) {
    const PcSettings *s = pc_settings_get();
    const char *test = getenv("TOKI_CONTROLS_HINT");
    char path[PC_SETTINGS_PATH_MAX + 16], text[128], seen[32];
    const char *hk, *menu, *key = "controls_hint", *val = "shown";
    if (!controls_enabled() || g_page < 0) return;
    /* TOKI_CONTROLS_HINT=FILE (tests): a settings-format file of its own */
    if (test && *test) snprintf(path, sizeof path, "%s", test);
    else if (!s->harness && s->file_path[0]) snprintf(path, sizeof path, "%s", s->file_path);
    else return;
    if (pc_settings_state_get(path, key, seen, sizeof seen) == 0 && seen[0]) return;
    hk = hotkey_label(PC_HK_CONTROLS);
    menu = hotkey_label(PC_HK_MENU);
    if (hk) snprintf(text, sizeof text, "Press %s to see the controls", hk);
    else if (menu) snprintf(text, sizeof text, "The controls: settings (%s) > Controls", menu);
    else snprintf(text, sizeof text, "The controls: settings menu > Controls");
    if (pc_present_note) pc_present_note(text, 6000);
    if (pc_settings_state_set(path, &key, &val, 1)) {
        fprintf(stderr, "[CONTROLS] cannot write %s: the hint will show again\n", path);
        return;
    }
    printf("[CONTROLS] first-run hint shown (%s)\n", path);
    fflush(stdout);
}
