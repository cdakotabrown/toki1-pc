/* SPDX-License-Identifier: GPL-3.0-or-later */
/* data_root.c -- the data root, the asset root and the player log
 * (pc/include/pc_data_root.h has the rules). */
#define PC_DATA_ROOT_IMPL
#include "pc_data_root.h"
#include "pc_settings.h"
#include "pc_fs.h"
#include "asset_setup.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define pc_isatty(fd) 0          /* the GUI build has no console of its own */
#define pc_dup2 _dup2
#define pc_fileno _fileno
#define PC_NULL_DEVICE "NUL"
#else
#include <unistd.h>
#define pc_isatty(fd) isatty(fd)
#define pc_dup2 dup2
#define pc_fileno fileno
#define PC_NULL_DEVICE "/dev/null"
#endif

#ifndef PC_PORTABLE_DEFAULT
#  ifdef _WIN32
#    define PC_PORTABLE_DEFAULT 1
#  else
#    define PC_PORTABLE_DEFAULT 0
#  endif
#endif

static PcDataRoot g_root;
static int g_root_done;
static char g_asset_root[PC_ROOT_PATH_MAX] = "./pc/assets";
static int g_player;

static int exists(const char *dir, const char *name) {
    char p[PC_ROOT_PATH_MAX + 64];
    PcStat st;
    snprintf(p, sizeof p, "%s/%s", dir, name);
    return pc_fs_stat(p, &st) == 0;
}

static int writable(const char *dir) {
    char p[PC_ROOT_PATH_MAX + 64];
    FILE *f;
    snprintf(p, sizeof p, "%s/.tokimeki-pc-write-test", dir);
    pc_fs_unlink(p);
    f = pc_fs_create_excl(p);
    if (!f) return 0;
    fclose(f);
    pc_fs_unlink(p);
    return 1;
}

static const char *env_get(const char *name) { return pc_fs_getenv(name); }

const PcDataRoot *pc_data_root(void) {
    const char *v;
    int want;
    if (g_root_done) return &g_root;
    g_root_done = 1;
    if (pc_fs_exe_dir(g_root.exe_dir, sizeof g_root.exe_dir)) snprintf(g_root.exe_dir, sizeof g_root.exe_dir, ".");
    if ((v = pc_fs_getenv("TOKI_CONFIG_DIR")) && *v) {
        g_root.mode = PC_ROOT_ENV;
        snprintf(g_root.root, sizeof g_root.root, "%s", v);
        snprintf(g_root.why, sizeof g_root.why, "TOKI_CONFIG_DIR");
        snprintf(pc_portable_root, sizeof pc_portable_root, "%s", v);
        return &g_root;
    }
    want = PC_PORTABLE_DEFAULT;
    snprintf(g_root.why, sizeof g_root.why, "%s build default",
             PC_PORTABLE_DEFAULT ? "portable" : "per-user");
    if (exists(g_root.exe_dir, "portable.txt")) { want = 1; snprintf(g_root.why, sizeof g_root.why, "portable.txt"); }
    if (exists(g_root.exe_dir, "user-data.txt")) { want = 0; snprintf(g_root.why, sizeof g_root.why, "user-data.txt"); }
    if ((v = pc_fs_getenv("TOKI_PORTABLE")) && *v) {
        want = strcmp(v, "0") != 0;
        snprintf(g_root.why, sizeof g_root.why, "TOKI_PORTABLE=%s", v);
    }
    if (want) {
        if (writable(g_root.exe_dir)) {
            g_root.mode = PC_ROOT_PORTABLE;
            snprintf(g_root.root, sizeof g_root.root, "%s", g_root.exe_dir);
            snprintf(pc_portable_root, sizeof pc_portable_root, "%s", g_root.exe_dir);
            return &g_root;
        }
        g_root.fallback = 1;
    }
    {
        static PcSettings s;      /* large; the settings module's own path rule */
        pc_portable_root[0] = 0;
        pc_settings_paths(&s, env_get);
        g_root.mode = PC_ROOT_USER;
        snprintf(g_root.root, sizeof g_root.root, "%s", s.data_dir);
    }
    return &g_root;
}

const char *pc_asset_root(void) { return g_asset_root; }

void pc_asset_root_set(const char *dir) {
    size_t n;
    snprintf(g_asset_root, sizeof g_asset_root, "%s", dir);
    n = strlen(g_asset_root);
    while (n > 1 && (g_asset_root[n - 1] == '/' || g_asset_root[n - 1] == '\\')) g_asset_root[--n] = 0;
}


void pc_data_root_set_player(int on) { g_player = on; }
int pc_data_root_player(void) { return g_player; }

int pc_player_quiet(void) {
    return g_player && !pc_settings_harness_reason(env_get);
}

/* ---- the player log ---- */

static FILE *g_log;
static int g_redirected;
static char g_log_path[PC_ROOT_PATH_MAX + 32];
/* lines logged before the log is open (the data-root and game-data
 * decisions come first); written at the top of the log when it opens */
static char g_pending[16384];
static size_t g_pending_n;

int pc_log_open(const char *path, int redirect) {
    char old[PC_ROOT_PATH_MAX + 40];
    PcStat st;
    snprintf(g_log_path, sizeof g_log_path, "%s", path);
    snprintf(old, sizeof old, "%s.old", path);
    if (!pc_fs_stat(path, &st)) {
        pc_fs_unlink(old);
        pc_fs_rename(path, old);
    }
    if (redirect && !pc_isatty(1)) {
        fflush(stdout);
        fflush(stderr);
        if (pc_fs_freopen(path, "w", stdout)) {
            pc_dup2(pc_fileno(stdout), 2);
            g_redirected = 1;
            g_log = stdout;
            if (g_pending_n) { fwrite(g_pending, 1, g_pending_n, g_log); fflush(g_log); }
            return 0;
        }
    }
    g_log = pc_fs_fopen(path, "w");
    if (g_log && g_pending_n) { fwrite(g_pending, 1, g_pending_n, g_log); fflush(g_log); }
    return g_log ? 0 : -1;
}

const char *pc_log_path(void) { return g_log ? g_log_path : NULL; }

void pc_logf(const char *fmt, ...) {
    va_list ap;
    if (!g_log && g_pending_n < sizeof g_pending - 2) {
        int k;
        va_start(ap, fmt);
        k = vsnprintf(g_pending + g_pending_n, sizeof g_pending - g_pending_n - 1, fmt, ap);
        va_end(ap);
        if (k > 0) {
            g_pending_n += (size_t)k < sizeof g_pending - g_pending_n - 2 ? (size_t)k : sizeof g_pending - g_pending_n - 2;
            g_pending[g_pending_n++] = '\n';
        }
    }
    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fputc('\n', g_log);
        fflush(g_log);
    }
    if (!g_redirected) {
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        putchar('\n');
        fflush(stdout);
    }
}

void pc_log_frame(void) {
    static unsigned n;
    long pos;
    if (!g_redirected || ++n % 600) return;
    pos = ftell(stdout);
    if (pos > 64L * 1024 * 1024) {
        fprintf(stdout, "\n[LOG] log reached 64 MB; the rest of this run is not logged\n");
        fflush(stdout);
        freopen(PC_NULL_DEVICE, "w", stdout);
        pc_dup2(pc_fileno(stdout), 2);
        g_redirected = 0;
        g_log = NULL;
    }
}

/* ---- the data folder and the old layout ---- */

static int g_legacy;            /* the old <root>/assets is still the game data */

static int is_dir(const char *p) {
    PcStat st;
    return pc_fs_stat(p, &st) == 0 && st.is_dir;
}

static int has_manifest(const char *dir) { return exists(dir, AS_MANIFEST); }

const char *pc_data_dir(void) {
    static char dir[PC_ROOT_PATH_MAX + 16];
    snprintf(dir, sizeof dir, "%s/%s", pc_data_root()->root, g_legacy ? "assets" : PC_DATA_DIR);
    return dir;
}

int pc_data_layout_legacy(void) { return g_legacy; }

typedef struct {
    const char *from, *to;
    int moved, failed;
    char first[PC_ROOT_PATH_MAX + 300];
} Merge;

static int merge_entry(const char *name, const PcStat *st, void *ud) {
    Merge *m = (Merge *)ud;
    char a[PC_ROOT_PATH_MAX + 300], b[PC_ROOT_PATH_MAX + 300];
    PcStat t;
    (void)st;
    if (!strcmp(name, AS_MANIFEST)) return 0;           /* last, by the caller */
    snprintf(a, sizeof a, "%s/%s", m->from, name);
    snprintf(b, sizeof b, "%s/%s", m->to, name);
    if (pc_fs_stat(b, &t) == 0) return 0;               /* data/ has its own */
    if (pc_fs_rename(a, b)) {
        if (!m->failed++) snprintf(m->first, sizeof m->first, "%s: %s", a, strerror(errno));
    } else {
        m->moved++;
    }
    return 0;
}

/* Every entry of `from` that `to` lacks moves into `to`, the manifest last
 * (until it is there, `to` is not game data).  0 when all moved. */
static int merge_into(const char *from, const char *to, Merge *m) {
    char a[PC_ROOT_PATH_MAX + 64], b[PC_ROOT_PATH_MAX + 64];
    memset(m, 0, sizeof *m);
    m->from = from;
    m->to = to;
    pc_fs_listdir(from, merge_entry, m);
    if (m->failed) return -1;
    snprintf(a, sizeof a, "%s/%s", from, AS_MANIFEST);
    snprintf(b, sizeof b, "%s/%s", to, AS_MANIFEST);
    if (pc_fs_rename(a, b)) {
        m->failed++;
        snprintf(m->first, sizeof m->first, "%s: %s", a, strerror(errno));
        return -1;
    }
    pc_fs_rmdir(from);                                  /* empty now, or kept */
    return 0;
}

void pc_data_layout_migrate(void) {
    const PcDataRoot *r = pc_data_root();
    char data[PC_ROOT_PATH_MAX + 16], assets[PC_ROOT_PATH_MAX + 16], en_old[PC_ROOT_PATH_MAX + 16];
    char en_new[PC_ROOT_PATH_MAX + 32], p[PC_ROOT_PATH_MAX + 64];
    snprintf(data, sizeof data, "%s/%s", r->root, PC_DATA_DIR);
    snprintf(assets, sizeof assets, "%s/assets", r->root);
    snprintf(en_old, sizeof en_old, "%s/assets-en", r->root);
    snprintf(en_new, sizeof en_new, "%s/%s", data, PC_DATA_EN);
    g_legacy = 0;
    /* the setup lock folder of the old layout, left by a crashed extraction
     * (the setup's lock is a locked file now, gone when it ends) */
    snprintf(p, sizeof p, "%s/.assets.prepare.lock", r->root);
    if (is_dir(p)) {
        char o[PC_ROOT_PATH_MAX + 96];
        snprintf(o, sizeof o, "%s/owner.json", p);
        pc_fs_unlink(o);
        if (pc_fs_rmdir(p) == 0) pc_logf("[DATA] removed a stale setup lock: %s", p);
    }
    if (is_dir(assets) && has_manifest(assets) && !has_manifest(data)) {
        if (!is_dir(data) && pc_fs_rename(assets, data) == 0) {
            pc_logf("[DATA] moved %s to %s (the folder layout of 2026-11-07)", assets, data);
        } else {
            /* data/ exists without game data (a log, English files): move
             * the contents instead; until the manifest has moved, the old
             * folder stays the game data and the next start goes on */
            Merge m;
            pc_fs_mkdirs(data);
            if (merge_into(assets, data, &m) == 0) {
                pc_logf("[DATA] moved the contents of %s into %s", assets, data);
            } else {
                g_legacy = 1;
                pc_logf("[DATA] cannot move %s to %s (%s); using %s as it is", assets, data,
                        m.first, assets);
            }
        }
    } else if (is_dir(assets) && has_manifest(assets) && has_manifest(data)) {
        pc_logf("[DATA] %s and %s both exist; using %s (the other is left as it is)", assets, data, data);
    }
    /* the log lived in the root: the last one joins the new place (where
     * this run's log turns it into tokimeki-pc.log.old) */
    {
        char lo[PC_ROOT_PATH_MAX + 32], ln[PC_ROOT_PATH_MAX + 64];
        PcStat st;
        snprintf(lo, sizeof lo, "%s/tokimeki-pc.log.old", r->root);
        pc_fs_unlink(lo);
        snprintf(lo, sizeof lo, "%s/tokimeki-pc.log", r->root);
        if (pc_fs_stat(lo, &st) == 0 && !st.is_dir) {
            snprintf(ln, sizeof ln, "%s/tokimeki-pc.log", pc_data_dir());
            pc_fs_mkdirs(pc_data_dir());
            if (pc_fs_stat(ln, &st) == 0 || pc_fs_rename(lo, ln)) pc_fs_unlink(lo);
        }
    }
    if (!g_legacy && is_dir(en_old) && is_dir(data)) {
        if (is_dir(en_new)) {
            pc_logf("[DATA] %s and %s both exist; using %s (the other is left as it is)", en_old,
                    en_new, en_new);
        } else if (pc_fs_rename(en_old, en_new) == 0) {
            pc_logf("[DATA] moved %s to %s", en_old, en_new);
            /* the lock file the old version left there (nothing has
             * started a preparation yet in this run) */
            snprintf(p, sizeof p, "%s/.prepare.lock", en_new);
            pc_fs_unlink(p);
        } else {
            pc_logf("[DATA] cannot move %s to %s (%s); the English files are read from %s", en_old,
                    en_new, strerror(errno), en_old);
        }
    }
}
