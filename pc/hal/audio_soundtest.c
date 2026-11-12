/* SPDX-License-Identifier: GPL-3.0-or-later */
/* audio_soundtest.c — Sound test / audio debug screen for the PC port.
 *
 * Toggled with F8. When active, replaces the game display with a menu that
 * lets the user audition every SEQ (BGM + short SFX SEQs), every XA track
 * (character theme BGM, if the decoder is wired), and eventually each VAB
 * voice sample.
 *
 * Navigation:
 *   Up/Down     — move cursor
 *   PgUp/PgDn   — jump 10 entries
 *   Tab / `[`/`]` — switch category (SEQ / XA / VOICE)
 *   Enter       — play selected entry
 *   Space       — stop all audio
 *   F8 / Esc    — close sound test (Esc also exits game; see caveat)
 *
 * Implementation status:
 *   - SEQ playback reuses audio_seq_load_and_play(); this is the same
 *     path the scene router already uses and is proven to work.
 *   - XA entries list the available files but playback prints a
 *     "decoder not implemented" message until task #83 lands.
 *   - VOICE entries enumerate VH/VB pairs; since we don't have a VAB
 *     renderer yet, selecting one currently just logs the asset name.
 */
#include "hal.h"
#include "pc_data_root.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/types.h>

extern void audio_seq_load_and_play(const char *path, int loop, s16 l_vol, s16 r_vol);
extern int  audio_preview_vab(const char *vh_path, int note, int velocity);
extern int  audio_xa_start(const char *path);
extern void audio_xa_stop(void);
extern void audio_seq_stop(void);   /* pc/hal/spu.c: the PC-only SEQ player */
extern void debug_log(const char *fmt, ...);
extern void draw_sprt(s32 x, s32 y, s32 w, s32 h, u8 u0, u8 v0,
                      u16 clut, u16 tpage, u8 r, u8 g, u8 b);
extern void ClearImage(RECT *rect, u8 r, u8 g, u8 b);

#define CAT_SEQ   0
#define CAT_XA    1
#define CAT_VOICE 2
#define CAT_COUNT 3

#define MAX_ENTRIES 128

typedef struct {
    char stem[24];      /* up to 23 chars of the base filename, uppercase */
    char path[256];     /* full relative path to the asset */
    char ext[8];        /* extension without leading dot (SEQ/XA/VH) */
} AudioEntry;

static AudioEntry g_entries[CAT_COUNT][MAX_ENTRIES];
static int g_entry_count[CAT_COUNT] = {0};
static int g_cursor[CAT_COUNT] = {0};
static int g_category = CAT_SEQ;
static int g_active = 0;
static int g_scanned = 0;
static char g_status[96] = "Sound test ready";

static void set_status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status, sizeof(g_status), fmt, ap);
    va_end(ap);
}

static int entry_compare(const void *a, const void *b) {
    return strcmp(((const AudioEntry *)a)->stem, ((const AudioEntry *)b)->stem);
}

static void to_upper(char *s) {
    for (; *s; s++) {
        if (*s >= 'a' && *s <= 'z') *s -= 32;
    }
}

static int ends_with(const char *s, const char *suffix) {
    size_t sl = strlen(s), tl = strlen(suffix);
    if (tl > sl) return 0;
    return strcasecmp(s + sl - tl, suffix) == 0;
}

static void scan_dir(const char *dir, const char *want_ext, int category) {
    DIR *d = opendir(dir);
    struct dirent *ent;
    if (!d) return;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (!ends_with(ent->d_name, want_ext)) continue;
        if (g_entry_count[category] >= MAX_ENTRIES) break;
        AudioEntry *e = &g_entries[category][g_entry_count[category]++];
        /* stem */
        const char *dot = strrchr(ent->d_name, '.');
        size_t stem_len = dot ? (size_t)(dot - ent->d_name) : strlen(ent->d_name);
        if (stem_len >= sizeof(e->stem)) stem_len = sizeof(e->stem) - 1;
        memcpy(e->stem, ent->d_name, stem_len);
        e->stem[stem_len] = '\0';
        to_upper(e->stem);
        /* full path */
        snprintf(e->path, sizeof(e->path), "%s/%s", dir, ent->d_name);
        /* extension (lowercase, no dot) */
        if (dot) {
            size_t el = strlen(dot + 1);
            if (el >= sizeof(e->ext)) el = sizeof(e->ext) - 1;
            memcpy(e->ext, dot + 1, el);
            e->ext[el] = '\0';
        } else {
            e->ext[0] = '\0';
        }
    }
    closedir(d);
}

static void scan_assets(void) {
    if (g_scanned) return;
    g_scanned = 1;
    /* SEQ: scene router uses raw/CDROM/VAB_SEQ/. pc/assets/music/ is the
     * same file set, kept in sync. Prefer the scene-router path so the
     * SF2 derivation rule in spu.c keeps working. */
    char d[1100];
    scan_dir(pc_asset_path(d, sizeof d, "raw/CDROM/VAB_SEQ"), ".SEQ", CAT_SEQ);
    if (g_entry_count[CAT_SEQ] == 0) {
        scan_dir(pc_asset_path(d, sizeof d, "music"), ".SEQ", CAT_SEQ);
    }
    scan_dir(pc_asset_path(d, sizeof d, "audio"), ".XA", CAT_XA);
    /* Voice: list VH files; VB is the paired sample data. */
    scan_dir(pc_asset_path(d, sizeof d, "music"), ".VH", CAT_VOICE);

    qsort(g_entries[CAT_SEQ], g_entry_count[CAT_SEQ],
          sizeof(AudioEntry), entry_compare);
    qsort(g_entries[CAT_XA], g_entry_count[CAT_XA],
          sizeof(AudioEntry), entry_compare);
    qsort(g_entries[CAT_VOICE], g_entry_count[CAT_VOICE],
          sizeof(AudioEntry), entry_compare);
    debug_log("Sound test: scanned SEQ=%d XA=%d VOICE=%d",
              g_entry_count[CAT_SEQ], g_entry_count[CAT_XA],
              g_entry_count[CAT_VOICE]);
}

static void play_current(void);

int soundtest_is_active(void) {
    /* Lazy startup: if env SOUNDTEST=1 is set, open on first query.
     * SOUNDTEST_PLAY=<STEM> also auto-selects and plays that SEQ for
     * headless verification (matches by stem, case-insensitive). */
    static int env_checked = 0;
    if (!env_checked) {
        const char *env = getenv("SOUNDTEST");
        env_checked = 1;
        if (env && env[0] && env[0] != '0') {
            scan_assets();
            g_active = 1;
            set_status("Auto-opened via SOUNDTEST env var");
            debug_log("Sound test: auto-opened (SOUNDTEST=%s)", env);
        }
        const char *autoplay = getenv("SOUNDTEST_PLAY");
        if (autoplay && autoplay[0]) {
            char want[32];
            strncpy(want, autoplay, sizeof(want) - 1);
            want[sizeof(want) - 1] = '\0';
            to_upper(want);
            scan_assets();
            g_active = 1;
            int cat, i;
            for (cat = 0; cat < CAT_COUNT; cat++) {
                for (i = 0; i < g_entry_count[cat]; i++) {
                    if (strcmp(g_entries[cat][i].stem, want) == 0) {
                        g_category = cat;
                        g_cursor[cat] = i;
                        play_current();
                        return g_active;
                    }
                }
            }
            debug_log("Sound test: SOUNDTEST_PLAY=%s not found", autoplay);
        }
    }
    return g_active;
}

void soundtest_toggle(void) {
    scan_assets();
    g_active = !g_active;
    if (g_active) {
        set_status("SEQ=%d XA=%d VOICE=%d   ENTER play   SPACE stop",
                   g_entry_count[CAT_SEQ], g_entry_count[CAT_XA],
                   g_entry_count[CAT_VOICE]);
    } else {
        audio_seq_stop();
        audio_xa_stop();
    }
    debug_log("Sound test: %s", g_active ? "OPEN" : "CLOSED");
}

static void play_current(void) {
    int c = g_category;
    int idx = g_cursor[c];
    if (idx < 0 || idx >= g_entry_count[c]) return;
    AudioEntry *e = &g_entries[c][idx];
    if (c == CAT_SEQ) {
        audio_seq_load_and_play(e->path, 1, 112, 112);
        set_status("Playing SEQ %s", e->stem);
        debug_log("Sound test: play SEQ %s (%s)", e->stem, e->path);
    } else if (c == CAT_XA) {
        /* Stop SEQ so the XA stream plays solo for auditioning. */
        audio_seq_stop();
        if (audio_xa_start(e->path)) {
            set_status("Playing XA %s", e->stem);
            debug_log("Sound test: play XA %s (%s)", e->stem, e->path);
        } else {
            set_status("XA %s: decode failed", e->stem);
            debug_log("Sound test: XA %s failed to start (%s)",
                      e->stem, e->path);
        }
    } else {
        /* Audition the VAB's matching SF2 with a single middle-C note.
         * This proves the bank is present and audible; full per-program
         * voice selection is a follow-up (see task #85). */
        if (audio_preview_vab(e->path, 60, 110)) {
            set_status("Playing VAB %s note 60", e->stem);
            debug_log("Sound test: VAB %s note 60 (%s)", e->stem, e->path);
        } else {
            set_status("VAB %s has no matching SF2 in soundbanks/", e->stem);
            debug_log("Sound test: VAB %s missing SF2 (%s)",
                      e->stem, e->path);
        }
    }
}

static void stop_all(void) {
    audio_seq_stop();
    audio_xa_stop();
    set_status("Stopped");
    debug_log("Sound test: STOP");
}

/* Called by debug.c when F8 or any handled key is pressed AND soundtest
 * is active. Returns 1 if the event was consumed, 0 to pass through. */
int soundtest_handle_key(SDL_Event *e) {
    if (!g_active) return 0;
    if (e->type != SDL_KEYDOWN) return 0;
    int c = g_category;
    int n = g_entry_count[c];
    int cur = g_cursor[c];
    switch (e->key.keysym.sym) {
    case SDLK_UP:
        if (n > 0) g_cursor[c] = (cur + n - 1) % n;
        return 1;
    case SDLK_DOWN:
        if (n > 0) g_cursor[c] = (cur + 1) % n;
        return 1;
    case SDLK_PAGEUP:
        if (n > 0) {
            int nv = cur - 10;
            if (nv < 0) nv = 0;
            g_cursor[c] = nv;
        }
        return 1;
    case SDLK_PAGEDOWN:
        if (n > 0) {
            int nv = cur + 10;
            if (nv >= n) nv = n - 1;
            g_cursor[c] = nv;
        }
        return 1;
    case SDLK_TAB:
    case SDLK_RIGHTBRACKET:
        g_category = (g_category + 1) % CAT_COUNT;
        set_status("Category: %s",
                   g_category == CAT_SEQ ? "SEQ (BGM)" :
                   g_category == CAT_XA  ? "XA (streaming)" : "VOICE (VAB)");
        return 1;
    case SDLK_LEFTBRACKET:
        g_category = (g_category + CAT_COUNT - 1) % CAT_COUNT;
        set_status("Category: %s",
                   g_category == CAT_SEQ ? "SEQ (BGM)" :
                   g_category == CAT_XA  ? "XA (streaming)" : "VOICE (VAB)");
        return 1;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        play_current();
        return 1;
    case SDLK_SPACE:
        stop_all();
        return 1;
    default:
        return 0;
    }
}

/* Minimal text renderer: the project's font tpage (0x000A/clut 481) only
 * carries A-Z, a-z, 0-9. We replace anything else with `_` so labels stay
 * aligned. Uppercase-only rendering makes this font forgiving. */
static void st_text(s32 x, s32 y, const char *text, u8 r, u8 g, u8 b) {
    u16 tpage = 0x000A;
    u16 clut = (481 << 6) | 0;
    s32 i;
    for (i = 0; text[i]; i++) {
        char ch = text[i];
        if (ch == ' ') continue;
        /* remap: digits already supported; lowercase -> upper; others skip */
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) {
            s32 u0, v0;
            if (ch >= '0' && ch <= '9') { u0 = (ch - '0') * 8; v0 = 16; }
            else { u0 = (ch - 'A') * 8; v0 = 0; }
            draw_sprt(x + i * 8, y, 8, 14, u0, v0, clut, tpage, r, g, b);
        }
    }
}

#define VIEW_ROWS 16

void soundtest_draw(void) {
    RECT r;
    int c = g_category;
    int n = g_entry_count[c];
    int cur = g_cursor[c];
    int top = cur - VIEW_ROWS / 2;
    int i;
    const char *cat_name;

    if (!g_active) return;

    /* Dark-blue background box covering a 320x240 field */
    r.x = 0; r.y = 0; r.w = 320; r.h = 240;
    ClearImage(&r, 0, 0, 30);

    /* Header band */
    r.x = 0; r.y = 0; r.w = 320; r.h = 18;
    ClearImage(&r, 40, 40, 90);

    st_text(6, 3, "SOUND TEST", 255, 255, 80);
    cat_name = (c == CAT_SEQ) ? "SEQ" : (c == CAT_XA) ? "XA" : "VOICE";
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%s %d of %d",
                 cat_name, n > 0 ? cur + 1 : 0, n);
        st_text(130, 3, buf, 180, 220, 255);
    }
    {
        char nav[16];
        snprintf(nav, sizeof(nav), "TAB SWITCH");
        st_text(228, 3, nav, 180, 180, 180);
    }

    /* List */
    if (top < 0) top = 0;
    if (top > n - VIEW_ROWS) top = n - VIEW_ROWS;
    if (top < 0) top = 0;
    for (i = 0; i < VIEW_ROWS && top + i < n; i++) {
        AudioEntry *e = &g_entries[c][top + i];
        int y = 24 + i * 12;
        int is_sel = (top + i == cur);
        u8 cr = is_sel ? 0 : 0;
        u8 cg = is_sel ? 80 : 0;
        u8 cb = is_sel ? 30 : 0;
        RECT rr; rr.x = 4; rr.y = y - 1; rr.w = 312; rr.h = 11;
        ClearImage(&rr, cr, cg, cb);
        {
            u8 tr = is_sel ? 255 : 220;
            u8 tg = is_sel ? 255 : 220;
            u8 tb = is_sel ? 120 : 220;
            char line[40];
            snprintf(line, sizeof(line), "%s %s", e->stem, e->ext);
            st_text(10, y, line, tr, tg, tb);
        }
    }

    /* Status band */
    r.x = 0; r.y = 218; r.w = 320; r.h = 22;
    ClearImage(&r, 20, 20, 40);
    st_text(6, 223, g_status, 200, 255, 200);

    DrawSync(0);
}
