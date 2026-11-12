/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_save_browser.c -- the save browser page (pc/hal/save_browser.c) on
 * the menu's custom-page API (pc/hal/menu.c), the thumbnail PNG reader
 * (pc/hal/present_layout.c) and the [saves] settings.  No SDL, no game:
 * the save entries come from the fake quicksave.h below, which records the
 * requests the page makes.  The real files, loads and saves are checked
 * against the binary by pc/tools/test_saves.py. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pc_menu.h"
#include "pc_present.h"
#include "pc_settings.h"
#include "pc_ui.h"
#include "../hal/quicksave.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "  %s:%d: %s\n", __FILE__, __LINE__, #c); return __LINE__; } } while (0)

/* ---- a fake quicksave.c ---- */
static QsEntry g_fake[8];
static int g_nfake;
static char g_saved[32], g_loaded[32], g_deleted[32];

int qs_entries(QsEntry *out, int max) {
    int i;
    for (i = 0; i < g_nfake && i < max; i++) out[i] = g_fake[i];
    return i;
}
void qs_request_save_id(const char *id) { snprintf(g_saved, sizeof g_saved, "%s", id); }
void qs_request_load_id(const char *id) { snprintf(g_loaded, sizeof g_loaded, "%s", id); }
int qs_new_save_id(char *out, size_t cap) { snprintf(out, cap, "save%04d", 7); return 1; }
int qs_delete_id(const char *id) {
    int i;
    snprintf(g_deleted, sizeof g_deleted, "%s", id);
    for (i = 0; i < g_nfake; i++)
        if (!strcmp(g_fake[i].id, id)) {
            memmove(&g_fake[i], &g_fake[i + 1], (size_t)(g_nfake - i - 1) * sizeof *g_fake);
            g_nfake--;
            return 0;
        }
    return -1;
}
int qs_thumb_read(const char *id, unsigned char *rgb) { (void)id; (void)rgb; return 0; }
void qs_notify(const char *text) { (void)text; }
void qs_id_name(const char *id, char *out, size_t cap) { snprintf(out, cap, "%s", id); }
extern int save_browser_page(void);

static void fake(int i, const char *id, int kind, int same_build, long long when) {
    memset(&g_fake[i], 0, sizeof g_fake[i]);
    snprintf(g_fake[i].id, sizeof g_fake[i].id, "%s", id);
    g_fake[i].kind = kind;
    g_fake[i].number = atoi(id + strcspn(id, "0123456789"));
    g_fake[i].has_state = g_fake[i].has_index = 1;
    g_fake[i].same_build = same_build;
    g_fake[i].saved_unix = when;
    g_fake[i].in_progress = 1;
    g_fake[i].year = 1995;
    g_fake[i].month = 4;
    g_fake[i].day = 16;
    snprintf(g_fake[i].weekday, sizeof g_fake[i].weekday, "Sun");
}

/* a canvas that records what was drawn, and any text that would run past
 * the canvas (latin12 is proportional: the page measures, cuts, wraps) */
static char g_drawn[4096];
static int g_outside;
static void c_fill(void *ctx, int x, int y, int w, int h, unsigned rgba) {
    (void)ctx; (void)x; (void)y; (void)w; (void)h; (void)rgba;
}
static void c_text(void *ctx, int x, int y, unsigned rgba, const char *s) {
    (void)ctx; (void)rgba;
    if (x < 0 || y < 0 || x + ui_text_width(s) > 284 || y + UI_BASE + 1 > 168) {
        fprintf(stderr, "  outside the canvas: %d,%d \"%s\"\n", x, y, s);
        g_outside++;
    }
    strncat(g_drawn, s, sizeof g_drawn - strlen(g_drawn) - 2);
    strcat(g_drawn, "|");
}
static void c_image(void *ctx, int x, int y, int w, int h, const unsigned char *rgb, int iw,
                    int ih, unsigned long key) {
    (void)ctx; (void)x; (void)y; (void)w; (void)h; (void)rgb; (void)iw; (void)ih; (void)key;
}
static void draw_page(void) {
    PcMenuView v;
    PcMenuCanvas cv = { NULL, 284, 168, c_fill, c_text, c_image };
    g_drawn[0] = 0;
    pc_menu_view(&v);
    if (v.custom && v.custom->draw) v.custom->draw(v.custom->ctx, &cv);
}

static void setup(void) {
    /* an automated run's settings: the player's settings file is never read */
    setenv("TOKI_SETTINGS", "off", 1);
    pc_menu_reset();
    g_saved[0] = g_loaded[0] = g_deleted[0] = 0;
    g_nfake = 3;
    fake(0, "auto2", QS_KIND_AUTO, 1, 300);
    fake(1, "slot3", QS_KIND_QUICK, 1, 200);
    fake(2, "save0001", QS_KIND_SAVE, 0, 100);       /* another build */
    save_browser_register();
}

int test_save_browser_pages(void) {
    PcMenuView v;
    int i, page = -1, opts = 0;
    setup();
    /* the main page: a SAVES header, the browser and the options pages */
    pc_menu_open();
    pc_menu_view(&v);
    CHECK(v.custom == NULL);
    for (i = 0; i < v.nrows; i++) {
        if (!strcmp(v.rows[i].label, "Browse saves")) page = i;
        if (!strcmp(v.rows[i].label, "Save options")) opts = i;
    }
    CHECK(page >= 1 && opts == page + 1 && !strcmp(v.rows[page - 1].label, "Saves"));
    pc_menu_close();

    /* a label too long for the details column (checked below) */
    snprintf(g_fake[1].label, sizeof g_fake[1].label, "BEFORE THE DATE WITH SHIORI AT THE ZOO");
    /* straight onto the browser (the saves hotkey): newest first, NEW SAVE on top */
    pc_menu_open_page(save_browser_page());
    CHECK(pc_menu_is_open() && pc_menu_custom() != NULL);
    draw_page();
    if (!strstr(g_drawn, "+ New save|Auto 2|95/04/16 Sun|Slot 3|95/04/16 Sun|Save 1")) fprintf(stderr, "%s\n", g_drawn);
    CHECK(strstr(g_drawn, "+ New save|Auto 2|95/04/16 Sun|Slot 3|95/04/16 Sun|Save 1"));
    CHECK(strstr(g_drawn, "New save|Save the game"));          /* the New save row's panel */
    /* every row and panel: no text past the canvas; a label too long for
     * the details column is cut with an ellipsis, not run off */
    g_outside = 0;
    for (i = 0; i < 4; i++) {
        draw_page();
        pc_menu_input(PC_MENU_DOWN);
    }
    CHECK(g_outside == 0);
    pc_menu_input(PC_MENU_DOWN);
    pc_menu_input(PC_MENU_DOWN);                               /* slot3 */
    draw_page();
    CHECK(strstr(g_drawn, "|BEFORE THE DATE WITH S\xE2\x80\xA6|"));   /* 138 of 142 units */
    CHECK(g_outside == 0);
    g_fake[1].label[0] = 0;
    /* BACK from the browser opened straight closes the menu */
    pc_menu_input(PC_MENU_BACK);
    CHECK(!pc_menu_is_open());
    return 0;
}

int test_save_browser_actions(void) {
    setup();
    pc_menu_open_page(save_browser_page());
    /* NEW SAVE: a request for the next free saveNNNN, and the menu closes */
    pc_menu_input(PC_MENU_OK);
    CHECK(!strcmp(g_saved, "save0007") && !pc_menu_is_open());

    /* load auto2: OK opens the actions (LOAD first) */
    pc_menu_open_page(save_browser_page());
    pc_menu_input(PC_MENU_DOWN);
    draw_page();
    CHECK(strstr(g_drawn, "auto2|1995/04/16 Sun  week 0"));
    pc_menu_input(PC_MENU_OK);
    draw_page();
    CHECK(strstr(g_drawn, "Load|Delete|Cancel|"));           /* no Save here on an autosave */
    pc_menu_input(PC_MENU_OK);
    CHECK(!strcmp(g_loaded, "auto2") && !pc_menu_is_open());

    /* a quick slot offers SAVE HERE; BACK leaves the actions, not the page */
    g_loaded[0] = 0;
    pc_menu_open_page(save_browser_page());
    pc_menu_input(PC_MENU_DOWN);
    pc_menu_input(PC_MENU_DOWN);
    pc_menu_input(PC_MENU_OK);
    draw_page();
    CHECK(strstr(g_drawn, "Load|Save here|Delete|Cancel|"));
    pc_menu_input(PC_MENU_BACK);
    CHECK(pc_menu_is_open() && pc_menu_custom() != NULL);
    pc_menu_input(PC_MENU_OK);
    pc_menu_input(PC_MENU_DOWN);
    pc_menu_input(PC_MENU_OK);                                /* SAVE HERE */
    CHECK(!strcmp(g_saved, "slot3") && !pc_menu_is_open());

    /* another build's save: shown, but LOAD refuses with a message */
    pc_menu_open_page(save_browser_page());
    pc_menu_input(PC_MENU_UP);                                /* wraps to the last row */
    draw_page();
    CHECK(strstr(g_drawn, "Another build of the|game: cannot load"));
    pc_menu_input(PC_MENU_OK);
    pc_menu_input(PC_MENU_OK);
    draw_page();
    CHECK(strstr(g_drawn, "Saved by another build") && !g_loaded[0] && pc_menu_is_open());
    pc_menu_input(PC_MENU_OK);                                /* dismiss */

    /* DELETE asks first; NO keeps it, YES deletes it and the list shrinks */
    pc_menu_input(PC_MENU_OK);
    pc_menu_input(PC_MENU_DOWN);                              /* SAVE HERE, DELETE */
    pc_menu_input(PC_MENU_DOWN);
    pc_menu_input(PC_MENU_OK);
    draw_page();
    CHECK(strstr(g_drawn, "Delete this save?|\xE2\x99\xA5|No|Yes, delete|"));
    pc_menu_input(PC_MENU_OK);                                /* NO */
    CHECK(!g_deleted[0] && g_nfake == 3);
    pc_menu_input(PC_MENU_OK);
    pc_menu_input(PC_MENU_DOWN);
    pc_menu_input(PC_MENU_DOWN);
    pc_menu_input(PC_MENU_OK);
    pc_menu_input(PC_MENU_DOWN);
    pc_menu_input(PC_MENU_OK);                                /* YES */
    CHECK(!strcmp(g_deleted, "save0001") && g_nfake == 2);
    draw_page();
    CHECK(!strstr(g_drawn, "Save 1") && strstr(g_drawn, "Slot 3"));
    pc_menu_close();
    return 0;
}

int test_save_browser_resume_prompt(void) {
    setup();
    fake(1, "resume", QS_KIND_RESUME, 1, 400);
    save_browser_resume_prompt();
    CHECK(pc_menu_is_open() && pc_menu_custom() != NULL);
    draw_page();
    CHECK(strstr(g_drawn, "Continue where you|") && strstr(g_drawn, "Continue|Start normally|"));
    CHECK(strstr(g_drawn, "resume|"));                       /* only the resume entry */
    pc_menu_input(PC_MENU_OK);                                /* CONTINUE */
    CHECK(!strcmp(g_loaded, "resume") && !pc_menu_is_open());
    /* ESC starts normally: nothing requested */
    g_loaded[0] = 0;
    save_browser_resume_prompt();
    pc_menu_input(PC_MENU_BACK);
    CHECK(!pc_menu_is_open() && !g_loaded[0]);
    return 0;
}

int test_save_settings_and_png_read(void) {
    PcSettings s;
    unsigned char rgb[4 * 3 * 3], back[4 * 3 * 3];
    char path[64];
    int i;
    pc_settings_defaults(&s);
    CHECK(s.autosave == 1 && s.autosave_slots == 3 && s.autosave_ending == 1 && s.resume == 1);
    CHECK(!strcmp(s.hotkey[PC_HK_SAVES], "Ctrl+S") && !strcmp(pc_hotkey_names[PC_HK_SAVES], "saves"));
    CHECK(pc_settings_set(&s, "saves.autosave_slots", "9") == 0 && s.autosave_slots == 9);
    CHECK(pc_settings_set(&s, "saves.autosave_slots", "10") != 0 && s.autosave_slots == 9);
    CHECK(pc_settings_set(&s, "saves.autosave", "false") == 0 && s.autosave == 0);
    /* the writer's PNG reads back exactly; any other size is refused */
    for (i = 0; i < (int)sizeof rgb; i++) rgb[i] = (unsigned char)(i * 7 + 3);
    snprintf(path, sizeof path, "/tmp/test_png_read_%d.png", (int)getpid());
    CHECK(present_png_write(path, rgb, 4, 3) == 0);
    memset(back, 0, sizeof back);
    CHECK(present_png_read(path, back, 4, 3) == 0 && !memcmp(rgb, back, sizeof rgb));
    CHECK(present_png_read(path, back, 3, 4) != 0);
    CHECK(present_png_read("/nonexistent/x.png", back, 4, 3) != 0);
    unlink(path);
    return 0;
}
