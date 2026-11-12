/* SPDX-License-Identifier: GPL-3.0-or-later */
/* backlog.c -- the session's dialogue backlog (pc/include/pc_backlog.h).
 *
 * The list of captured pages (the newest 200), the backlog view's model
 * (open / selection / input), the replay keys, the settings-menu entries
 * and the test log.  SDL-free; the drawing and the SDL events are
 * pc/hal/backlog_view.c, the capture pc/hal/backlog_capture.c, the
 * replay pc/hal/voice_replay.c.
 *
 * The view is a presenter overlay over the paused game: while it is open
 * the presenter's pause loop holds the game thread (pc/hal/present.c,
 * held_here), exactly as for the settings menu, so the game sees no input
 * and no time passes.  Session state: kept across quickloads
 * (pc/hal/dbg_state.c KEEP_FILES), never saved to disk.
 *
 * TOKI_BACKLOG_LOG=PATH (testing): one JSON line per captured page (codes,
 * positions, colours, speaker, voice) -- the glyph bitmaps are not written.
 */
#include "types.h"
#include "pc_backlog.h"
#include "pc_menu.h"
#include "pc_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void pc_present_toast(const char *text) __attribute__((weak));
extern const PcSettings *pc_settings_get(void) __attribute__((weak));
extern PcSettings *pc_settings_mut(void) __attribute__((weak));
extern const char *hal_profile_name(void) __attribute__((weak));

static BacklogPage g_pages[BACKLOG_MAX_PAGES];
static int g_head, g_count;
static u32 g_serial;
static int g_open;
static int g_sel = -1;
static int g_view_replay;           /* the playing replay was started in the view */

static void note(const char *text)
{
    printf("[BACKLOG] %s\n", text);
    fflush(stdout);
    if (pc_present_toast) pc_present_toast(text);
}

/* ---------------------------------------------------------------- log */

static FILE *log_file(void)
{
    static int init;
    static FILE *f;
    if (!init) {
        const char *p = getenv("TOKI_BACKLOG_LOG");
        init = 1;
        if (p && *p) f = fopen(p, "w");
    }
    return f;
}

static void log_page(const BacklogPage *p)
{
    FILE *f = log_file();
    int s, k;
    if (!f) return;
    fprintf(f, "{\"serial\":%u,\"frame\":%u,\"vblank\":%u,\"screen\":\"%06x\",\"girl\":%u,"
               "\"speaker\":%d,\"glyphs\":%u,\"segs\":[", p->serial, p->frame, p->vblank,
            p->screen, p->girl, p->speaker, p->nglyph);
    for (s = 0; s < p->nseg; s++) {
        const BacklogSeg *g = &p->seg[s];
        fprintf(f, "%s{\"x\":%d,\"y\":%d,\"sjis\":\"", s ? "," : "", g->x, g->y);
        for (k = 0; k < g->count; k++) fprintf(f, "%04x", p->g[g->first + k].code);
        fprintf(f, "\",\"fg\":\"%04x\"}", g->count ? p->g[g->first].fg : 0);
    }
    fprintf(f, "]");
    if (p->has_voice)
        fprintf(f, ",\"voice\":{\"path\":\"%s\",\"file_lba\":%u,\"lba\":[%u,%u],\"req\":[%u,%u],"
                   "\"file\":%u,\"channel\":%u,\"sectors\":%u}",
                p->voice.path, p->voice.file_lba, p->voice.lba_first, p->voice.lba_last,
                p->voice.req_first, p->voice.req_last, p->voice.file, p->voice.channel,
                p->voice.sectors);
    fprintf(f, "}\n");
    fflush(f);
}

/* ---------------------------------------------------------------- list */

void backlog_add(const BacklogPage *p)
{
    int slot = (g_head + g_count) % BACKLOG_MAX_PAGES;
    BacklogPage *q;
    if (g_count == BACKLOG_MAX_PAGES) {
        slot = g_head;                      /* the oldest goes */
        g_head = (g_head + 1) % BACKLOG_MAX_PAGES;
        if (g_sel > 0) g_sel--;
    } else {
        g_count++;
    }
    q = &g_pages[slot];
    *q = *p;
    q->serial = ++g_serial;
    printf("[BACKLOG] page %u: %u glyphs, %u strings%s\n", q->serial, q->nglyph, q->nseg,
           q->has_voice ? ", voice" : "");
    fflush(stdout);
    log_page(q);
}

int backlog_count(void) { return g_count; }

const BacklogPage *backlog_page(int i)
{
    if (i < 0 || i >= g_count) return NULL;
    return &g_pages[(g_head + i) % BACKLOG_MAX_PAGES];
}

void backlog_clear(void)
{
    g_head = g_count = 0;
    g_sel = -1;
}

/* The game started a voice line of its own: a replay stops for it. */
void backlog_game_voice(void)
{
    if (voice_replay_active()) voice_replay_stop();
}

/* ---------------------------------------------------------------- view */

int backlog_is_open(void) { return g_open; }
int backlog_selected(void) { return g_open ? g_sel : -1; }

void backlog_open(void)
{
    char t[64];
    if (g_open || !backlog_enabled()) return;
    g_open = 1;
    g_sel = g_count - 1;
    snprintf(t, sizeof t, "open, %d page%s", g_count, g_count == 1 ? "" : "s");
    printf("[BACKLOG] %s\n", t);
    fflush(stdout);
}

void backlog_close(void)
{
    if (!g_open) return;
    g_open = 0;
    if (g_view_replay) voice_replay_stop();   /* Ctrl+R's plays on over the game */
    g_view_replay = 0;
    printf("[BACKLOG] closed\n");
    fflush(stdout);
}

static void replay(const BacklogPage *p)
{
    char t[64];
    if (!p || !p->has_voice) {
        note("No voice on this page");
        return;
    }
    if (!voice_replay_start(&p->voice, NULL)) {
        note("Voice not found");
        return;
    }
    g_view_replay = g_open;
    snprintf(t, sizeof t, "replay page %u", p->serial);
    printf("[BACKLOG] %s\n", t);
    fflush(stdout);
}

void backlog_replay_last(void)
{
    int i;
    if (!backlog_enabled()) return;
    for (i = g_count - 1; i >= 0; i--)
        if (backlog_page(i)->has_voice) {
            replay(backlog_page(i));
            return;
        }
    note("No voice line yet");
}

void backlog_input(int in)
{
    if (!g_open) return;
    switch (in) {
    case BACKLOG_UP: if (g_sel > 0) g_sel--; break;
    case BACKLOG_DOWN: if (g_sel < g_count - 1) g_sel++; break;
    case BACKLOG_PAGE_UP: g_sel = g_sel > 5 ? g_sel - 5 : (g_count ? 0 : -1); break;
    case BACKLOG_PAGE_DOWN: g_sel = g_sel + 5 < g_count ? g_sel + 5 : g_count - 1; break;
    case BACKLOG_HOME: g_sel = g_count ? 0 : -1; break;
    case BACKLOG_END: g_sel = g_count - 1; break;
    case BACKLOG_OK: if (g_sel >= 0) replay(backlog_page(g_sel)); break;
    case BACKLOG_BACK: backlog_close(); break;
    }
}

/* ---------------------------------------------------------------- menu */

static int get_on(void *p)
{
    (void)p;
    return pc_settings_get ? pc_settings_get()->backlog : 1;
}
static void set_on(void *p, int v)
{
    (void)p;
    if (pc_settings_mut) pc_settings_mut()->backlog = v != 0;
    if (!v) voice_replay_stop();
}
static const char *on_note(void *p)
{
    (void)p;
    if (hal_profile_name && strcmp(hal_profile_name(), "enhanced")) return "Enhanced only";
    return "Ctrl+B";
}
static void show(void *p)
{
    (void)p;
    if (!backlog_enabled()) {
        note("Backlog is off");
        return;
    }
    pc_menu_close();            /* the backlog takes over the pause */
    backlog_open();
}
static const char *show_note(void *p)
{
    static char t[24];
    (void)p;
    snprintf(t, sizeof t, "%d page%s", g_count, g_count == 1 ? "" : "s");
    return t;
}
static void replay_last(void *p)
{
    (void)p;
    backlog_replay_last();
}
static const char *replay_note(void *p)
{
    (void)p;
    return "Ctrl+R";
}

void backlog_menu_register(void)
{
    static int done;
    PcMenuItem it;
    if (done) return;
    done = 1;
    memset(&it, 0, sizeof it);
    it.type = PC_MENU_HEADER;
    it.label = "Dialogue";
    pc_menu_add(&it);
    memset(&it, 0, sizeof it);
    it.type = PC_MENU_TOGGLE;
    it.label = "Backlog";
    it.key = "enhanced.backlog";
    it.get = get_on;
    it.set = set_on;
    it.note = on_note;
    pc_menu_add(&it);
    memset(&it, 0, sizeof it);
    it.type = PC_MENU_ACTION;
    it.label = "Show backlog";
    it.activate = show;
    it.note = show_note;
    pc_menu_add(&it);
    memset(&it, 0, sizeof it);
    it.type = PC_MENU_ACTION;
    it.label = "Replay last voice";
    it.activate = replay_last;
    it.note = replay_note;
    pc_menu_add(&it);
}
