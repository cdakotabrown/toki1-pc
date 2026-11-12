/* SPDX-License-Identifier: GPL-3.0-or-later */
/* quicksave.c -- the player's save states (PC layer): quicksave slots,
 * named saves, autosaves and the resume state (pc/hal/quicksave.h).
 *
 * A save is a debug-toolkit save state (pc/hal/dbg_state.c: PS1 RAM, the
 * hardware-register window, the executable's device models and the game
 * stack, taken at the main-loop boundary) written into the player's saves
 * folder (pc/hal/save_paths.c) as an ENTRY of three files, with a
 * thumbnail and a small index the slot list and the save browser
 * (pc/hal/save_browser.c) read without opening the state:
 *
 *   <saves>/quicksave/ID.tks   the state (format: dbg_state.c header)
 *   <saves>/quicksave/ID.png   160x120 thumbnail of the displayed picture
 *   <saves>/quicksave/ID.ini   the index: [slot] id/kind/state/thumbnail/
 *                              saved/build_id/profile/scene..., [game] the
 *                              in-game date and stats when a game is in
 *                              progress (docs/pc-port/ENHANCED-MODE.md)
 *
 * Ids (quicksave.h): slot1..slot9 the quicksave slots, slot0 the automatic
 * undo (written before every load, so an accidental load can be undone),
 * save0001.. the browser's unlimited saves, auto1..auto9 the weekly
 * autosaves, ending, resume (pc/hal/save_auto.c).  A load keeps the live
 * memory-card image (see card_hold below).
 *
 * Nothing here runs unless the player asks (a quicksave hotkey, the save
 * browser), an automatic save is due (save_auto.c, never in automated
 * runs) or a toolkit command asks (`qsave` / `qload`, pc/hal/dbg_cmds.c).
 * A request waits for the next main-loop boundary, where qs_boundary() acts
 * on it: the same rule as every save state, so a save never lands with
 * game code on the stack.  No game memory is read except to describe the
 * entry (date, stats), and none is written except by a load itself.
 *
 * Writing.  At the boundary the state is CAPTURED (copies of the images,
 * dbg_state_capture), the thumbnail is reduced and the index text built;
 * a worker thread then encodes and writes the three files, each through a
 * temporary file and a rename, and the result toast is shown at a later
 * boundary.  The game runs on meanwhile: a save costs the frame only the
 * copy (measured: docs/pc-port/ENHANCED-MODE.md "Save write time").
 * Listings, loads, deletes and quitting wait for the writer first.
 *
 * Every static here describes the host session (the selected slot, pending
 * requests, the writer), so dbg_state.c keeps this file out of a restore
 * (KEEP_FILES).
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include "types.h"
#include "dbg_toolkit.h"
#include "gpu_display.h"
#include "pc_present.h"
#include "pc_settings.h"
#include "profile.h"
#include "quicksave.h"

extern s32 frame_counter;
extern u8 now_step, now_sub_step, now_sub_sub_step;
extern u16 g_vram[512][1024];
extern u32 hal_clock_vblanks(void);
extern const char *hal_overlay_loaded(void);
extern const char *pc_save_dir(void);
extern void *dbg_mem_alloc(unsigned long n);
extern void dbg_mem_free(void *p);
extern int dbg_write_file(const char *path, const void *data, unsigned long n);
extern void dbg_toolkit_activate(void);
/* pc/hal/present.c; weak so a harness without the presenter still links */
extern void pc_present_toast(const char *text) __attribute__((weak));
extern void pc_present_osd(const char *text, unsigned ms) __attribute__((weak));
extern void pc_present_note(const char *text, unsigned ms) __attribute__((weak));
/* pc/hal/memcard.c: a card request, directory pass or file transfer is in
 * flight (weak: harnesses without the card) */
extern int memcard_busy(void) __attribute__((weak));
/* pc/hal/save_auto.c; weak for the same reason */
extern void sa_boundary(void) __attribute__((weak));

#define QS_SLOTS 10          /* 0 = automatic "before the last quickload" */
#define QS_BUSY_LIMIT 600    /* boundaries a deferred save keeps retrying */
#define QS_CARD_LIMIT 600    /* qs_boundary calls a request waits for the memory card */

static int g_slot = 1;               /* the selected slot, 1..9 */
static char g_req_save[QS_ID_MAX], g_req_load[QS_ID_MAX];
static int g_busy_tries;
static int g_card_waits;             /* qs_boundary calls waited for the card */
static int g_last_result;            /* 0 ok, else failed (tests read it) */

/* ------------------------------------------------------------------ */
/* ids                                                                 */
/* ------------------------------------------------------------------ */

int qs_id_kind(const char *id, int *number) {
    int n = 0, k;
    const char *d = NULL;
    if (number) *number = 0;
    if (!id) return QS_KIND_NONE;
    if (!strcmp(id, "resume")) return QS_KIND_RESUME;
    if (!strcmp(id, "ending")) return QS_KIND_ENDING;
    if (!strncmp(id, "slot", 4) && strlen(id) == 5) { d = id + 4; k = QS_KIND_QUICK; }
    else if (!strncmp(id, "auto", 4) && strlen(id) == 5) { d = id + 4; k = QS_KIND_AUTO; }
    else if (!strncmp(id, "save", 4) && strlen(id) == 8) { d = id + 4; k = QS_KIND_SAVE; }
    else return QS_KIND_NONE;
    for (; *d; d++) {
        if (*d < '0' || *d > '9') return QS_KIND_NONE;
        n = n * 10 + (*d - '0');
    }
    if (k == QS_KIND_QUICK && n == 0) k = QS_KIND_UNDO;
    if ((k == QS_KIND_AUTO || k == QS_KIND_SAVE) && n == 0) return QS_KIND_NONE;
    if (number) *number = n;
    return k;
}

/* A save's name inside a sentence: "Saved slot 3" (a static copy). */
static const char *mid(const char *name) {
    static char buf[32];
    snprintf(buf, sizeof buf, "%s", name);
    buf[0] = (char)tolower((unsigned char)buf[0]);
    return buf;
}

/* What the player reads: "Slot 3", "Save 12", "Autosave 2", ... */
void qs_id_name(const char *id, char *out, size_t cap) {
    int n, k = qs_id_kind(id, &n);
    switch (k) {
    case QS_KIND_QUICK: case QS_KIND_UNDO: snprintf(out, cap, "Slot %d", n); break;
    case QS_KIND_SAVE: snprintf(out, cap, "Save %d", n); break;
    case QS_KIND_AUTO: snprintf(out, cap, "Autosave %d", n); break;
    case QS_KIND_ENDING: snprintf(out, cap, "Ending save"); break;
    case QS_KIND_RESUME: snprintf(out, cap, "Resume save"); break;
    default: snprintf(out, cap, "%s", id ? id : "?"); break;
    }
}

/* the console log's name: "slot 3" (as before), else the id */
static void log_name(const char *id, char *out, size_t cap) {
    int n, k = qs_id_kind(id, &n);
    if (k == QS_KIND_QUICK || k == QS_KIND_UNDO) snprintf(out, cap, "slot %d", n);
    else snprintf(out, cap, "%s", id);
}

static const char *kind_word(int k) {
    switch (k) {
    case QS_KIND_QUICK: return "quick";
    case QS_KIND_UNDO: return "undo";
    case QS_KIND_SAVE: return "save";
    case QS_KIND_AUTO: return "auto";
    case QS_KIND_ENDING: return "ending";
    case QS_KIND_RESUME: return "resume";
    default: return "?";
    }
}

/* ------------------------------------------------------------------ */
/* paths, files                                                        */
/* ------------------------------------------------------------------ */

static void entry_path(char *out, size_t n, const char *id, const char *ext) {
    snprintf(out, n, "%s/quicksave/%s.%s", pc_save_dir(), id, ext);
}

static int ensure_dir(void) {
    char d[PC_SETTINGS_PATH_MAX + 32];
    snprintf(d, sizeof d, "%s/quicksave", pc_save_dir());
    return pc_settings_mkdirs(d, 1);
}

/* the whole (small) file in buf, NUL-terminated; 0 when unreadable */
static int read_small(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    size_t n = 0;
    if (fd < 0) return 0;
    while (n + 1 < cap) {
        ssize_t r = read(fd, buf + n, cap - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    close(fd);
    buf[n] = 0;
    return 1;
}

/* KEY's value in INI text (any section; the index keys are unique) */
static int ini_get(const char *text, const char *key, char *out, size_t cap) {
    size_t kl = strlen(key);
    const char *p = text;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len >= kl + 3 && !strncmp(p, key, kl) && !strncmp(p + kl, " = ", 3)) {
            size_t vl = len - kl - 3;
            if (vl >= cap) vl = cap - 1;
            memcpy(out, p + kl + 3, vl);
            out[vl] = 0;
            return 1;
        }
        p = nl ? nl + 1 : NULL;
    }
    return 0;
}

static int ini_int(const char *text, const char *key, int dflt) {
    char v[32];
    return ini_get(text, key, v, sizeof v) ? atoi(v) : dflt;
}

/* Results are toasts ([system] toasts); a failure or refusal is a note, which
 * shows whatever that setting says. */
static void qs_note(const char *text);
void qs_notify(const char *text) {
    static const char *const bad[] = { "fail", "not ", "empty", "Nothing", "Slot 0 is", NULL };
    int i;
    for (i = 0; bad[i]; i++)
        if (strstr(text, bad[i])) { qs_note(text); return; }
    printf("[QUICKSAVE] %s\n", text);
    fflush(stdout);
    if (pc_present_toast) pc_present_toast(text);
}
#define notify qs_notify

/* A note that the next toast must not replace (a request that will not be
 * carried out): queued by the presenter (pc_present_note). */
static void qs_note(const char *text) {
    printf("[QUICKSAVE] %s\n", text);
    fflush(stdout);
    if (pc_present_note) pc_present_note(text, 4000);
    else if (pc_present_toast) pc_present_toast(text);
}

/* ------------------------------------------------------------------ */
/* what an entry shows                                                 */
/* ------------------------------------------------------------------ */

/* The calendar and the player's stats, read from PS1 RAM (the album
 * record's fields, pc/tools/tokimeki_album.py FIELDS / STATS):
 * now_year 0x800EE79E (95..98), now_month, now_day, now_youbi (0 Monday),
 * pass_days 0x800EE7A2 (days since 1995-04-04), and the s16 integer half
 * of the nine 16.16 stats at 0x800EE85E + 4 i. */
#define RAM8(a) (*(volatile const u8 *)(unsigned long)(a))
#define RAM16(a) (*(volatile const u16 *)(unsigned long)(a))
static const char *const k_stats[9] = {
    "para_tai", "para_bun", "para_ri", "para_gei", "para_un", "para_zat",
    "para_you", "para_kon", "para_str",
};
static const char *const k_youbi[7] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };

/* The calendar holds its initial 1995-04-04 from power-on, so a plausible
 * date is not enough: scenes up to 20 (boot, title 18, album / system file
 * 19, name entry 20) are before or outside a game.  Step 2 is the overlay
 * loader between two scenes (step_state_save_and_redirect: now_step = 2,
 * prev_step = the scene being loaded): it counts as that scene. */
int qs_game_in_progress(void) {
    unsigned y = RAM8(0x800EE79E), m = RAM8(0x800EE79F), d = RAM8(0x800EE7A0);
    unsigned step = now_step == 2 ? RAM8(0x800EF86E) : now_step;   /* prev_step */
    return step > 20 && y >= 95 && y <= 98 && m >= 1 && m <= 12 && d >= 1 && d <= 31 &&
           RAM8(0x800EE7A1) < 7;
}

static int index_text(const char *id, char *text, size_t cap) {
    char stamp[32];
    struct timespec ts;
    time_t now;
    struct tm tmv;
    const char *ovl = hal_overlay_loaded();
    int n, i, num, k = qs_id_kind(id, &num);
    clock_gettime(CLOCK_REALTIME, &ts);
    now = ts.tv_sec;
    localtime_r(&now, &tmv);
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tmv);
    n = snprintf(text, cap,
                 "# Heartthrob Memorial PC Enhanced save (pc/hal/quicksave.c;\n"
                 "# format: docs/pc-port/ENHANCED-MODE.md \"Quicksave slots\").\n"
                 "[slot]\nformat = 1\n");
    if (k == QS_KIND_QUICK || k == QS_KIND_UNDO)
        n += snprintf(text + n, cap - (size_t)n, "slot = %d\n", num);
    n += snprintf(text + n, cap - (size_t)n,
                  "id = %s\nkind = %s\nstate = %s.tks\nthumbnail = %s.png\n"
                  "thumbnail_size = %dx%d\nsaved = %s\nsaved_unix = %lld\nsaved_unix_ms = %lld\n"
                  "build_id = %s\n"
                  "profile = %s\nframe_counter = %ld\nvblanks = %lu\nscene = %u/%u/%u\n"
                  "overlay = %s\n[game]\n",
                  id, kind_word(k), id, id, QS_THUMB_W, QS_THUMB_H, stamp,
                  (long long)now, (long long)now * 1000 + ts.tv_nsec / 1000000,
                  dbg_state_build_id(), hal_profile_name(),
                  (long)frame_counter, (unsigned long)hal_clock_vblanks(), now_step,
                  now_sub_step, now_sub_sub_step, ovl && *ovl ? ovl : "-");
    if (qs_game_in_progress()) {
        unsigned pass = RAM16(0x800EE7A2);
        n += snprintf(text + n, cap - (size_t)n,
                      "in_progress = 1\nyear = %u\nmonth = %u\nday = %u\nyoubi = %u\n"
                      "weekday = %s\npass_days = %u\nweek = %u\n",
                      1900u + RAM8(0x800EE79E), RAM8(0x800EE79F), RAM8(0x800EE7A0),
                      RAM8(0x800EE7A1), k_youbi[RAM8(0x800EE7A1)], pass, pass / 7 + 1);
        for (i = 0; i < 9; i++)
            n += snprintf(text + n, cap - (size_t)n, "%s = %d\n", k_stats[i],
                          (int)(s16)RAM16(0x800EE85E + 4 * i));
    } else {
        n += snprintf(text + n, cap - (size_t)n, "in_progress = 0\n");
    }
    return n;
}

/* 160x120: each thumbnail pixel the average of a 2x2 block of the
 * displayed 320x240 picture; 0 when the display is not 320x240 */
static int thumbnail_rgb(unsigned char *rgb) {
    GpuScanoutState d;
    unsigned int *px = dbg_mem_alloc(GPU_SCANOUT_PIXELS * sizeof *px);
    int w = 0, h = 0, x, y, c, ok = 0;
    if (px) {
        gpu_get_scanout_state(&d);
        if (gpu_scanout_pack(&d, &g_vram[0][0], px, GPU_SCANOUT_PIXELS, &w, &h) &&
            w == 2 * QS_THUMB_W && h == 2 * QS_THUMB_H) {
            for (y = 0; y < QS_THUMB_H; y++)
                for (x = 0; x < QS_THUMB_W; x++)
                    for (c = 0; c < 3; c++) {
                        int sh = 24 - 8 * c, sum = 0, dy, dx;
                        for (dy = 0; dy < 2; dy++)
                            for (dx = 0; dx < 2; dx++)
                                sum += (px[(2 * y + dy) * GPU_SCANOUT_WIDTH + 2 * x + dx] >> sh) & 0xFF;
                        rgb[(y * QS_THUMB_W + x) * 3 + c] = (unsigned char)((sum + 2) / 4);
                    }
            ok = 1;
        }
    }
    dbg_mem_free(px);
    return ok;
}

/* ------------------------------------------------------------------ */
/* the writer                                                          */
/* ------------------------------------------------------------------ */

/* One save in flight: captured at the boundary, written by the worker. */
typedef struct {
    char id[QS_ID_MAX];
    int quiet;
    DbgStateSnap *snap;
    unsigned char *rgb;          /* thumbnail, NULL when none */
    char index[2048];
    int index_len;
    double capture_ms, write_ms;
    int result;                  /* 0 ok, -1 failed, 2 index not written */
    char why[512];
} Job;

static Job g_job;
static pthread_t g_worker;
static int g_worker_started;     /* g_worker must be joined */
static int g_job_done;           /* the worker finished (atomic) */
static int g_job_reported = 1;   /* its result was shown */

static double ms_since(const struct timespec *t0) {
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (t1.tv_sec - t0->tv_sec) * 1000.0 + (t1.tv_nsec - t0->tv_nsec) / 1e6;
}

static void *worker_main(void *arg) {
    Job *j = arg;
    char path[PC_SETTINGS_PATH_MAX + 64], tmp[PC_SETTINGS_PATH_MAX + 64];
    const char *why = "";
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    {
        /* PC_QS_WRITE_DELAY_MS=N (tests only): a slow disk -- the writer
         * stays busy N ms, so the machines that wait for it
         * (save_auto.c's quit) can be driven through their busy path */
        const char *d = getenv("PC_QS_WRITE_DELAY_MS");
        long ms = d && *d ? atol(d) : 0;
        if (ms > 0) {
            struct timespec w = { ms / 1000, (ms % 1000) * 1000000L };
            nanosleep(&w, NULL);
        }
    }
    entry_path(path, sizeof path, j->id, "tks");
    if (dbg_state_write(j->snap, path, &why)) {
        j->snap = NULL;
        snprintf(j->why, sizeof j->why, "%s", why);
        j->result = -1;
        goto done;
    }
    j->snap = NULL;
    snprintf(j->why, sizeof j->why, "%s", why);
    {
        /* the entry's old picture and index describe the old state: gone
         * before the new ones are written, so a failure leaves no stale
         * description */
        entry_path(tmp, sizeof tmp, j->id, "png");
        unlink(tmp);
        entry_path(tmp, sizeof tmp, j->id, "ini");
        unlink(tmp);
    }
    if (j->rgb) {
        char fin[PC_SETTINGS_PATH_MAX + 64];
        entry_path(tmp, sizeof tmp, j->id, "png.tmp");
        entry_path(fin, sizeof fin, j->id, "png");
        if (present_png_write(tmp, j->rgb, QS_THUMB_W, QS_THUMB_H) || rename(tmp, fin)) {
            unlink(tmp);
            /* the index says so */
            char *t = strstr(j->index, "\nthumbnail = ");
            if (t) {
                char *e = strchr(t + 1, '\n');
                memmove(t + 13, e, strlen(e) + 1);
                j->index_len = (int)strlen(j->index);
            }
        }
    }
    {
        char fin[PC_SETTINGS_PATH_MAX + 64];
        entry_path(tmp, sizeof tmp, j->id, "ini.tmp");
        entry_path(fin, sizeof fin, j->id, "ini");
        if (!dbg_write_file(tmp, j->index, (unsigned long)j->index_len) || rename(tmp, fin)) {
            unlink(tmp);
            j->result = 2;
            goto done;
        }
    }
    j->result = 0;
done:
    dbg_mem_free(j->rgb);
    j->rgb = NULL;
    j->write_ms = ms_since(&t0);
    __atomic_store_n(&g_job_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

/* The finished job's toast and log line (game thread). */
static void report_job(void) {
    char name[32], msg[64];
    Job *j = &g_job;
    if (g_job_reported || !__atomic_load_n(&g_job_done, __ATOMIC_ACQUIRE)) return;
    g_job_reported = 1;
    log_name(j->id, name, sizeof name);
    if (j->result < 0) {
        fprintf(stderr, "[QUICKSAVE] %s: %s\n", name, j->why);
        if (!j->quiet) notify("Quicksave failed");
    } else if (j->result == 2) {
        fprintf(stderr, "[QUICKSAVE] %s: index not written\n", name);
        if (!j->quiet) notify("Quicksave: index not written");
    } else {
        printf("[QUICKSAVE] %s saved: %s/quicksave/%s.tks (%s) capture %.1f ms, write %.1f ms\n",
               name, pc_save_dir(), j->id, j->why, j->capture_ms, j->write_ms);
        if (!j->quiet) {
            qs_id_name(j->id, name, sizeof name);
            snprintf(msg, sizeof msg, "Saved %s", mid(name));
            notify(msg);
        }
    }
    fflush(stdout);
}

void qs_writer_wait(void) {
    if (!g_worker_started) return;
    pthread_join(g_worker, NULL);
    g_worker_started = 0;
    report_job();
}

void qs_shutdown(void) { qs_writer_wait(); }

static int writer_busy(void) {
    return g_worker_started && !__atomic_load_n(&g_job_done, __ATOMIC_ACQUIRE);
}

/* 0 captured (being written), 1 not now, -1 failed */
int qs_save_now(const char *id, int quiet) {
    const char *why = "";
    struct timespec t0;
    char name[32];
    int r;
    if (qs_id_kind(id, NULL) == QS_KIND_NONE) return -1;
    if (writer_busy()) return 1;
    qs_writer_wait();            /* join the finished one */
    if (ensure_dir()) {
        if (!quiet) notify("Quicksave failed: no save folder");
        return -1;
    }
    memset(&g_job, 0, sizeof g_job);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    r = dbg_state_capture(&g_job.snap, &why);
    if (r > 0) return 1;
    if (r < 0) {
        log_name(id, name, sizeof name);
        fprintf(stderr, "[QUICKSAVE] %s: %s\n", name, why);
        if (!quiet) notify("Quicksave failed");
        return -1;
    }
    snprintf(g_job.id, sizeof g_job.id, "%s", id);
    g_job.quiet = quiet;
    g_job.rgb = dbg_mem_alloc(QS_THUMB_W * QS_THUMB_H * 3);
    if (g_job.rgb && !thumbnail_rgb(g_job.rgb)) {
        dbg_mem_free(g_job.rgb);
        g_job.rgb = NULL;
    }
    g_job.index_len = index_text(id, g_job.index, sizeof g_job.index);
    if (!g_job.rgb) {
        char *t = strstr(g_job.index, "\nthumbnail = ");
        if (t) {
            char *e = strchr(t + 1, '\n');
            memmove(t + 13, e, strlen(e) + 1);
            g_job.index_len = (int)strlen(g_job.index);
        }
    }
    g_job.capture_ms = ms_since(&t0);
    g_job_done = 0;
    g_job_reported = 0;
    /* PC_QS_SYNC=1: write on this thread (measuring the write's cost) */
    if ((getenv("PC_QS_SYNC") && *getenv("PC_QS_SYNC") == '1') ||
        pthread_create(&g_worker, NULL, worker_main, &g_job)) {
        worker_main(&g_job);
        report_job();
        return 0;
    }
    g_worker_started = 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* loading                                                             */
/* ------------------------------------------------------------------ */

/* The memory card is the player's persistent storage, not machine state.
 * A save state carries the card image (memcard.c g_card is in the
 * executable's data), and a plain load would put back the card as it was
 * at the quicksave -- the game's next card write would then flush that old
 * image over card1.mcr and lose every in-game save made since.  A load
 * therefore keeps the live card image and its dirty flag.  (The debug
 * toolkit's own `load` / PC_LOADSTATE restore it, for bit-exact replays.) */
static const char *const k_card_syms[] = { "memcard.c:g_card", "memcard.c:g_card_dirty" };
typedef struct { unsigned char *addr, *copy; unsigned long size; } Held;

static void card_hold(Held h[2]) {
    int i;
    for (i = 0; i < 2; i++) {
        h[i].addr = dbg_sym_lookup(k_card_syms[i], &h[i].size);
        h[i].copy = h[i].addr && h[i].size ? dbg_mem_alloc(h[i].size) : NULL;
        if (h[i].copy) memcpy(h[i].copy, h[i].addr, h[i].size);
        else fprintf(stderr, "[QUICKSAVE] %s not found: the card image is not kept\n", k_card_syms[i]);
    }
}

static void card_put(Held h[2]) {
    int i;
    for (i = 0; i < 2; i++) {
        if (h[i].copy) memcpy(h[i].addr, h[i].copy, h[i].size);
        dbg_mem_free(h[i].copy);
    }
}

int qs_load_now(const char *id, int quiet) {
    char path[PC_SETTINGS_PATH_MAX + 64], ini[PC_SETTINGS_PATH_MAX + 64], text[2048];
    char bid[80], msg[64], name[32], lname[32];
    const char *why = "";
    struct stat st;
    (void)quiet;
    qs_writer_wait();            /* the state may still be being written */
    qs_id_name(id, name, sizeof name);
    log_name(id, lname, sizeof lname);
    entry_path(path, sizeof path, id, "tks");
    if (qs_id_kind(id, NULL) == QS_KIND_NONE || stat(path, &st)) {
        snprintf(msg, sizeof msg, "%s is empty", name);
        notify(msg);
        return -1;
    }
    entry_path(ini, sizeof ini, id, "ini");
    if (read_small(ini, text, sizeof text) && ini_get(text, "build_id", bid, sizeof bid) &&
        strcmp(bid, dbg_state_build_id()) && !getenv("PC_LOADSTATE_FORCE")) {
        snprintf(msg, sizeof msg, "%s: saved by another build", name);
        fprintf(stderr, "[QUICKSAVE] %s was saved by build %s, this is %s\n", lname, bid,
                dbg_state_build_id());
        notify(msg);
        return -1;
    }
    /* the machine as it is, so this load can be undone (slot 0) */
    if (strcmp(id, "slot0")) {
        int r = qs_save_now("slot0", 1);
        if (r) printf("[QUICKSAVE] slot 0 (undo) not written (%s)\n", r > 0 ? "busy" : "failed");
    }
    {
        Held held[2];
        int r;
        card_hold(held);
        r = dbg_state_load(path, &why);
        card_put(held);
        if (r) {
            fprintf(stderr, "[QUICKSAVE] load %s: %s\n", lname, why);
            snprintf(msg, sizeof msg, "Load %s failed", mid(name));
            notify(msg);
            return -1;
        }
    }
    /* (dbg_state_load told every observer -- pacing, autosave, the rewind
     * buffer, extras, ...: dbg_state_after_restore, pc/hal/dbg_state.c) */
    printf("[QUICKSAVE] %s loaded: %s (%s)\n", lname, path, why);
    snprintf(msg, sizeof msg, "Loaded %s", mid(name));
    notify(msg);
    return 0;
}

/* pc/hal/dbg_cmds.c calls this at every main-loop boundary once the toolkit
 * is active (on the toolkit's own stack, no game code below). */
void qs_boundary(void) {
    report_job();
    /* Not while the memory card has a transfer in flight, as the autosave
     * (save_auto.c machine_quiet): a state taken then holds a half-done
     * transfer that its load would finish onto the newer live card image
     * (a load keeps the live card), and a load now would abandon the live
     * one half-written.  Card transfers take a few frames; after
     * QS_CARD_LIMIT calls the request goes ahead anyway. */
    if ((g_req_load[0] || g_req_save[0]) && memcard_busy && memcard_busy()) {
        if (!g_card_waits)
            printf("[QUICKSAVE] %s waits for the memory card (a transfer is in flight)\n",
                   g_req_load[0] ? "the load" : "the save");
        if (++g_card_waits < QS_CARD_LIMIT) goto autos;
        printf("[QUICKSAVE] the memory card stayed busy for %d boundary calls: going ahead\n",
               g_card_waits);
    }
    if (g_card_waits) {
        printf("[QUICKSAVE] memory card idle after %d boundary call(s)\n", g_card_waits);
        g_card_waits = 0;
    }
    if (g_req_load[0]) {
        char id[QS_ID_MAX];
        snprintf(id, sizeof id, "%s", g_req_load);
        g_req_load[0] = 0;
        if (g_req_save[0]) {
            /* a load supersedes a pending save: said, not silent */
            char name[32], msg[96];
            qs_id_name(g_req_save, name, sizeof name);
            snprintf(msg, sizeof msg, "Save to %s cancelled: a load came first", mid(name));
            qs_note(msg);
            g_req_save[0] = 0;
        }
        g_last_result = qs_load_now(id, 0);
    }
    if (g_req_save[0]) {
        int r = qs_save_now(g_req_save, 0);
        if (r > 0 && ++g_busy_tries < QS_BUSY_LIMIT) goto autos;   /* retry next boundary */
        if (r > 0) notify("Quicksave failed: busy");
        g_last_result = r;
        g_req_save[0] = 0;
        g_busy_tries = 0;
    }
autos:
    if (sa_boundary) sa_boundary();
}

/* ------------------------------------------------------------------ */
/* entries: listing, deleting, thumbnails                              */
/* ------------------------------------------------------------------ */

int qs_entry_read(const char *id, QsEntry *e) {
    char path[PC_SETTINGS_PATH_MAX + 64], text[2048], v[64];
    struct stat st;
    int i;
    memset(e, 0, sizeof *e);
    snprintf(e->id, sizeof e->id, "%s", id);
    e->kind = qs_id_kind(id, &e->number);
    if (e->kind == QS_KIND_NONE) return 0;
    entry_path(path, sizeof path, id, "tks");
    e->has_state = stat(path, &st) == 0;
    entry_path(path, sizeof path, id, "ini");
    if (!read_small(path, text, sizeof text)) {
        if (e->has_state) {
            e->saved_unix = (long long)st.st_mtime;
            e->saved_ms = e->saved_unix * 1000;
        }
        return e->has_state;
    }
    e->has_index = 1;
    if (ini_get(text, "build_id", v, sizeof v)) e->same_build = !strcmp(v, dbg_state_build_id());
    if (ini_get(text, "saved_unix", v, sizeof v)) e->saved_unix = atoll(v);
    e->saved_ms = ini_get(text, "saved_unix_ms", v, sizeof v) ? atoll(v) : e->saved_unix * 1000;
    ini_get(text, "saved", e->saved, sizeof e->saved);
    ini_get(text, "scene", e->scene, sizeof e->scene);
    ini_get(text, "profile", e->profile, sizeof e->profile);
    ini_get(text, "label", e->label, sizeof e->label);
    if (ini_get(text, "in_progress", v, sizeof v) && v[0] == '1') {
        e->in_progress = 1;
        e->year = ini_int(text, "year", 0);
        e->month = ini_int(text, "month", 0);
        e->day = ini_int(text, "day", 0);
        e->youbi = ini_int(text, "youbi", 0);
        e->pass_days = ini_int(text, "pass_days", 0);
        e->week = ini_int(text, "week", 0);
        ini_get(text, "weekday", e->weekday, sizeof e->weekday);
        /* "Sun": indexes written before the PC UI's sentence case hold "SUN" */
        for (i = 1; e->weekday[0] && e->weekday[i]; i++)
            e->weekday[i] = (char)tolower((unsigned char)e->weekday[i]);
        for (i = 0; i < 9; i++) e->stats[i] = ini_int(text, k_stats[i], 0);
    }
    return 1;
}

static int entry_cmp(const void *a, const void *b) {
    const QsEntry *x = a, *y = b;
    if (x->saved_ms != y->saved_ms) return x->saved_ms > y->saved_ms ? -1 : 1;
    return strcmp(x->id, y->id);
}

int qs_entries(QsEntry *out, int max) {
    char dir[PC_SETTINGS_PATH_MAX + 32];
    DIR *d;
    struct dirent *de;
    int n = 0, i;
    qs_writer_wait();
    snprintf(dir, sizeof dir, "%s/quicksave", pc_save_dir());
    d = opendir(dir);
    if (!d) return 0;
    while ((de = readdir(d)) != NULL && n < max) {
        char id[QS_ID_MAX];
        const char *dot = strrchr(de->d_name, '.');
        size_t len;
        if (!dot || (strcmp(dot, ".tks") && strcmp(dot, ".ini"))) continue;
        len = (size_t)(dot - de->d_name);
        if (len == 0 || len >= sizeof id) continue;
        memcpy(id, de->d_name, len);
        id[len] = 0;
        if (qs_id_kind(id, NULL) == QS_KIND_NONE) continue;
        for (i = 0; i < n; i++)
            if (!strcmp(out[i].id, id)) break;
        if (i < n) continue;       /* seen through its other file */
        if (qs_entry_read(id, &out[n])) n++;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof *out, entry_cmp);
    return n;
}

int qs_new_save_id(char *out, size_t cap) {
    int i;
    char path[PC_SETTINGS_PATH_MAX + 64];
    struct stat st;
    qs_writer_wait();
    for (i = 1; i <= 9999; i++) {
        snprintf(out, cap, "save%04d", i);
        entry_path(path, sizeof path, out, "tks");
        if (stat(path, &st) == 0) continue;
        entry_path(path, sizeof path, out, "ini");
        if (stat(path, &st) == 0) continue;
        if (g_req_save[0] && !strcmp(g_req_save, out)) continue;
        return 1;
    }
    out[0] = 0;
    return 0;
}

int qs_delete_id(const char *id) {
    static const char *const exts[] = { "tks", "png", "ini" };
    char path[PC_SETTINGS_PATH_MAX + 64], name[32];
    int i, any = 0;
    if (qs_id_kind(id, NULL) == QS_KIND_NONE) return -1;
    qs_writer_wait();
    for (i = 0; i < 3; i++) {
        entry_path(path, sizeof path, id, exts[i]);
        if (unlink(path) == 0) any = 1;
    }
    log_name(id, name, sizeof name);
    printf("[QUICKSAVE] %s %s\n", name, any ? "deleted" : "not found");
    fflush(stdout);
    return any ? 0 : -1;
}

/* Our own PNG (present_png_write: 8-bit RGB, filter 0, stored blocks). */
int qs_thumb_read(const char *id, unsigned char *rgb) {
    char path[PC_SETTINGS_PATH_MAX + 64];
    entry_path(path, sizeof path, id, "png");
    return present_png_read(path, rgb, QS_THUMB_W, QS_THUMB_H) == 0;
}

/* ------------------------------------------------------------------ */
/* the slot list                                                       */
/* ------------------------------------------------------------------ */

/* One line describing SLOT (no newline) */
static void slot_line(int slot, char *out, size_t cap) {
    char id[8], when[32] = "";
    QsEntry e;
    snprintf(id, sizeof id, "slot%d", slot);
    if (!qs_entry_read(id, &e) || !e.has_index) {
        snprintf(out, cap, "%c%d  -", slot == g_slot ? '>' : ' ', slot);
        return;
    }
    if (strlen(e.saved) >= 16)
        snprintf(when, sizeof when, "%.5s %.5s", e.saved + 5, e.saved + 11);   /* MM-DD HH:MM */
    if (e.in_progress && e.weekday[0]) {
        snprintf(out, cap, "%c%d  %d/%02d/%02d %s  %s%s", slot == g_slot ? '>' : ' ', slot,
                 e.year, e.month, e.day, e.weekday, when, slot == 0 ? " (undo)" : "");
    } else {
        snprintf(out, cap, "%c%d  Scene %s  %s%s", slot == g_slot ? '>' : ' ', slot, e.scene, when,
                 slot == 0 ? " (undo)" : "");
    }
}

int qs_slot_list(char *out, size_t cap) {
    size_t n = 0;
    int s;
    qs_writer_wait();
    n += (size_t)snprintf(out, cap, "Quicksave slot %d\n", g_slot);
    for (s = 1; s <= QS_SLOTS && n < cap; s++) {
        char line[96];
        slot_line(s % QS_SLOTS, line, sizeof line);   /* 1..9, then 0 */
        n += (size_t)snprintf(out + n, cap - n, "%s\n", line);
    }
    return (int)n;
}

static void show_list(void) {
    char text[1200];
    qs_slot_list(text, sizeof text);
    printf("[QUICKSAVE] %s", text);
    fflush(stdout);
    if (pc_present_osd) pc_present_osd(text, 3000);
}

/* ------------------------------------------------------------------ */
/* requests (hotkeys, the browser, toolkit commands)                   */
/* ------------------------------------------------------------------ */

/* The hotkeys work in interactive runs only: never in an automated run
 * (the settings' harness rule; TOKI_QS_HOTKEYS=1 is the tests' exception),
 * and not when PC_DBG_SLOT hands F5/F9 to the debug toolkit's own state
 * file. */
int qs_hotkeys_enabled(void) {
    const char *slot = getenv("PC_DBG_SLOT"), *t = getenv("TOKI_QS_HOTKEYS");
    if (slot && *slot) return 0;
    if (t && *t == '1') return 1;     /* tests: the hotkeys in an automated run */
    return !pc_settings_get()->harness;
}

static int clamp_slot(int slot) {
    if (slot < 0 || slot >= QS_SLOTS) return g_slot;
    return slot;
}

/* 1 when this executable can take and load states (pc/hal/dbg_state.c);
 * else the player is told once why not, and nothing is attempted.  (A
 * stripped executable has no symbol table to name its objects by.) */
int qs_available(void) {
    static int told;
    const char *why = dbg_state_unavailable();
    if (!why) return 1;
    if (!told++) fprintf(stderr, "[QUICKSAVE] save states are not available: %s\n", why);
    notify("Save states are not available in this build");
    return 0;
}

/* A request is acted on at the next boundary, which can be a while away
 * (the boot movie is one long frame): say so at once. */
void qs_request_save_id(const char *id) {
    char name[32], msg[64];
    int k = qs_id_kind(id, NULL);
    if (k == QS_KIND_NONE) return;
    if (!qs_available()) return;
    if (k == QS_KIND_UNDO) { notify("Slot 0 is the automatic undo slot"); return; }
    if (g_req_save[0] && strcmp(g_req_save, id)) {
        /* a second save request before the first was carried out: the
         * first is dropped -- said, not silent */
        char old[32], msg[96];
        qs_id_name(g_req_save, old, sizeof old);
        old[0] = (char)tolower((unsigned char)old[0]);
        qs_id_name(id, name, sizeof name);
        snprintf(msg, sizeof msg, "Saving %s instead of %s", mid(name), old);
        qs_note(msg);
    }
    snprintf(g_req_save, sizeof g_req_save, "%s", id);
    g_busy_tries = 0;
    qs_id_name(id, name, sizeof name);
    snprintf(msg, sizeof msg, "Saving %s...", mid(name));
    if (pc_present_toast) pc_present_toast(msg);
    dbg_toolkit_activate();
}

void qs_request_load_id(const char *id) {
    char name[32], msg[64];
    if (qs_id_kind(id, NULL) == QS_KIND_NONE) return;
    if (!qs_available()) return;
    if (g_req_load[0] && strcmp(g_req_load, id)) {
        char old[32], msg[96];
        qs_id_name(g_req_load, old, sizeof old);
        old[0] = (char)tolower((unsigned char)old[0]);
        qs_id_name(id, name, sizeof name);
        snprintf(msg, sizeof msg, "Loading %s instead of %s", mid(name), old);
        qs_note(msg);
    }
    snprintf(g_req_load, sizeof g_req_load, "%s", id);
    qs_id_name(id, name, sizeof name);
    snprintf(msg, sizeof msg, "Loading %s...", mid(name));
    if (pc_present_toast) pc_present_toast(msg);
    dbg_toolkit_activate();
}

void qs_request_save(int slot) {
    char id[8];
    slot = clamp_slot(slot);
    snprintf(id, sizeof id, "slot%d", slot);
    qs_request_save_id(id);
}

void qs_request_load(int slot) {
    char id[8];
    snprintf(id, sizeof id, "slot%d", clamp_slot(slot));
    qs_request_load_id(id);
}

void qs_select_slot(int slot) {
    if (slot >= 0 && slot < QS_SLOTS) g_slot = slot;
    show_list();
}

void qs_slot_step(int delta) {
    g_slot = ((g_slot + delta) % QS_SLOTS + QS_SLOTS) % QS_SLOTS;
    show_list();
}

int qs_current_slot(void) { return g_slot; }
int qs_last_result(void) { return g_last_result; }
int qs_pending(void) { return g_req_save[0] || g_req_load[0]; }
