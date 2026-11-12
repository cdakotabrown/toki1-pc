/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pc_menu.h -- the in-game settings menu's model (pc/hal/menu.c).
 *
 * A PC-layer overlay: the presenter (pc/hal/present.c) draws it over the
 * finished, scaled frame, never into PS1 VRAM, and holds the game in its
 * pause loop while it is open, so the game sees no input and no time
 * passes.  This file is the SDL-free model -- pages of items, navigation,
 * value text and the "which settings changed" bookkeeping -- so
 * pc/tests/test_settings.c can drive it without a window.
 *
 * Adding entries (another feature's page, e.g. "Save slots"):
 *
 *     static int slot_count(void *ctx) { ... }
 *     static void slot_load(void *ctx) { ... }
 *     ...
 *     int page = pc_menu_page("SAVE SLOTS");          // a new page
 *     PcMenuItem it;
 *     memset(&it, 0, sizeof it);
 *     it.type = PC_MENU_PAGE; it.label = "SAVE SLOTS"; it.target = page;
 *     pc_menu_add(&it);                               // its entry on page 0
 *     memset(&it, 0, sizeof it);
 *     it.page = page; it.type = PC_MENU_ACTION;
 *     it.label = "LOAD SLOT 1"; it.activate = slot_load; it.ctx = ...;
 *     pc_menu_add(&it);
 *
 * Items are shown in the order they are added; the presenter adds the
 * built-in entries (display, audio, input, game) when the window is
 * created, so later registrations follow them.  Labels and value text are
 * UTF-8, drawn in the UI font (pc/include/pc_ui.h: latin12, proportional;
 * text too wide for its column is cut with an ellipsis).  Everything runs on the game thread
 * while the game is held in the pause loop, so callbacks may touch HAL
 * state freely, but must not block: the window stays live meanwhile.
 *
 * Settings: an item with `key` names its settings.ini key ("display.scale",
 * "profile").  When its `set` runs, the key is marked changed; when the
 * menu closes, the save callback (the presenter's: pc_settings_save_keys,
 * refused in harness runs) receives every changed key once.  An item
 * without `key` is never saved (an action, or a session-only switch).
 */
#ifndef PC_MENU_H
#define PC_MENU_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PC_MENU_HEADER,     /* a caption; not selectable */
    PC_MENU_TOGGLE,     /* get/set 0 or 1; OK / left / right flip it */
    PC_MENU_CHOICE,     /* get/set an index into `choices`; left/right/OK cycle */
    PC_MENU_SLIDER,     /* get/set min..max in `step`s; left/right move */
    PC_MENU_ACTION,     /* OK runs `activate` */
    PC_MENU_PAGE        /* OK opens page `target`; BACK returns */
};

/* flags */
#define PC_MENU_RESTART 1u      /* the change takes effect on the next launch */

typedef struct PcMenuItem {
    int page;                   /* 0 = the main page, else a pc_menu_page() id */
    int type;                   /* PC_MENU_* */
    const char *label;
    const char *key;            /* settings.ini key saved on close, or NULL */
    const char *const *choices; /* CHOICE: NULL-terminated display names */
    int min, max, step;         /* SLIDER range and left/right step (0 = 1) */
    const char *unit;           /* SLIDER: suffix of the number ("%"), or NULL */
    int target;                 /* PAGE: the page to open */
    unsigned flags;             /* PC_MENU_RESTART */
    int  (*get)(void *ctx);
    void (*set)(void *ctx, int value);      /* apply the new value now */
    void (*activate)(void *ctx);            /* ACTION */
    /* optional: the value text instead of the default (SLIDER / CHOICE) */
    void (*format)(void *ctx, int value, char *out, int n);
    /* optional: a short note drawn after the value (NULL / "" = none) */
    const char *(*note)(void *ctx);
    void *ctx;
} PcMenuItem;

#define PC_MENU_MAX_ITEMS 96
#define PC_MENU_MAX_PAGES 16

/* A new page; returns its id (> 0), or -1 when the table is full. */
int  pc_menu_page(const char *title);
/* Copy an item into the menu; returns its index, or -1 (full / bad page). */
int  pc_menu_add(const PcMenuItem *item);
/* The number of items registered (tests; "are the built-ins in yet"). */
int  pc_menu_count(void);
/* Forget every page and item (tests). */
void pc_menu_reset(void);

/* Called with every changed settings key, once each, when the menu
 * closes; returns 0 when they were saved. */
typedef int (*PcMenuSaveFn)(const char *const *keys, int n);
void pc_menu_set_save(PcMenuSaveFn fn);

void pc_menu_open(void);
/* Close (from any page), saving the changed keys; returns the save
 * callback's result, 0 when nothing changed, -2 when nothing was open. */
int  pc_menu_close(void);
int  pc_menu_is_open(void);

enum { PC_MENU_UP, PC_MENU_DOWN, PC_MENU_LEFT, PC_MENU_RIGHT, PC_MENU_OK, PC_MENU_BACK };
/* One navigation input.  BACK on the main page closes the menu. */
void pc_menu_input(int input);

/* What the presenter draws. */
typedef struct {
    int type;
    const char *label;
    char value[48];             /* "" for headers, actions and pages */
    const char *note;           /* "" = none */
    int slider;                 /* SLIDER: 1, with pos / min / max */
    int pos, min, max;
} PcMenuRow;
typedef struct {
    const char *title;
    int nrows;
    int selected;               /* row index, or -1 */
    PcMenuRow rows[PC_MENU_MAX_ITEMS];
    const struct PcMenuCustom *custom;   /* a custom page: it draws itself */
} PcMenuView;
void pc_menu_view(PcMenuView *v);

/* The changed keys not yet saved (tests). */
int  pc_menu_dirty_count(void);

/* ---- custom pages ----
 * A page that draws itself and takes its own input, for content the item
 * list cannot show (the save browser's thumbnails, pc/hal/save_browser.c).
 * The presenter draws the panel, the title and the footer; the page draws
 * into a canvas of w x h menu units (the panel's body: one menu unit is
 * as many output pixels as the panel is scaled by) with the presenter's
 * UI font: `text` puts a line's 12-unit box at (x, y), latin12 with the
 * game's black shadow; measure, cut and wrap with pc/include/pc_ui.h
 * (ui_text_width, ui_text_fit, ui_text_wrap).  The canvas is 284 x 168
 * units.  Colours are 0xRRGGBBAA (the palette: UI_C_* in pc_ui.h). */
typedef struct PcMenuCanvas {
    void *ctx;
    int w, h;
    void (*fill)(void *ctx, int x, int y, int w, int h, unsigned rgba);
    void (*text)(void *ctx, int x, int y, unsigned rgba, const char *s);
    /* an RGB24 picture of iw x ih pixels, scaled into the w x h rectangle;
     * `key` names it (the same key = the same pixels, so it can be cached) */
    void (*image)(void *ctx, int x, int y, int w, int h, const unsigned char *rgb,
                  int iw, int ih, unsigned long key);
} PcMenuCanvas;

typedef struct PcMenuCustom {
    void (*enter)(void *ctx);                   /* the page was opened */
    /* one navigation input; return 1 when handled, 0 for the default
     * (BACK goes back / closes, the rest do nothing) */
    int  (*input)(void *ctx, int input);
    void (*draw)(void *ctx, const PcMenuCanvas *c);
    /* the footer's two help lines (0, 1); NULL = the default text */
    const char *(*help)(void *ctx, int line);
    void *ctx;
} PcMenuCustom;

/* A custom page (OPS is copied); returns its id (> 0) or -1.  Link to it
 * with a PC_MENU_PAGE item as to any page. */
int  pc_menu_custom_page(const char *title, const PcMenuCustom *ops);
/* Open the menu straight on PAGE (BACK there closes it): a prompt that is
 * not reached from the main page (the resume offer at launch). */
void pc_menu_open_page(int page);
/* The page shown: its custom ops, or NULL for an item page. */
const PcMenuCustom *pc_menu_custom(void);

#ifdef __cplusplus
}
#endif
#endif /* PC_MENU_H */
