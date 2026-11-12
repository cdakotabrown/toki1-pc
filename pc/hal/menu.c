/* SPDX-License-Identifier: GPL-3.0-or-later */
/* menu.c -- the in-game settings menu's model (see pc/include/pc_menu.h).
 *
 * SDL-free: pages, items, the selection, value text and the changed-key
 * list.  The presenter (pc/hal/present.c) registers the built-in entries,
 * turns keys / controller buttons into pc_menu_input() calls, draws
 * pc_menu_view() over the finished frame and saves on close.
 */
#include "pc_menu.h"

#include <stdio.h>
#include <string.h>

typedef struct { const char *title; int custom; PcMenuCustom ops; } Page;

static PcMenuItem g_items[PC_MENU_MAX_ITEMS];
static int g_nitems;
static Page g_pages[PC_MENU_MAX_PAGES] = { { "Settings", 0, { 0 } } };
static int g_npages = 1;

static int g_open;
static int g_page;                       /* the page shown */
static int g_sel = -1;                   /* item index (g_items), or -1 */
static int g_stack[PC_MENU_MAX_PAGES];   /* pages to return to, and their selection */
static int g_stack_sel[PC_MENU_MAX_PAGES];
static int g_depth;

static const char *g_dirty[PC_MENU_MAX_ITEMS];
static int g_ndirty;
static PcMenuSaveFn g_save;

int pc_menu_page(const char *title) {
    if (g_npages >= PC_MENU_MAX_PAGES) return -1;
    memset(&g_pages[g_npages], 0, sizeof g_pages[g_npages]);
    g_pages[g_npages].title = title;
    return g_npages++;
}

int pc_menu_custom_page(const char *title, const PcMenuCustom *ops) {
    int id = pc_menu_page(title);
    if (id < 0 || !ops) return id;
    g_pages[id].custom = 1;
    g_pages[id].ops = *ops;
    return id;
}

const PcMenuCustom *pc_menu_custom(void) {
    return g_open && g_pages[g_page].custom ? &g_pages[g_page].ops : NULL;
}

int pc_menu_add(const PcMenuItem *item) {
    if (g_nitems >= PC_MENU_MAX_ITEMS || item->page < 0 || item->page >= g_npages) return -1;
    if (item->type == PC_MENU_PAGE && (item->target <= 0 || item->target >= g_npages)) return -1;
    g_items[g_nitems] = *item;
    return g_nitems++;
}

int pc_menu_count(void) { return g_nitems; }

void pc_menu_reset(void) {
    memset(g_items, 0, sizeof(g_items));
    g_nitems = 0;
    g_npages = 1;
    g_open = 0;
    g_page = 0;
    g_sel = -1;
    g_depth = 0;
    g_ndirty = 0;
    g_save = NULL;
}

void pc_menu_set_save(PcMenuSaveFn fn) { g_save = fn; }

static int selectable(int i) {
    return g_items[i].page == g_page && g_items[i].type != PC_MENU_HEADER;
}

/* The next selectable item of the page after `from` in direction `dir`
 * (wrapping); `from` itself if there is no other; -1 if none at all. */
static int step_sel(int from, int dir) {
    int i, k = from;
    for (i = 0; i < g_nitems; i++) {
        k = (k + dir + g_nitems) % g_nitems;
        if (selectable(k)) return k;
    }
    return -1;
}

static void show_page(int page, int sel) {
    g_page = page;
    if (sel >= 0 && sel < g_nitems && selectable(sel)) g_sel = sel;
    else g_sel = g_nitems ? step_sel(g_nitems - 1, 1) : -1;   /* the page's first */
    if (g_pages[page].custom) {
        g_sel = -1;
        if (g_pages[page].ops.enter) g_pages[page].ops.enter(g_pages[page].ops.ctx);
    }
}

void pc_menu_open(void) {
    if (g_open) return;
    g_open = 1;
    g_depth = 0;
    show_page(0, -1);
}

void pc_menu_open_page(int page) {
    if (page < 0 || page >= g_npages) return;
    g_open = 1;
    g_depth = 0;
    show_page(page, -1);
}

int pc_menu_is_open(void) { return g_open; }

int pc_menu_dirty_count(void) { return g_ndirty; }

int pc_menu_close(void) {
    int r = 0;
    if (!g_open) return -2;
    g_open = 0;
    if (g_ndirty && g_save) r = g_save(g_dirty, g_ndirty);
    g_ndirty = 0;
    return r;
}

static void mark_dirty(const char *key) {
    int i;
    if (!key) return;
    for (i = 0; i < g_ndirty; i++)
        if (!strcmp(g_dirty[i], key)) return;
    if (g_ndirty < PC_MENU_MAX_ITEMS) g_dirty[g_ndirty++] = key;
}

static int nchoices(const PcMenuItem *it) {
    int n = 0;
    while (it->choices && it->choices[n]) n++;
    return n;
}

static int get_value(const PcMenuItem *it) {
    return it->get ? it->get(it->ctx) : 0;
}

static void set_value(PcMenuItem *it, int v) {
    if (v == get_value(it) || !it->set) return;
    it->set(it->ctx, v);
    mark_dirty(it->key);
}

/* left (-1), right (+1) or OK (0) on an item */
static void change(PcMenuItem *it, int dir) {
    int v = get_value(it), n, step;
    switch (it->type) {
    case PC_MENU_TOGGLE:
        set_value(it, !v);
        break;
    case PC_MENU_CHOICE:
        n = nchoices(it);
        if (n <= 0) break;
        if (v < 0 || v >= n) v = 0;
        set_value(it, (v + (dir < 0 ? n - 1 : 1)) % n);
        break;
    case PC_MENU_SLIDER:
        if (!dir) break;
        step = it->step > 0 ? it->step : 1;
        v += dir * step;
        if (v < it->min) v = it->min;
        if (v > it->max) v = it->max;
        set_value(it, v);
        break;
    case PC_MENU_ACTION:
        if (!dir && it->activate) it->activate(it->ctx);
        break;
    case PC_MENU_PAGE:
        if (!dir && g_depth < PC_MENU_MAX_PAGES) {
            g_stack[g_depth] = g_page;
            g_stack_sel[g_depth] = g_sel;
            g_depth++;
            show_page(it->target, -1);
        }
        break;
    }
}

void pc_menu_input(int in) {
    if (!g_open) return;
    if (g_pages[g_page].custom) {
        const PcMenuCustom *c = &g_pages[g_page].ops;
        if (c->input && c->input(c->ctx, in)) return;
        if (in != PC_MENU_BACK) return;
    }
    switch (in) {
    case PC_MENU_UP:
    case PC_MENU_DOWN:
        if (g_sel >= 0) g_sel = step_sel(g_sel, in == PC_MENU_UP ? -1 : 1);
        break;
    case PC_MENU_LEFT:
    case PC_MENU_RIGHT:
        if (g_sel >= 0 && g_items[g_sel].type != PC_MENU_ACTION &&
            g_items[g_sel].type != PC_MENU_PAGE)
            change(&g_items[g_sel], in == PC_MENU_LEFT ? -1 : 1);
        break;
    case PC_MENU_OK:
        if (g_sel >= 0) change(&g_items[g_sel], 0);
        break;
    case PC_MENU_BACK:
        if (g_depth > 0) {
            g_depth--;
            show_page(g_stack[g_depth], g_stack_sel[g_depth]);
        } else {
            pc_menu_close();
        }
        break;
    }
}

static void value_text(const PcMenuItem *it, char *out, int n) {
    int v = get_value(it);
    out[0] = 0;
    if (it->format && (it->type == PC_MENU_SLIDER || it->type == PC_MENU_CHOICE)) {
        it->format(it->ctx, v, out, n);
        return;
    }
    switch (it->type) {
    case PC_MENU_TOGGLE:
        snprintf(out, (size_t)n, "%s", v ? "On" : "Off");
        break;
    case PC_MENU_CHOICE:
        if (v >= 0 && v < nchoices(it)) snprintf(out, (size_t)n, "%s", it->choices[v]);
        else snprintf(out, (size_t)n, "%d", v);
        break;
    case PC_MENU_SLIDER:
        snprintf(out, (size_t)n, "%d%s", v, it->unit ? it->unit : "");
        break;
    default:
        break;
    }
}

void pc_menu_view(PcMenuView *v) {
    int i;
    v->title = g_pages[g_page].title ? g_pages[g_page].title : "";
    v->nrows = 0;
    v->selected = -1;
    v->custom = g_pages[g_page].custom ? &g_pages[g_page].ops : NULL;
    if (v->custom) return;
    for (i = 0; i < g_nitems; i++) {
        const PcMenuItem *it = &g_items[i];
        PcMenuRow *r;
        if (it->page != g_page) continue;
        r = &v->rows[v->nrows];
        memset(r, 0, sizeof(*r));
        r->type = it->type;
        r->label = it->label ? it->label : "";
        value_text(it, r->value, (int)sizeof(r->value));
        r->note = it->note ? it->note(it->ctx) : NULL;
        if (!r->note && (it->flags & PC_MENU_RESTART)) r->note = "On restart";
        if (!r->note) r->note = "";
        if (it->type == PC_MENU_SLIDER) {
            r->slider = 1;
            r->pos = get_value(it);
            r->min = it->min;
            r->max = it->max;
        }
        if (i == g_sel) v->selected = v->nrows;
        v->nrows++;
    }
}
