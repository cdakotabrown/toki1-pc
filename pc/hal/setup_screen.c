/* SPDX-License-Identifier: GPL-3.0-or-later */
/* setup_screen.c -- first run in the game window (pc/include/pc_setup.h).
 *
 * The player has the executable and their own disc image; this file turns
 * that into a running game with no console and no other tool:
 *   - finds the disc: disk/ beside the executable first (any .cue, else
 *     the .bin pair), then --disc, then the executable's folder, its parent
 *     and Downloads / Documents / Desktop one level deep -- only files whose
 *     size is the supported Track 1's (or a .cue naming one) are offered, and
 *     nothing is hashed until the player agrees;
 *   - takes a dropped file (SDL_DROPFILE) or one chosen in the system file
 *     picker (Windows: GetOpenFileNameW; Linux: zenity or kdialog when
 *     installed);
 *   - runs the C extractor (pc/hal/asset_setup.c) on a worker thread with a
 *     progress bar, and turns each failure into a specific message: which
 *     file, what was expected, what was found, what to do;
 *   - logs every check, path and hash to tokimeki-pc.log.
 * Drawn like the settings menu (pc/include/pc_ui_draw.h): the game's
 * message window over its checked backdrop, latin12 text (paths and
 * licences in their own case), at the largest whole scale that keeps a
 * canvas of at least 480 x 360 units.  No game data exists yet here, so
 * there is no Japanese to draw.
 *
 * Test hooks (never needed by a player):
 *   TOKI_SETUP_SCRIPT="drop:PATH;key:return;key:c;quit"  injected input,
 *       one item each time the screen is idle;
 *   TOKI_SETUP_SHOTS=DIR   a BMP of each screen state (NN-state.bmp).
 */
#include "pc_setup.h"
#include "asset_setup.h"
#include "pc_data_root.h"
#include "pc_fs.h"
#include "pc_menu.h"
#include "pc_present.h"
#include "pc_settings.h"
#include "pc_ui_draw.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#define SEP "\\"
#else
#include <unistd.h>
#define SEP "/"
#endif

/* ------------------------------------------------------------------ */
/* arguments                                                           */
/* ------------------------------------------------------------------ */

int pc_setup_args(int argc, char **argv, PcSetupOpts *o) {
    int i, n = 1;
    memset(o, 0, sizeof *o);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--disc") && i + 1 < argc) { o->disc = argv[++i]; continue; }
        if (!strncmp(a, "--disc=", 7)) { o->disc = a + 7; continue; }
        if (!strcmp(a, "--audio-track") && i + 1 < argc) { o->audio = argv[++i]; continue; }
        if (!strcmp(a, "--check")) { o->check = 1; continue; }
        if (!strcmp(a, "--licenses") || !strcmp(a, "--licences")) { o->licenses = 1; continue; }
        if (!strcmp(a, "--prepare-assets") || !strcmp(a, "--verify-assets")) o->cli = 1;
        argv[n++] = argv[i];
    }
    argv[n] = NULL;
    return n;
}

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static int ends_with_ci(const char *s, const char *sfx) {
    size_t a = strlen(s), b = strlen(sfx), i;
    if (a < b) return 0;
    for (i = 0; i < b; i++)
        if (tolower((unsigned char)s[a - b + i]) != tolower((unsigned char)sfx[i])) return 0;
    return 1;
}

static const char *base_name(const char *p) {
    const char *a = strrchr(p, '/'), *b = strrchr(p, '\\');
    if (b && (!a || b > a)) a = b;
    return a ? a + 1 : p;
}

static void dir_of(const char *p, char *out, size_t n) {
    const char *b = base_name(p);
    if (b == p) snprintf(out, n, ".");
    else snprintf(out, n, "%.*s", (int)(b - p - 1), p);
}

/* Native separators for the player's eyes. */
static void native(char *s) {
#ifdef _WIN32
    for (; *s; s++) if (*s == '/') *s = '\\';
#else
    (void)s;
#endif
}

static void path_join(char *out, size_t n, const char *a, const char *b) {
    snprintf(out, n, "%s" SEP "%s", a, b);
}

static void fmt_bytes(int64_t v, char *out, size_t n) {
    /* 702104928 -> "702,104,928" */
    char tmp[32];
    int len, i, k = 0;
    snprintf(tmp, sizeof tmp, "%lld", (long long)v);
    len = (int)strlen(tmp);
    for (i = 0; i < len && k + 2 < (int)n; i++) {
        out[k++] = tmp[i];
        if (tmp[i] != '-' && (len - i - 1) % 3 == 0 && i < len - 1) out[k++] = ',';
    }
    out[k] = 0;
}

static const PcDataRoot *root(void) { return pc_data_root(); }

/* <root>/data (pc_data_dir: <root>/assets while an old install's folder
 * could not be moved) */
static void assets_dest(char *out, size_t n) { snprintf(out, n, "%s", pc_data_dir()); }

static void disk_dir(char *out, size_t n) { path_join(out, n, root()->exe_dir, "disk"); }

/* ------------------------------------------------------------------ */
/* the game-data folder                                                */
/* ------------------------------------------------------------------ */

static AsError g_verify_err;            /* the fast check's failure, for the screen */
static int g_damaged;                   /* <root>/data exists but fails the check */

static int legacy_tree(const char *dir) {
    char p[PC_ROOT_PATH_MAX + 32];
    PcStat st;
    snprintf(p, sizeof p, "%s/raw/PSX.EXE", dir);
    return pc_fs_stat(p, &st) == 0;
}

/* The data folder `dir` as found: 1 game data that passes the fast check,
 * 0 none yet (missing, or only the log / en/), -1 damaged or unmanaged
 * (g_verify_err says why). */
static int data_state(const char *dir) {
    char other[256];
    AsCtl c;
    switch (as_dest_status(dir, other, sizeof other)) {
    case AS_DEST_EMPTY:
        return 0;
    case AS_DEST_OTHER:
        memset(&g_verify_err, 0, sizeof g_verify_err);
        g_verify_err.code = AS_E_DEST_INVALID;
        snprintf(g_verify_err.message, sizeof g_verify_err.message,
                 "%s holds %s but no game data (no %s)", dir, other[0] ? other : "a file", AS_MANIFEST);
        snprintf(g_verify_err.path, sizeof g_verify_err.path, "%s", dir);
        return -1;
    default:
        break;
    }
    memset(&c, 0, sizeof c);
    c.fast_verify = 1;
    memset(&g_verify_err, 0, sizeof g_verify_err);
    return as_verify(dir, NULL, &c, &g_verify_err) == AS_OK ? 1 : -1;
}

static int fast_ok(const char *dir, AsError *err) {
    AsCtl c;
    memset(&c, 0, sizeof c);
    c.fast_verify = 1;
    memset(err, 0, sizeof *err);
    return as_verify(dir, NULL, &c, err) == AS_OK;
}

/* a data folder path longer than this risks MAX_PATH below it (Windows) */
#define PC_WIN_LONG_ROOT 150u

static void log_root(void) {
    const PcDataRoot *r = root();
    static const char *const modes[] = {"per-user", "portable", "TOKI_CONFIG_DIR"};
    pc_logf("[SETUP] executable folder: %s", r->exe_dir);
    pc_logf("[SETUP] data folder: %s (%s; %s)%s", r->root, modes[r->mode], r->why,
            r->fallback ? " -- the executable's folder is not writable, so the per-user folder is used" : "");
#ifdef _WIN32
    /* Windows limits a full path to 260 characters unless long paths are
     * enabled (the manifest declares longPathAware, the system setting
     * decides).  The deepest files below the data folder (texture packs,
     * the extracted disc) add ~110 characters. */
    if (strlen(r->root) > PC_WIN_LONG_ROOT)
        pc_logf("[SETUP] WARNING: the data folder path is %u characters long; Windows limits a "
                "full path to 260 characters unless long paths are enabled, so files deep "
                "below it may fail to open -- move the folder higher up (e.g. C:\\Games\\tokimeki-pc)",
                (unsigned)strlen(r->root));
#endif
}

const char *pc_setup_choose(const char *explicit_dir, const PcSetupOpts *o) {
    static char dir[PC_ROOT_PATH_MAX + 32];
    const PcDataRoot *r = root();
    const char *v;
    PcStat st;
    (void)o;
    if (explicit_dir) {
        pc_logf("[SETUP] game data: %s (given on the command line)", explicit_dir);
        return explicit_dir;
    }
    if ((v = getenv("TOKI_ASSET_DIR")) && *v) {
        pc_logf("[SETUP] game data: %s (TOKI_ASSET_DIR)", v);
        return v;
    }
    assets_dest(dir, sizeof dir);
    if (r->mode != PC_ROOT_USER || r->fallback) {
        /* portable (or a chosen folder): its own data/ first */
        int k = data_state(dir);
        if (k > 0) {
            pc_logf("[SETUP] game data: %s (manifest, sizes and executable SHA1 checked)", dir);
            pc_data_root_set_player(1);
            return dir;
        }
        if (k < 0) {
            g_damaged = 1;
            pc_logf("[SETUP] game data in %s fails the check: %s", dir, g_verify_err.message);
            return NULL;
        }
    }
    if (legacy_tree("./pc/assets")) {
        pc_logf("[SETUP] game data: ./pc/assets (working folder, developer layout)");
        return "./pc/assets";
    }
    {
        static char launcher[PC_ROOT_PATH_MAX + 32];
        AsError e;
        snprintf(launcher, sizeof launcher, "%s/pc/assets", r->exe_dir);
        if (!pc_fs_stat(launcher, &st) && fast_ok(launcher, &e)) {
            pc_logf("[SETUP] game data: %s (the launcher's folder beside the executable)", launcher);
            return launcher;
        }
    }
    {
        int k = data_state(dir);
        if (k > 0) {
            pc_logf("[SETUP] game data: %s (manifest, sizes and executable SHA1 checked)", dir);
            pc_data_root_set_player(1);
            return dir;
        }
        if (k < 0) {
            g_damaged = 1;
            pc_logf("[SETUP] game data in %s fails the check: %s", dir, g_verify_err.message);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* finding the disc                                                    */
/* ------------------------------------------------------------------ */

enum { DISK_MISSING, DISK_EMPTY, DISK_NOTHING_USABLE, DISK_FOUND };
enum { FOUND_NONE, FOUND_DISK, FOUND_ARG, FOUND_ELSEWHERE };

typedef struct {
    int where;                  /* FOUND_* */
    int disk_state;             /* DISK_* */
    char path[PC_ROOT_PATH_MAX + 300];
    char listing[400];          /* what disk/ holds when nothing there is usable */
} Discovery;

typedef struct {
    char best[PC_ROOT_PATH_MAX + 300];
    int rank;                   /* 4 cue candidate, 3 cue, 2 bin of the size, 1 bin, 0.5 other image */
    int files;
    char listing[400];
    const char *dir;
} DiskScan;

static int image_like(const char *name) {
    static const char *const ext[] = {".iso", ".chd", ".pbp", ".ecm", ".zip", ".7z", ".rar", ".img",
                                      ".mdf", ".mds", ".ccd", ".cdi", ".nrg", NULL};
    int i;
    for (i = 0; ext[i]; i++) if (ends_with_ci(name, ext[i])) return 1;
    return 0;
}

static int scan_disk_entry(const char *name, const PcStat *st, void *ud) {
    DiskScan *s = (DiskScan *)ud;
    char full[PC_ROOT_PATH_MAX + 300];
    int rank = 0;
    if (st->is_dir) return 0;
    s->files++;
    if (strlen(s->listing) + strlen(name) + 3 < sizeof s->listing) {
        if (s->listing[0]) strcat(s->listing, ", ");
        strcat(s->listing, name);
    }
    path_join(full, sizeof full, s->dir, name);
    if (ends_with_ci(name, ".cue")) rank = as_quick_candidate(full, NULL) ? 40 : 30;
    else if (ends_with_ci(name, ".bin")) {
        PcStat fs;
        int track1 = !pc_fs_stat(full, &fs) && fs.size == as_supported_profile.track_size;
        /* the Track 1 BIN of a split image; never the audio track */
        rank = track1 ? 20 : (strstr(name, "(Track 2)") || strstr(name, "(track 2)")) ? 0 : 10;
    } else if (image_like(name)) rank = 5;
    if (rank > s->rank || (rank && rank == s->rank && strcmp(full, s->best) < 0)) {
        s->rank = rank;
        snprintf(s->best, sizeof s->best, "%s", full);
    }
    return 0;
}

static int scan_disk(Discovery *d) {
    char dir[PC_ROOT_PATH_MAX + 32];
    DiskScan s;
    PcStat st;
    disk_dir(dir, sizeof dir);
    memset(&s, 0, sizeof s);
    s.dir = dir;
    if (pc_fs_stat(dir, &st) || !st.is_dir) { d->disk_state = DISK_MISSING; return 0; }
    pc_fs_listdir(dir, scan_disk_entry, &s);
    snprintf(d->listing, sizeof d->listing, "%s", s.listing);
    if (!s.files) { d->disk_state = DISK_EMPTY; return 0; }
    if (!s.rank) { d->disk_state = DISK_NOTHING_USABLE; return 0; }
    d->disk_state = DISK_FOUND;
    snprintf(d->path, sizeof d->path, "%s", s.best);
    return 1;
}

typedef struct { char best[PC_ROOT_PATH_MAX + 300]; int rank; const char *dir; int depth; } Hunt;

static int hunt_entry(const char *name, const PcStat *st, void *ud) {
    Hunt *h = (Hunt *)ud;
    char full[PC_ROOT_PATH_MAX + 300];
    path_join(full, sizeof full, h->dir, name);
    if (st->is_dir && !st->is_link) {
        if (h->depth == 0 && name[0] != '.' && strcmp(name, "data") && strcmp(name, "assets") &&
            strcmp(name, "assets-en") && strcmp(name, "saves") && strncmp(name, "data.damaged", 12) &&
            strncmp(name, "assets.damaged", 14)) {
            Hunt sub = *h;
            sub.dir = full;
            sub.depth = 1;
            sub.best[0] = 0;
            sub.rank = 0;
            pc_fs_listdir(full, hunt_entry, &sub);
            if (sub.rank > h->rank) { h->rank = sub.rank; snprintf(h->best, sizeof h->best, "%s", sub.best); }
        }
        return 0;
    }
    /* size first, never a hash: a .cue whose Track 1 has the size, or a
     * .bin of exactly that size */
    if (ends_with_ci(name, ".cue") && st->size < 65536 && as_quick_candidate(full, NULL)) {
        if (h->rank < 2) { h->rank = 2; snprintf(h->best, sizeof h->best, "%s", full); }
    } else if (ends_with_ci(name, ".bin") && st->size == as_supported_profile.track_size) {
        if (h->rank < 1) { h->rank = 1; snprintf(h->best, sizeof h->best, "%s", full); }
    }
    return 0;
}

static int hunt(const char *dir, char *out, size_t n) {
    Hunt h;
    memset(&h, 0, sizeof h);
    h.dir = dir;
    pc_fs_listdir(dir, hunt_entry, &h);
    if (!h.rank) return 0;
    snprintf(out, n, "%s", h.best);
    return 1;
}

static const char *g_arg_disc, *g_arg_audio;

static void discover(Discovery *d) {
    char dirs[6][PC_ROOT_PATH_MAX + 64];
    int nd = 0, i;
    const char *home;
    memset(d, 0, sizeof *d);
    if (scan_disk(d)) { d->where = FOUND_DISK; return; }
    if (g_arg_disc) {
        snprintf(d->path, sizeof d->path, "%s", g_arg_disc);
        d->where = FOUND_ARG;
        return;
    }
    snprintf(dirs[nd++], sizeof dirs[0], "%s", root()->exe_dir);
    dir_of(root()->exe_dir, dirs[nd], sizeof dirs[0]);
    nd++;
#ifdef _WIN32
    home = pc_fs_getenv("USERPROFILE");
#else
    home = getenv("HOME");
#endif
    if (home && *home) {
        char h[PC_ROOT_PATH_MAX];
        snprintf(h, sizeof h, "%s", home);
        path_join(dirs[nd++], sizeof dirs[0], h, "Downloads");
        path_join(dirs[nd++], sizeof dirs[0], h, "Documents");
        path_join(dirs[nd++], sizeof dirs[0], h, "Desktop");
    }
    for (i = 0; i < nd; i++)
        if (hunt(dirs[i], d->path, sizeof d->path)) { d->where = FOUND_ELSEWHERE; return; }
    d->where = FOUND_NONE;
}

/* ------------------------------------------------------------------ */
/* drawing                                                             */
/* ------------------------------------------------------------------ */

static SDL_Renderer *g_ren;

/* The screen is drawn in output pixels at a whole scale u (crisp at any
 * window size): the canvas is the window divided by u, u the largest that
 * keeps it at least CANVAS_W x CANVAS_H units. */
#define CANVAS_W 480
#define CANVAS_H 360
#define MARGIN 8                /* the window panel inside the canvas */
#define INSET 18                /* text inside the panel */
#define LINE UI_LINE
#define TITLE_PINK 0xF096BEFFu  /* the logo's pink */

typedef struct { int ox, oy, u, w, h; } Canvas;
static Canvas g_cv;

static void fill(int x, int y, int w, int h, unsigned rgba) {
    ui_fill(g_ren, g_cv.ox + x * g_cv.u, g_cv.oy + y * g_cv.u, w * g_cv.u, h * g_cv.u, rgba);
}

/* latin12 at k x the canvas unit (k = 2: the big lines) */
static int text(int x, int y, int k, unsigned rgba, const char *t) {
    return ui_draw_text(g_ren, g_cv.ox + x * g_cv.u, g_cv.oy + y * g_cv.u, g_cv.u * k, rgba,
                        UI_SHADOW, t) * k;
}

/* Word-wrapped text from y, LINE apart and PARA_GAP more after each '\n'
 * (the body's paragraphs), at most `max` lines and never below `bottom`;
 * returns the y after it. */
#define PARA_GAP 4
static int para(int x, int y, int w, unsigned rgba, int max, int bottom, const char *s) {
    UiLine l[64];
    char buf[512];
    int n = ui_text_wrap(s, w, l, max < 64 ? max : 64), i;
    for (i = 0; i < n && y + LINE <= bottom; i++) {
        const char *end = l[i].s + l[i].n;
        snprintf(buf, sizeof buf, "%.*s", l[i].n, l[i].s);
        text(x, y, 1, rgba, buf);
        y += LINE;
        while (*end == ' ') end++;
        if (*end == '\n') y += PARA_GAP;
    }
    return y;
}

/* The canvas for the window as it is now. */
static void canvas_setup(void) {
    int w = CANVAS_W, h = CANVAS_H, u;
    if (SDL_GetRendererOutputSize(g_ren, &w, &h) || w <= 0 || h <= 0) { w = CANVAS_W; h = CANVAS_H; }
    u = w / CANVAS_W < h / CANVAS_H ? w / CANVAS_W : h / CANVAS_H;
    if (u < 1) u = 1;
    g_cv.u = u;
    g_cv.w = w / u;
    g_cv.h = h / u;
    g_cv.ox = (w - g_cv.w * u) / 2;
    g_cv.oy = (h - g_cv.h * u) / 2;
}

/* The game's backdrop: a check of mint and teal squares. */
static void backdrop(void) {
    int x, y;
    ui_fill(g_ren, 0, 0, g_cv.w * g_cv.u + 2 * g_cv.ox, g_cv.h * g_cv.u + 2 * g_cv.oy, 0x28C8A0FFu);
    for (y = 0; y < g_cv.h; y += 16)
        for (x = (y / 16) % 2 * 16; x < g_cv.w; x += 32) fill(x, y, 16, 16, 0xA0E0A0FFu);
}


/* ------------------------------------------------------------------ */
/* screen model                                                        */
/* ------------------------------------------------------------------ */

enum { ST_FIND, ST_ASK, ST_WORK, ST_ERROR, ST_NODISC, ST_DAMAGED, ST_DONE };

typedef struct {
    int state;
    char title[96];
    char body[4096];            /* paragraphs, '\n' separated */
    char keys[200];             /* the key line */
    int can_continue;           /* C: go on without Track 2 */
    int can_reextract;          /* ENTER: move the damaged folder aside and extract */
    char image[PC_ROOT_PATH_MAX + 300];
    char audio[PC_ROOT_PATH_MAX + 300];
    int allow_no_audio;
} Screen;

static Screen g_s;
static AsCtl g_ctl;
static AsError g_err;
static SDL_atomic_t g_job_done;
static int g_job_rc;
static SDL_Thread *g_thread;
static char g_dest[PC_ROOT_PATH_MAX + 32];

static void addf(char *buf, size_t n, const char *fmt, ...) {
    size_t len = strlen(buf);
    va_list ap;
    if (len >= n - 1) return;
    va_start(ap, fmt);
    vsnprintf(buf + len, n - len, fmt, ap);
    va_end(ap);
}

static void log_hint(char *body, size_t n) {
    const char *lp = pc_log_path();
    if (lp) {
        char p[PC_ROOT_PATH_MAX + 64];
        snprintf(p, sizeof p, "%s", lp);
        native(p);
        addf(body, n, "\nDetails are in %s", p);
    }
}

static const char *phase_name(int ph) {
    switch (ph) {
    case AS_PH_HASH: return "Checking Track 1 (SHA1 of 702 MB)";
    case AS_PH_HASH_AUDIO: return "Checking Track 2 (SHA1 of 45 MB)";
    case AS_PH_CHECK: return "Checking the disc's files and the game program (SHA1)";
    case AS_PH_EXTRACT: return "Extracting the game data";
    case AS_PH_VERIFY: return "Verifying every extracted file (SHA-256)";
    case AS_PH_DONE: return "Done";
    default: return "Starting";
    }
}

static void expected_disc(char *b, size_t n) {
    char sz[32], asz[32];
    fmt_bytes(as_supported_profile.track_size, sz, sizeof sz);
    fmt_bytes(as_supported_profile.audio_size, asz, sizeof asz);
    addf(b, n, "\nNeeded: %s, %s, as a raw BIN/CUE:"
               "\n  Track 1 .bin  %s bytes  SHA1 %s"
               "\n  Track 2 .bin  %s bytes  SHA1 %s",
         AS_RELEASE_NAME, AS_SERIAL, sz, as_supported_profile.track_sha1, asz,
         as_supported_profile.audio_sha1);
}

typedef struct { char *buf; size_t n; int count; } ListCtx;

static int list_files(const char *name, const PcStat *st, void *ud) {
    ListCtx *l = (ListCtx *)ud;
    if (st->is_dir || l->count >= 8) return 0;
    addf(l->buf, l->n, "\n  %s", name);
    l->count++;
    return 0;
}

/* The message for a failed setup: what, which file, expected, found, do. */
static void describe(const AsError *e, const char *image) {
    char *b = g_s.body;
    size_t n = sizeof g_s.body;
    char p[PC_ROOT_PATH_MAX + 64], img[PC_ROOT_PATH_MAX + 300];
    snprintf(p, sizeof p, "%s", e->path);
    native(p);
    snprintf(img, sizeof img, "%s", image);
    native(img);
    g_s.body[0] = 0;
    g_s.can_continue = g_s.can_reextract = 0;
    snprintf(g_s.keys, sizeof g_s.keys, "Enter: Choose a disc image   R: Try again   Esc: Quit");
    switch (e->code) {
    case AS_E_NOT_FOUND:
        if (ends_with_ci(image, ".cue")) {
            char names[8][256], dir[PC_ROOT_PATH_MAX + 300];
            int k, nn = as_cue_files(image, names, 8);
            ListCtx l;
            snprintf(g_s.title, sizeof g_s.title, "A file the .cue needs is missing");
            dir_of(image, dir, sizeof dir);
            addf(b, n, "The .cue file %s names files that must be in the same folder:", base_name(img));
            for (k = 0; k < nn; k++) {
                char full[PC_ROOT_PATH_MAX + 600];
                PcStat st;
                path_join(full, sizeof full, dir, names[k]);
                addf(b, n, "\n  %s  -- %s", names[k], pc_fs_stat(full, &st) ? "Missing" : "Found");
            }
            addf(b, n, "\nThe folder holds:");
            memset(&l, 0, sizeof l);
            l.buf = b; l.n = n;
            pc_fs_listdir(dir, list_files, &l);
            addf(b, n, "\nRename the .bin files to exactly the names above (or correct the FILE lines "
                       "in the .cue), then press R.");
        } else {
            snprintf(g_s.title, sizeof g_s.title, "The disc image was not found");
            addf(b, n, "Not found: %s", p);
        }
        break;
    case AS_E_UNREADABLE:
        snprintf(g_s.title, sizeof g_s.title, "The disc image cannot be read");
        addf(b, n, "%s\nCheck that the file is not open in another program and that you may read it, "
                   "then press R.", e->message);
        break;
    case AS_E_CUE:
        snprintf(g_s.title, sizeof g_s.title, "The .cue file cannot be used");
        addf(b, n, "%s: %s\nExpected a cue sheet like:\n  FILE \"...(Track 1).bin\" BINARY\n"
                   "    TRACK 01 MODE2/2352\n      INDEX 01 00:00:00\n  FILE \"...(Track 2).bin\" BINARY\n"
                   "    TRACK 02 AUDIO\n      INDEX 00 00:00:00\n      INDEX 01 00:02:00\n"
                   "Use the .cue that came with your dump.", img, e->message);
        break;
    case AS_E_COOKED_ISO:
        snprintf(g_s.title, sizeof g_s.title, "This is a cooked ISO, not a raw BIN/CUE");
        addf(b, n, "%s has 2048-byte sectors: the voice, music and movie sectors of the disc are "
                   "lost in that format, so the game cannot be built from it.\nDump the disc again "
                   "as a raw BIN/CUE (2352-byte sectors, both tracks) and put the .cue and the .bin "
                   "files in the disk folder.", img);
        expected_disc(b, n);
        break;
    case AS_E_FORMAT:
        snprintf(g_s.title, sizeof g_s.title, "%s images are not supported", e->found[0] ? e->found : "These");
        addf(b, n, "%s\n", img);
        if (!strcmp(e->found, "CHD"))
            addf(b, n, "Convert it back to BIN/CUE with MAME's chdman:\n  chdman extractcd -i game.chd -o game.cue\n"
                       "Then put the .cue and the .bin files in the disk folder.");
        else if (!strcmp(e->found, "ECM"))
            addf(b, n, "Unpack it with unecm (it gives back the .bin), keep the .cue, and put both in the disk folder.");
        else if (!strcmp(e->found, "PBP"))
            addf(b, n, "A PBP (PSP EBOOT) is recompressed and cannot be turned back into the original disc. "
                       "Use a raw BIN/CUE dump of your disc.");
        else if (strstr(e->found, "ARCHIVE"))
            addf(b, n, "Extract the archive first, then put the .cue and the .bin files in the disk folder.");
        else
            addf(b, n, "Use a raw BIN/CUE dump of your disc (2352-byte sectors, both tracks).");
        expected_disc(b, n);
        break;
    case AS_E_AUDIO_TRACK_GIVEN:
        snprintf(g_s.title, sizeof g_s.title, "This is the audio track (Track 2)");
        addf(b, n, "%s is the CD audio track. Choose the .cue (or the Track 1 .bin) instead.", img);
        expected_disc(b, n);
        break;
    case AS_E_WRONG_SIZE: {
        char f[32], x[32];
        fmt_bytes(strtoll(e->found, NULL, 10), f, sizeof f);
        fmt_bytes(strtoll(e->expected, NULL, 10), x, sizeof x);
        if (strtoll(e->found, NULL, 10) < 16 * 1024 * 1024 && !ends_with_ci(image, ".bin") &&
            !ends_with_ci(image, ".cue")) {
            snprintf(g_s.title, sizeof g_s.title, "This is not a disc image");
            addf(b, n, "File: %s (%s bytes)\nChoose the .cue file of your disc image (or its Track 1 .bin).", p, f);
            expected_disc(b, n);
            break;
        }
        snprintf(g_s.title, sizeof g_s.title, "Track 1 has the wrong size");
        addf(b, n, "File: %s\nFound %s bytes, expected %s bytes.\n%s", p, f, x,
             strtoll(e->found, NULL, 10) < as_supported_profile.track_size
                 ? "A smaller file is usually an incomplete download or copy: copy it again."
                 : "A different size is another release or a modified image.");
        expected_disc(b, n);
        break;
    }
    case AS_E_WRONG_SHA1:
        snprintf(g_s.title, sizeof g_s.title, "This is not the supported release");
        addf(b, n, "File: %s\nTrack 1 SHA1 found:    %s\nTrack 1 SHA1 expected: %s\n"
                   "The size is right but the contents differ: another revision (e.g. Rev 0), a patched or "
                   "translated image, or a damaged dump. Only the release below is supported.",
             p, e->found, e->expected);
        expected_disc(b, n);
        break;
    case AS_E_NO_TRACK2:
        snprintf(g_s.title, sizeof g_s.title, "Track 2 (CD audio) is missing");
        addf(b, n, "Expected: %s\nTrack 1 is fine. Track 2 holds one CD audio track that the game never "
                   "plays, so you can go on without it (C). To have a complete copy, add the Track 2 .bin "
                   "next to the .cue and press R.", p);
        g_s.can_continue = 1;
        snprintf(g_s.keys, sizeof g_s.keys, "C: Continue without Track 2   R: Try again   Enter: Choose   Esc: Quit");
        break;
    case AS_E_TRACK2:
        snprintf(g_s.title, sizeof g_s.title, "Track 2 (CD audio) differs");
        addf(b, n, "File: %s\n%s\nFound:    %s\nExpected: %s\nTrack 1 is fine. The game never plays Track 2, "
                   "so you can go on without it (C), or replace it with a good copy and press R.",
             p, e->message, e->found, e->expected);
        g_s.can_continue = 2;
        snprintf(g_s.keys, sizeof g_s.keys, "C: Continue without Track 2   R: Try again   Enter: Choose   Esc: Quit");
        break;
    case AS_E_LAYOUT:
        snprintf(g_s.title, sizeof g_s.title, "The disc's contents fail a check");
        addf(b, n, "File: %s\n%s\nThe image may be damaged or modified. Use a good dump of the supported "
                   "release.", p, e->message);
        expected_disc(b, n);
        break;
    case AS_E_DISK_SPACE:
        snprintf(g_s.title, sizeof g_s.title, "Not enough free disk space");
        addf(b, n, "The game data goes to %s and needs about %s; that drive has %s free.\nFree some space "
                   "(or move the game folder to another drive) and press R.", p, e->expected, e->found);
        break;
    case AS_E_WRITE:
        snprintf(g_s.title, sizeof g_s.title, "The game data cannot be written");
        addf(b, n, "%s\nThe folder must be writable. Move the game folder somewhere you own (e.g. your "
                   "Desktop or Documents, not Program Files), or create an empty file named user-data.txt "
                   "next to the game to keep its data in your user folder. Then press R.", e->message);
        break;
    case AS_E_DEST_INVALID:
    case AS_E_MANIFEST:
    case AS_E_CORRUPT:
        snprintf(g_s.title, sizeof g_s.title, "The game data is damaged or incomplete");
        addf(b, n, "%s", e->message);
        if (e->expected[0]) addf(b, n, "\nExpected: %s\nFound:    %s", e->expected, e->found);
        addf(b, n, "\nPress Enter to extract it again from your disc. The damaged files are moved aside "
                   "(into data.damaged-N), not deleted; you can delete that folder afterwards.");
        g_s.can_reextract = 1;
        snprintf(g_s.keys, sizeof g_s.keys, "Enter: Extract again   Esc: Quit");
        break;
    case AS_E_LOCKED:
        snprintf(g_s.title, sizeof g_s.title, "Another setup is running");
        addf(b, n, "Another copy of the game is preparing its data right now (it holds %s). Wait until it "
                   "has finished or close it, then press R.", p);
        break;
    case AS_E_CHANGED:
        snprintf(g_s.title, sizeof g_s.title, "The disc image changed while being read");
        addf(b, n, "%s was modified during the setup. Wait until it is completely copied, then press R.", p);
        break;
    case AS_E_CANCELLED:
        snprintf(g_s.title, sizeof g_s.title, "Setup cancelled");
        addf(b, n, "Nothing was changed. Press R to start again.");
        break;
    default:
        snprintf(g_s.title, sizeof g_s.title, "Setup failed");
        addf(b, n, "%s", e->message);
        break;
    }
    log_hint(b, n);
}

/* Every change of screen bumps the version: the screen is drawn (and, for
 * the tests, shot) before scripted input or the ready pause acts on it. */
static int g_state_version, g_drawn_since_change;
static void set_state(int st) { g_s.state = st; g_state_version++; g_drawn_since_change = 0; }

static int g_shot_seq;
static int g_last_drawn_state = -1;

static void shot(const char *tag) {
    const char *dir = getenv("TOKI_SETUP_SHOTS");
    SDL_Surface *s;
    int w = 0, h = 0;
    char path[1200];
    if (!dir || !*dir || !g_ren) return;
    if (SDL_GetRendererOutputSize(g_ren, &w, &h) || w <= 0) return;
    s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 24, SDL_PIXELFORMAT_RGB24);
    if (!s) return;
    if (SDL_RenderReadPixels(g_ren, NULL, SDL_PIXELFORMAT_RGB24, s->pixels, s->pitch) == 0) {
        snprintf(path, sizeof path, "%s/%02d-%s.bmp", dir, g_shot_seq++, tag);
        SDL_SaveBMP(s, path);
        pc_logf("[SETUP] screenshot %s", path);
    }
    SDL_FreeSurface(s);
}

static const char *state_tag(void) {
    static char tag[64];
    const char *t = "screen";
    size_t i;
    switch (g_s.state) {
    case ST_ASK: t = "found-elsewhere"; break;
    case ST_WORK: t = "progress"; break;
    case ST_NODISC: t = "no-disc"; break;
    case ST_DONE: t = "ready"; break;
    case ST_DAMAGED: t = "damaged"; break;
    case ST_ERROR: t = "error"; break;
    }
    snprintf(tag, sizeof tag, "%s", t);
    if (g_s.state == ST_ERROR) {
        size_t k = strlen(tag);
        tag[k++] = '-';
        for (i = 0; g_s.title[i] && k < sizeof tag - 1; i++)
            tag[k++] = isalnum((unsigned char)g_s.title[i]) ? (char)tolower((unsigned char)g_s.title[i]) : '_';
        tag[k] = 0;
    }
    return tag;
}

static void draw(void) {
    int y, x, w, bottom, keys_y, k;
    canvas_setup();
    SDL_RenderSetLogicalSize(g_ren, 0, 0);
    SDL_RenderSetScale(g_ren, 1.0f, 1.0f);
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
    SDL_RenderClear(g_ren);
    backdrop();
    /* the game's message window, nearly the whole screen */
    ui_draw_panel(g_ren, g_cv.ox + MARGIN * g_cv.u, g_cv.oy + MARGIN * g_cv.u, g_cv.w - 2 * MARGIN,
                  g_cv.h - 2 * MARGIN, g_cv.u);
    x = INSET;
    w = g_cv.w - 2 * INSET;
    y = INSET;
    text(x, y - 2, 2, TITLE_PINK, "Heartthrob Memorial");
    y += 2 * LINE;
    text(x, y, 1, UI_C_DIM, "PC Enhanced  \xE2\x99\xA5  First-time setup");
    y += LINE + 3;
    fill(x, y, w, 1, UI_C_RULE);
    y += 6;

    /* the keys sit on the bottom edge, under a rule */
    {
        UiLine l[3];
        int n = ui_text_wrap(g_s.keys, w, l, 2);
        keys_y = g_cv.h - INSET - n * LINE;
        bottom = keys_y - 6;
        fill(x, bottom + 2, w, 1, UI_C_RULE);
        para(x, keys_y, w, UI_C_KEY, 2, g_cv.h, g_s.keys);
    }
    if (g_s.state == ST_WORK) bottom -= 3 * LINE + 12;     /* room for the progress */

    if (root()->fallback) {
        char p[PC_ROOT_PATH_MAX + 64];
        snprintf(p, sizeof p, "The game's folder is read-only, so its data goes to %s", root()->root);
        native(p);
        y = para(x, y, w, UI_C_WARN, 3, bottom, p) + 4;
    }
    /* the state's title: big when it fits */
    k = ui_text_width(g_s.title) * 2 <= w ? 2 : 1;
    text(x, y, k, UI_C_HEAD, g_s.title);
    y += k * LINE + 6;
    para(x, y, w, UI_C_TEXT, 64, bottom, g_s.body);

    if (g_s.state == ST_WORK) {
        int64_t done = g_ctl.done, total = g_ctl.total > 0 ? g_ctl.total : 1;
        int pct = (int)(done * 100 / total);
        char line[96];
        if (pct > 100) pct = 100;
        y = bottom + 6;
        text(x, y, 1, UI_C_TEXT, phase_name(g_ctl.phase));
        y += LINE + 2;
        /* the menu's bar: copper frame, dark trough, the logo's pink */
        fill(x, y, w, 12, UI_C_RULE);
        fill(x + 1, y + 1, w - 2, 10, UI_C_TROUGH);
        fill(x + 1, y + 1, (int)((int64_t)(w - 2) * pct / 100), 10, TITLE_PINK);
        y += 14;
        snprintf(line, sizeof line, "%d%%   %lld / %lld MB", pct, (long long)(done >> 20),
                 (long long)(total >> 20));
        text(x, y, 1, UI_C_DIM, line);
    }
    SDL_RenderPresent(g_ren);
    if (g_last_drawn_state != g_state_version) {
        g_last_drawn_state = g_state_version;
        g_drawn_since_change = 0;
    }
    g_drawn_since_change++;
    if (g_drawn_since_change == 2) shot(state_tag());
}

/* ------------------------------------------------------------------ */
/* the work                                                            */
/* ------------------------------------------------------------------ */

static int worker(void *ud) {
    (void)ud;
    g_job_rc = as_prepare(g_s.image, g_s.audio[0] ? g_s.audio : NULL, g_dest, NULL, &g_ctl, &g_err);
    SDL_AtomicSet(&g_job_done, 1);
    return 0;
}

static void start(const char *image_in) {
    char shown[PC_ROOT_PATH_MAX + 300], image[PC_ROOT_PATH_MAX + 300];
    snprintf(image, sizeof image, "%s", image_in);      /* image_in may be g_s.image */
    snprintf(g_s.image, sizeof g_s.image, "%s", image);
    snprintf(shown, sizeof shown, "%s", image);
    native(shown);
    memset(&g_ctl, 0, sizeof g_ctl);
    memset(&g_err, 0, sizeof g_err);
    g_ctl.allow_no_audio = g_s.allow_no_audio;
    assets_dest(g_dest, sizeof g_dest);
    snprintf(g_s.title, sizeof g_s.title, "Preparing the game data");
    g_s.body[0] = 0;
    addf(g_s.body, sizeof g_s.body, "Disc image: %s\n", shown);
    {
        char d[PC_ROOT_PATH_MAX + 64];
        snprintf(d, sizeof d, "%s", g_dest);
        native(d);
        addf(g_s.body, sizeof g_s.body, "Game data:  %s\nThis takes a minute. It happens only once: later "
                                        "launches start the game directly.", d);
    }
    snprintf(g_s.keys, sizeof g_s.keys, "Esc: Cancel");
    pc_logf("[SETUP] preparing from %s%s into %s", image,
            g_s.allow_no_audio ? " (without Track 2)" : "", g_dest);
    SDL_AtomicSet(&g_job_done, 0);
    set_state(ST_WORK);
    g_thread = SDL_CreateThread(worker, "asset-setup", NULL);
    if (!g_thread) worker(NULL);
}

static void log_disc_result(void) {
    const AsProfile *p = &as_supported_profile;
    if (g_ctl.track1_path[0]) pc_logf("[SETUP] track 1: %s", g_ctl.track1_path);
    if (g_ctl.track_sha1[0])
        pc_logf("[SETUP] track 1 SHA1 %s (expected %s) %s", g_ctl.track_sha1, p->track_sha1,
                strcmp(g_ctl.track_sha1, p->track_sha1) ? "MISMATCH" : "OK");
    if (g_ctl.audio_path[0]) pc_logf("[SETUP] track 2: %s", g_ctl.audio_path);
    if (g_ctl.audio_sha1[0])
        pc_logf("[SETUP] track 2 SHA1 %s (expected %s) %s", g_ctl.audio_sha1, p->audio_sha1,
                strcmp(g_ctl.audio_sha1, p->audio_sha1) ? "MISMATCH" : "OK");
    if (g_ctl.exe_sha1[0])
        pc_logf("[SETUP] PSX.EXE SHA1 %s (expected %s) %s", g_ctl.exe_sha1, p->exe_sha1,
                strcmp(g_ctl.exe_sha1, p->exe_sha1) ? "MISMATCH" : "OK");
}

static void finish_job(void) {
    if (g_thread) { SDL_WaitThread(g_thread, NULL); g_thread = NULL; }
    log_disc_result();
    if (g_job_rc == AS_OK) {
        pc_logf("[SETUP] game data ready: %s (%d files verified by SHA-256%s%s)", g_dest, g_ctl.files,
                g_ctl.anomalies ? "; the disc's one known XA subheader anomaly preserved" : "",
                g_ctl.audio_omitted ? "; Track 2 left out" : "");
        snprintf(g_s.title, sizeof g_s.title, "The game data is ready");
        g_s.body[0] = 0;
        addf(g_s.body, sizeof g_s.body, "%d files extracted and verified.%s\nStarting the game...",
             g_ctl.files, g_ctl.audio_omitted ? " (Without the unused CD audio track.)" : "");
        g_s.keys[0] = 0;
        set_state(ST_DONE);
        return;
    }
    pc_logf("[SETUP] FAILED (code %d): %s%s%s", g_err.code, g_err.message,
            g_err.path[0] ? " -- file: " : "", g_err.path);
    if (g_err.expected[0]) pc_logf("[SETUP]   expected %s, found %s", g_err.expected, g_err.found);
    describe(&g_err, g_s.image);
    set_state(ST_ERROR);
}

/* ------------------------------------------------------------------ */
/* screens                                                             */
/* ------------------------------------------------------------------ */

static void show_nodisc(const Discovery *d) {
    char disk[PC_ROOT_PATH_MAX + 64];
    disk_dir(disk, sizeof disk);
    native(disk);
    g_s.body[0] = 0;
    g_s.can_continue = g_s.can_reextract = 0;
    switch (d->disk_state) {
    case DISK_MISSING:
        snprintf(g_s.title, sizeof g_s.title, "Put your disc image in the disk folder");
        addf(g_s.body, sizeof g_s.body, "There is no folder %s yet.\nCreate a folder named disk next to the "
                                        "game and put your .cue and both .bin files in it, then press R.", disk);
        break;
    case DISK_EMPTY:
        snprintf(g_s.title, sizeof g_s.title, "The disk folder is empty");
        addf(g_s.body, sizeof g_s.body, "Put your .cue and both .bin files in %s, then press R.", disk);
        break;
    default:
        snprintf(g_s.title, sizeof g_s.title, "No disc image in the disk folder");
        addf(g_s.body, sizeof g_s.body, "%s holds: %s\nNone of these is a .cue or .bin. Put your .cue and "
                                        "both .bin files there, then press R.", disk, d->listing);
        break;
    }
    addf(g_s.body, sizeof g_s.body, "\nOr drag your .cue file onto this window, or press Enter to browse for it.");
    expected_disc(g_s.body, sizeof g_s.body);
    addf(g_s.body, sizeof g_s.body, "\n  (File names as in the Redump set: \"%s.cue\", \"... (Track 1).bin\", "
                                    "\"... (Track 2).bin\")", AS_RELEASE_NAME);
    log_hint(g_s.body, sizeof g_s.body);
    snprintf(g_s.keys, sizeof g_s.keys, "Enter: Browse   R: Look again   Drag and drop: Use that file   Esc: Quit");
    set_state(ST_NODISC);
}

static void show_ask(const char *path) {
    char p[PC_ROOT_PATH_MAX + 300], disk[PC_ROOT_PATH_MAX + 64];
    snprintf(g_s.image, sizeof g_s.image, "%s", path);
    snprintf(p, sizeof p, "%s", path);
    native(p);
    disk_dir(disk, sizeof disk);
    native(disk);
    snprintf(g_s.title, sizeof g_s.title, "Found your disc image");
    g_s.body[0] = 0;
    addf(g_s.body, sizeof g_s.body, "%s\nIt is not in the disk folder (%s), but it can be used where it is: "
                                    "the disc is only needed this once. Press Enter to use it.", p, disk);
    log_hint(g_s.body, sizeof g_s.body);
    snprintf(g_s.keys, sizeof g_s.keys, "Enter: Use it   B: Browse for another   Esc: Quit");
    set_state(ST_ASK);
}

static void find_and_go(void) {
    Discovery d;
    discover(&d);
    switch (d.where) {
    case FOUND_DISK:
        pc_logf("[SETUP] disc image in disk/: %s", d.path);
        start(d.path);
        break;
    case FOUND_ARG:
        pc_logf("[SETUP] disc image from --disc: %s", d.path);
        start(d.path);
        break;
    case FOUND_ELSEWHERE:
        pc_logf("[SETUP] disk/ has no usable image (%s); found a disc image of the right size at %s",
                d.disk_state == DISK_MISSING ? "no disk folder" : d.disk_state == DISK_EMPTY ? "empty" : d.listing,
                d.path);
        show_ask(d.path);
        break;
    default:
        pc_logf("[SETUP] no disc image found (disk/: %s)",
                d.disk_state == DISK_MISSING ? "missing" : d.disk_state == DISK_EMPTY ? "empty" : d.listing);
        show_nodisc(&d);
        break;
    }
}

static void show_damaged(void) {
    describe(&g_verify_err, "");
    set_state(ST_DAMAGED);
}

/* Move a damaged / unmanaged data folder's contents aside (never delete
 * them): into <data>.damaged-N, all but the program's own entries (the
 * open log, en/: as_owned_entry), so the folder itself stays. */
typedef struct { const char *from, *to; int failed; char why[PC_ROOT_PATH_MAX + 400]; } Aside;

static int aside_entry(const char *name, const PcStat *st, void *ud) {
    Aside *a = (Aside *)ud;
    char f[PC_ROOT_PATH_MAX + 300], t[PC_ROOT_PATH_MAX + 300];
    (void)st;
    if (as_owned_entry(name)) return 0;
    path_join(f, sizeof f, a->from, name);
    path_join(t, sizeof t, a->to, name);
    if (pc_fs_rename(f, t)) {
        a->failed = 1;
        snprintf(a->why, sizeof a->why, "cannot move %s to %s: %s", f, t, strerror(errno));
        return 1;
    }
    return 0;
}

static int move_aside(void) {
    char dest[PC_ROOT_PATH_MAX + 32], aside[PC_ROOT_PATH_MAX + 64];
    PcStat st;
    Aside a;
    int k;
    assets_dest(dest, sizeof dest);
    if (pc_fs_stat(dest, &st)) return 0;
    for (k = 1; k < 100; k++) {
        snprintf(aside, sizeof aside, "%s.damaged-%d", dest, k);
        if (pc_fs_stat(aside, &st)) break;
    }
    memset(&a, 0, sizeof a);
    a.from = dest;
    a.to = aside;
    if (!st.is_dir) {
        /* not a folder at all: the whole thing */
        if (pc_fs_rename(dest, aside)) {
            a.failed = 1;
            snprintf(a.why, sizeof a.why, "cannot rename %s to %s: %s", dest, aside, strerror(errno));
        }
    } else if (pc_fs_mkdir(aside)) {
        a.failed = 1;
        snprintf(a.why, sizeof a.why, "cannot create %s: %s", aside, strerror(errno));
    } else {
        pc_fs_listdir(dest, aside_entry, &a);
    }
    if (a.failed) {
        memset(&g_err, 0, sizeof g_err);
        g_err.code = AS_E_WRITE;
        snprintf(g_err.message, sizeof g_err.message, "%s", a.why);
        snprintf(g_err.path, sizeof g_err.path, "%s", dest);
        pc_logf("[SETUP] %s", g_err.message);
        describe(&g_err, "");
        set_state(ST_ERROR);
        return -1;
    }
    pc_logf("[SETUP] moved the damaged game data aside: %s", aside);
    return 0;
}

/* ------------------------------------------------------------------ */
/* the file picker                                                     */
/* ------------------------------------------------------------------ */

#ifndef _WIN32
static int on_path(const char *prog) {
    const char *path = getenv("PATH");
    char buf[1200];
    while (path && *path) {
        size_t n = strcspn(path, ":");
        snprintf(buf, sizeof buf, "%.*s/%s", (int)n, path, prog);
        if (access(buf, X_OK) == 0) return 1;
        path += n;
        if (*path) path++;
    }
    return 0;
}
#endif

static int browse(char *out, size_t n) {
    char start_dir[PC_ROOT_PATH_MAX + 64];
    disk_dir(start_dir, sizeof start_dir);
#ifdef _WIN32
    {
        wchar_t file[4096] = L"", dir[2048];
        OPENFILENAMEW ofn;
        memset(&ofn, 0, sizeof ofn);
        MultiByteToWideChar(CP_UTF8, 0, start_dir, -1, dir, 2048);
        ofn.lStructSize = sizeof ofn;
        ofn.lpstrFilter = L"Disc image (*.cue; *.bin)\0*.cue;*.bin\0All files\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = 4096;
        ofn.lpstrInitialDir = dir;
        ofn.lpstrTitle = L"Choose your Tokimeki Memorial disc image (.cue)";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&ofn)) return 0;
        return WideCharToMultiByte(CP_UTF8, 0, file, -1, out, (int)n, NULL, NULL) > 0;
    }
#else
    {
        char cmd[1600];
        FILE *p;
        size_t len;
        PcStat st;
        if (pc_fs_stat(start_dir, &st)) snprintf(start_dir, sizeof start_dir, "%s", root()->exe_dir);
        if (on_path("zenity"))
            snprintf(cmd, sizeof cmd, "zenity --file-selection --title='Choose your disc image (.cue)' "
                                      "--file-filter='Disc image | *.cue *.bin *.CUE *.BIN' --filename='%s/' 2>/dev/null",
                     start_dir);
        else if (on_path("kdialog"))
            snprintf(cmd, sizeof cmd, "kdialog --getopenfilename '%s' 'Disc image (*.cue *.bin)' 2>/dev/null", start_dir);
        else {
            pc_logf("[SETUP] no file picker (zenity or kdialog) installed: drag the .cue onto the window");
            snprintf(g_s.keys, sizeof g_s.keys, "No file picker on this system: drag the .cue onto this window   Esc: Quit");
            return 0;
        }
        if (strchr(start_dir, '\'')) return 0;
        p = popen(cmd, "r");
        if (!p) return 0;
        if (!fgets(out, (int)n, p)) out[0] = 0;
        pclose(p);
        len = strlen(out);
        while (len && (out[len - 1] == '\n' || out[len - 1] == '\r')) out[--len] = 0;
        return len > 0;
    }
#endif
}

/* ------------------------------------------------------------------ */
/* the loop                                                            */
/* ------------------------------------------------------------------ */

static const char *g_script;

/* One injected input item, or 0 when none is due. */
static int script_next(char *kind, size_t kn, char *arg, size_t an) {
    const char *end, *colon;
    if (!g_script || !*g_script) return 0;
    end = strchr(g_script, ';');
    if (!end) end = g_script + strlen(g_script);
    colon = memchr(g_script, ':', (size_t)(end - g_script));
    if (colon) {
        snprintf(kind, kn, "%.*s", (int)(colon - g_script), g_script);
        snprintf(arg, an, "%.*s", (int)(end - colon - 1), colon + 1);
    } else {
        snprintf(kind, kn, "%.*s", (int)(end - g_script), g_script);
        arg[0] = 0;
    }
    g_script = *end ? end + 1 : end;
    return 1;
}

static void on_drop(const char *file) {
    pc_logf("[SETUP] dropped: %s", file);
    g_s.allow_no_audio = 0;
    g_s.audio[0] = 0;
    start(file);
}

/* 1 = quit */
static int on_key(SDL_Keycode k) {
    char picked[PC_ROOT_PATH_MAX + 300];
    if (g_s.state == ST_WORK) {
        if (k == SDLK_ESCAPE) g_ctl.cancel = 1;
        return 0;
    }
    if (k == SDLK_ESCAPE) return 1;
    switch (g_s.state) {
    case ST_ASK:
        if (k == SDLK_RETURN || k == SDLK_KP_ENTER) { start(g_s.image); return 0; }
        if (k == SDLK_b && browse(picked, sizeof picked)) on_drop(picked);
        return 0;
    case ST_DAMAGED:
    case ST_ERROR:
        if (g_s.can_reextract && (k == SDLK_RETURN || k == SDLK_KP_ENTER)) {
            if (move_aside() == 0) { g_s.allow_no_audio = 0; find_and_go(); }
            return 0;
        }
        if (g_s.can_continue && k == SDLK_c) {
            g_s.allow_no_audio = g_s.can_continue;
            pc_logf("[SETUP] the player chose to continue without Track 2");
            start(g_s.image);
            return 0;
        }
        if (k == SDLK_r) {
            g_s.allow_no_audio = 0;
            if (g_s.state == ST_ERROR && g_s.image[0] && g_err.code != AS_E_NOT_FOUND) start(g_s.image);
            else find_and_go();
            return 0;
        }
        if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
            if (browse(picked, sizeof picked)) on_drop(picked);
        }
        return 0;
    case ST_NODISC:
        if (k == SDLK_r) { find_and_go(); return 0; }
        if ((k == SDLK_RETURN || k == SDLK_KP_ENTER) && browse(picked, sizeof picked)) on_drop(picked);
        return 0;
    }
    return 0;
}

static SDL_Keycode key_named(const char *n) {
    if (!strcmp(n, "return") || !strcmp(n, "enter")) return SDLK_RETURN;
    if (!strcmp(n, "escape") || !strcmp(n, "esc")) return SDLK_ESCAPE;
    if (strlen(n) == 1) return (SDL_Keycode)tolower((unsigned char)n[0]);
    return SDLK_UNKNOWN;
}

const char *pc_setup_run(SDL_Window *win, SDL_Renderer *ren) {
    int lw = 0, lh = 0, quit = 0, headless;
    Uint32 done_at = 0;
    const char *drv = SDL_GetCurrentVideoDriver();
    (void)win;
    g_ren = ren;
    g_script = getenv("TOKI_SETUP_SCRIPT");
    headless = !ren || (drv && (!strcmp(drv, "dummy") || !strcmp(drv, "offscreen")));
    log_root();
    if (ren) {
        SDL_RenderGetLogicalSize(ren, &lw, &lh);
        SDL_RenderSetLogicalSize(ren, 0, 0);     /* draw() works in output pixels */
    }
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    memset(&g_s, 0, sizeof g_s);
    assets_dest(g_dest, sizeof g_dest);
    if (g_damaged) show_damaged();
    else find_and_go();
    for (;;) {
        SDL_Event e;
        if (ren) draw();
        if (g_s.state == ST_WORK && SDL_AtomicGet(&g_job_done)) finish_job();
        if (quit) break;
        if (g_s.state != ST_WORK && g_drawn_since_change >= 2) {
            /* the test input, on every screen the player can act on --
             * "the game data is ready" too, before it moves on */
            char kind[32], arg[1200];
            if (script_next(kind, sizeof kind, arg, sizeof arg)) {
                pc_logf("[SETUP] test input: %s %s", kind, arg);
                if (!strcmp(kind, "drop")) { if (g_s.state != ST_DONE) on_drop(arg); }
                else if (!strcmp(kind, "key")) quit = on_key(key_named(arg));
                else if (!strcmp(kind, "quit")) quit = 1;
                continue;
            }
        }
        if (g_s.state == ST_DONE) {
            if (!done_at) done_at = SDL_GetTicks();
            if (g_drawn_since_change >= 2 && SDL_GetTicks() - done_at > (headless ? 0u : 1200u)) break;
        }
        if (g_s.state != ST_WORK && g_s.state != ST_DONE && g_drawn_since_change >= 2) {
            if (headless && !ren) {
                /* no window at all: nothing more can happen */
                fprintf(stderr, "%s\n%s\n", g_s.title, g_s.body);
                quit = 1;
                continue;
            }
            if (headless && (!g_script || !*g_script) && g_s.state != ST_WORK) {
                pc_logf("[SETUP] no window to ask in (SDL_VIDEODRIVER=%s): %s", drv ? drv : "?", g_s.title);
                fprintf(stderr, "Setup: %s\n%s\n", g_s.title, g_s.body);
                quit = 1;
                continue;
            }
        }
        if (SDL_WaitEventTimeout(&e, g_s.state == ST_WORK ? 50 : 100)) {
            do {
                if (e.type == SDL_QUIT) {
                    if (g_s.state == ST_WORK) g_ctl.cancel = 1;
                    quit = 1;
                } else if (e.type == SDL_DROPFILE) {
                    if (g_s.state != ST_WORK && g_s.state != ST_DONE) on_drop(e.drop.file);
                    SDL_free(e.drop.file);
                } else if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                    if (on_key(e.key.keysym.sym)) quit = 1;
                }
            } while (SDL_PollEvent(&e));
        }
        if (quit && g_s.state == ST_WORK) {
            g_ctl.cancel = 1;
            if (g_thread) { SDL_WaitThread(g_thread, NULL); g_thread = NULL; }
            pc_logf("[SETUP] cancelled; the partial work was removed");
            break;
        }
    }
    if (ren) {
        SDL_RenderSetLogicalSize(ren, lw, lh);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_RenderPresent(ren);
    }
    if (g_s.state != ST_DONE) {
        pc_logf("[SETUP] closed before the game data was ready");
        return NULL;
    }
    if (quit) {
        /* the close button or Esc on "the game data is ready": the player
         * asked to leave, as on every other screen of the setup (Esc is
         * "close" here, there is nothing to go back to).  The data stays:
         * the next launch starts the game straight away. */
        pc_logf("[SETUP] closed on \"the game data is ready\"; the data is kept for the next launch");
        return NULL;
    }
    pc_data_root_set_player(1);
    return g_dest;
}

/* ------------------------------------------------------------------ */
/* --check and the menu's "Verify game data"                           */
/* ------------------------------------------------------------------ */

/* The full report: every extracted file by SHA-256 against the manifest,
 * and the disc (when one is found) by its Track 1 / Track 2 / PSX.EXE
 * SHA1s.  Lines go to the log (and stdout).  Returns 0 when all is well. */
static int full_check(const char *asset_dir, AsCtl *ctl, char *summary, size_t sn) {
    AsError e;
    Discovery d;
    int bad = 0, rc;
    char disc[PC_ROOT_PATH_MAX + 300] = "";
    memset(&e, 0, sizeof e);
    pc_logf("[CHECK] game data: %s", asset_dir ? asset_dir : "(none)");
    if (asset_dir) {
        ctl->fast_verify = 0;
        rc = as_verify(asset_dir, NULL, ctl, &e);
        if (rc) {
            bad = 1;
            pc_logf("[CHECK] game data: FAILED: %s%s%s", e.message, e.expected[0] ? " -- expected " : "",
                    e.expected[0] ? e.expected : "");
            if (e.found[0]) pc_logf("[CHECK]   found %s", e.found);
            snprintf(summary, sn, "Game data damaged: %s", e.message);
        } else {
            pc_logf("[CHECK] game data: OK, %d files match the manifest (SHA-256), PSX.EXE SHA1 %s",
                    ctl->files, as_supported_profile.exe_sha1);
            snprintf(summary, sn, "Game data OK: %d files", ctl->files);
        }
    }
    discover(&d);
    if (d.where == FOUND_DISK || d.where == FOUND_ARG || d.where == FOUND_ELSEWHERE) snprintf(disc, sizeof disc, "%s", d.path);
    if (!disc[0]) {
        pc_logf("[CHECK] disc image: none found (disk/ %s); it is only needed to extract the game data",
                d.disk_state == DISK_MISSING ? "missing" : d.disk_state == DISK_EMPTY ? "empty" : "has no image");
        addf(summary, sn, "; no disc image to check");
        return bad;
    }
    {
        AsCtl c;
        memset(&c, 0, sizeof c);
        memset(&e, 0, sizeof e);
        pc_logf("[CHECK] disc image: %s", disc);
        rc = as_check_image(disc, NULL, NULL, &c, &e);
        if (c.track_sha1[0])
            pc_logf("[CHECK] track 1 SHA1 %s (expected %s)", c.track_sha1, as_supported_profile.track_sha1);
        if (c.audio_sha1[0])
            pc_logf("[CHECK] track 2 SHA1 %s (expected %s)", c.audio_sha1, as_supported_profile.audio_sha1);
        if (c.exe_sha1[0])
            pc_logf("[CHECK] PSX.EXE SHA1 %s (expected %s)", c.exe_sha1, as_supported_profile.exe_sha1);
        if (rc) {
            bad = 1;
            pc_logf("[CHECK] disc image: FAILED: %s", e.message);
            addf(summary, sn, "; disc image failed: %s", e.message);
        } else {
            pc_logf("[CHECK] disc image: OK (%s, %s)", AS_RELEASE_NAME, AS_SERIAL);
            addf(summary, sn, "; disc image OK");
        }
    }
    return bad;
}

int pc_setup_cli(const PcSetupOpts *o, int argc, char **argv, int *status) {
    if (o->cli) { *status = as_cli(argc, argv); return 1; }
    if (o->licenses) {
        fputs(pc_licenses(), stdout);
        *status = 0;
        return 1;
    }
    g_arg_disc = o->disc;
    g_arg_audio = o->audio;
    if (o->check) {
        AsCtl c;
        char summary[1024] = "";
        const char *dir = pc_setup_choose(argc > 1 ? argv[1] : NULL, o);
        char dest[PC_ROOT_PATH_MAX + 32];
        log_root();
        if (!dir && g_damaged) { assets_dest(dest, sizeof dest); dir = dest; }
        memset(&c, 0, sizeof c);
        *status = full_check(dir, &c, summary, sizeof summary) ? 1 : 0;
        if (!dir) { pc_logf("[CHECK] no game data yet: run the game once to prepare it"); *status = 1; }
        pc_logf("[CHECK] %s: %s", *status ? "PROBLEMS FOUND" : "ALL OK", summary);
        return 1;
    }
    return 0;
}

/* ---- the menu page ---- */

static SDL_Thread *g_vthread;
static AsCtl g_vctl;
static SDL_atomic_t g_vdone;
static int g_vrc = -1;
static char g_vsummary[1024];
static char g_vnote[160];

static int verify_worker(void *ud) {
    (void)ud;
    g_vrc = full_check(pc_asset_dir(), &g_vctl, g_vsummary, sizeof g_vsummary);
    SDL_AtomicSet(&g_vdone, 1);
    return 0;
}

static void verify_start(void *ctx) {
    (void)ctx;
    if (g_vthread) return;
    memset(&g_vctl, 0, sizeof g_vctl);
    g_vsummary[0] = 0;
    SDL_AtomicSet(&g_vdone, 0);
    g_vrc = -1;
    pc_logf("[CHECK] requested from the settings menu");
    g_vthread = SDL_CreateThread(verify_worker, "verify", NULL);
}

static const char *verify_note(void *ctx) {
    (void)ctx;
    if (g_vthread && SDL_AtomicGet(&g_vdone)) {
        SDL_WaitThread(g_vthread, NULL);
        g_vthread = NULL;
    }
    if (g_vthread) {
        int64_t t = g_vctl.total > 0 ? g_vctl.total : 1;
        snprintf(g_vnote, sizeof g_vnote, "Checking %d%%", (int)(g_vctl.done * 100 / t));
    } else if (g_vrc < 0) {
        snprintf(g_vnote, sizeof g_vnote, "SHA-256 of every file");
    } else {
        snprintf(g_vnote, sizeof g_vnote, "%s%s", g_vrc ? "Problems: " : "", g_vsummary);
        if (g_vrc && pc_log_path()) snprintf(g_vnote + strlen(g_vnote), sizeof g_vnote - strlen(g_vnote), " (see the log)");
    }
    return g_vnote;
}

static int g_lic_top;

#define LIC_MAX 4000            /* wrapped lines (the texts are ~900 lines) */

static void lic_enter(void *ctx) { (void)ctx; g_lic_top = 0; }

static int lic_input(void *ctx, int in) {
    (void)ctx;
    if (in == PC_MENU_UP) { if (g_lic_top > 0) g_lic_top -= 1; return 1; }
    if (in == PC_MENU_DOWN) { g_lic_top += 1; return 1; }
    if (in == PC_MENU_LEFT) { g_lic_top = g_lic_top > 14 ? g_lic_top - 14 : 0; return 1; }
    if (in == PC_MENU_RIGHT) { g_lic_top += 14; return 1; }
    return 0;
}

/* The licence texts as they are (mixed case), wrapped once to the page. */
/* A text line that is only a rule of '=' or '-'. */
static int rule_line(const char *s, int n) {
    int i;
    if (n < 4) return 0;
    for (i = 0; i < n; i++)
        if (s[i] != '=' && s[i] != '-') return 0;
    return 1;
}

/* The licence texts reflowed for a proportional font: a line joins the
 * next when both are prose (not empty, not indented, not a rule), so a
 * paragraph wraps as one; tables (indented lines) and rules keep their
 * lines.  Allocated once. */
static char *lic_reflow(void) {
    const char *src = pc_licenses();
    size_t n = strlen(src), i;
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, src, n + 1);
    for (i = 0; i < n; i++) {
        const char *line, *next;
        size_t ln;
        if (out[i] != '\n') continue;
        for (line = out + i; line > out && line[-1] != '\n'; line--) { }
        ln = (size_t)(out + i - line);
        next = out + i + 1;
        if (!ln || line[0] == ' ' || rule_line(line, (int)ln)) continue;
        if (!*next || *next == '\n' || *next == ' ') continue;
        {
            size_t nn = strcspn(next, "\n");
            if (rule_line(next, (int)nn)) continue;
        }
        out[i] = ' ';
    }
    return out;
}

static void lic_draw(void *ctx, const PcMenuCanvas *c) {
    static UiLine *lines;
    static char *text;
    static int n = -1, wrapped_w;
    int rows, i;
    char buf[512];
    (void)ctx;
    if (n < 0 || wrapped_w != c->w) {
        if (!lines) lines = (UiLine *)malloc(sizeof(*lines) * LIC_MAX);
        if (!text) text = lic_reflow();
        n = lines && text ? ui_text_wrap(text, c->w, lines, LIC_MAX) : 0;
        wrapped_w = c->w;
    }
    rows = c->h / UI_ROW_TIGHT;
    if (g_lic_top > n - rows) g_lic_top = n - rows > 0 ? n - rows : 0;
    for (i = 0; i < rows && g_lic_top + i < n; i++) {
        const UiLine *l = &lines[g_lic_top + i];
        if (rule_line(l->s, l->n)) {            /* a rule of '=': the copper line */
            c->fill(c->ctx, 0, i * UI_ROW_TIGHT + 5, c->w, 1, UI_C_RULE);
            continue;
        }
        snprintf(buf, sizeof buf, "%.*s", l->n, l->s);
        c->text(c->ctx, 0, i * UI_ROW_TIGHT, UI_C_TEXT, buf);
    }
}

static const char *lic_help(void *ctx, int line) {
    (void)ctx;
    return line == 0 ? "\xE2\x86\x91\xE2\x86\x93 Scroll   \xE2\x86\x90\xE2\x86\x92 Page" : "Esc Back";
}

void pc_setup_menu_register(void) {
    PcMenuItem it;
    PcMenuCustom ops;
    int page, lic;
    memset(&ops, 0, sizeof ops);
    ops.enter = lic_enter;
    ops.input = lic_input;
    ops.draw = lic_draw;
    ops.help = lic_help;
    page = pc_menu_page("Game data and about");
    lic = pc_menu_custom_page("About / licences", &ops);
    if (page < 0 || lic < 0) {
        fprintf(stderr, "[SETUP] the settings menu has no room for the game-data page\n");
        return;
    }
    memset(&it, 0, sizeof it);
    it.type = PC_MENU_PAGE;
    it.label = "Game data and about";
    it.target = page;
    if (pc_menu_add(&it) < 0) return;
    memset(&it, 0, sizeof it);
    it.page = page;
    it.type = PC_MENU_ACTION;
    it.label = "Verify game data";
    it.activate = verify_start;
    it.note = verify_note;
    pc_menu_add(&it);
    memset(&it, 0, sizeof it);
    it.page = page;
    it.type = PC_MENU_PAGE;
    it.label = "About / licences";
    it.target = lic;
    pc_menu_add(&it);
}
