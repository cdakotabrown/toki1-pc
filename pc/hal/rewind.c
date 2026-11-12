/* SPDX-License-Identifier: GPL-3.0-or-later */
/* rewind.c -- the enhanced profile's rewind (docs/pc-port/ENHANCED-MODE.md
 * "Rewind"; player keys: PLAYER-GUIDE.md).
 *
 * What it keeps.  Every [enhanced] rewind_interval_frames game frames, at
 * the main-loop boundary (pc/hal/dbg_cmds.c: no game code on the stack),
 * a SNAPSHOT of the bytes a quickload restores: the executable's data
 * minus the debug toolkit's KEEP ranges (host resources, the player's
 * session) and minus the memory card image, PS1 RAM, the MMIO window and
 * the game stack below the boundary (dbg_state_ranges, pc/hal/dbg_state.c).
 * The newest snapshot is kept whole (the HEAD); every older one as the
 * delta to the next newer (pc/hal/rewind_codec.c: XOR, zero runs of 64-bit
 * words), newest last, up to [enhanced] rewind_seconds of game frames or
 * TOKI_REWIND_MAX_MB of deltas, the oldest dropped first.
 *
 * Cost on the frame.  The boundary copies the ranges into a spare buffer
 * (a memcpy of ~TOTAL bytes, under the audio lock like a quicksave's
 * capture); a worker thread then codes HEAD xor SPARE, stores the delta and
 * makes the spare the new head.  A boundary while the worker is still busy
 * takes no snapshot (counted, `skipped`).  Measured numbers:
 * ENHANCED-MODE.md "Rewind".
 *
 * Rewinding.  While the rewind key is held ([hotkeys] rewind), the
 * boundary does not return to the game: it restores the head (the newest
 * snapshot), shows its picture, and every STEP period applies the newest
 * delta to the head and restores the older snapshot it makes, until the
 * key is released or the buffer is empty (it then waits at the oldest).
 * The game resumes from the snapshot shown.  A restore is a quickload of
 * that moment: the live memory card is kept (not in the ranges), the
 * session (KEEP files: settings, profile, saves, backlog list, this file)
 * is kept, the sound is paused while rewinding, and afterwards the frame
 * pacing is resynchronised (hal_pacing_resync), the autosave learns the
 * week (sa_after_load) and keys held for the rewind reach the game only
 * once released (pad_suppress_until_release).  Rewound-over snapshots are
 * gone (no "forward").
 *
 * When.  Enhanced profile (switch TOKI_REWIND), [enhanced] rewind (default
 * on), never in automated runs (the settings' harness rule) -- unless the
 * environment says TOKI_REWIND=1, which the tests do (either profile).
 * Toolkit commands `rewind [N]` / `rewind_to FRAME` (pc/hal/dbg_cmds.c)
 * drive it without a key.  PC_REWIND_LOG=1 prints every snapshot and
 * restore.  PC_REWIND_NEGATIVE=spu|pram|card breaks the restore on purpose
 * (negative controls of pc/tools/test_rewind.py only; see negative_mode).
 *
 * Nothing here writes game memory except a restore, which writes back
 * bytes the game itself had at that boundary.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <SDL2/SDL.h>
#include "types.h"
#include "dbg_toolkit.h"
#include "pc_settings.h"
#include "profile.h"
#include "rewind_codec.h"

extern s32 frame_counter;
extern u32 hal_clock_vblanks(void);
extern void *dbg_mem_alloc(unsigned long n);
extern void dbg_mem_free(void *p);
extern void spu_hal_audio_lock(void);
extern void spu_hal_audio_unlock(void);
extern void spu_hal_set_paused(int paused) __attribute__((weak));
extern void gpu_display(void) __attribute__((weak));
extern SDL_Renderer *hal_get_renderer(void) __attribute__((weak));
extern void pc_present_osd(const char *text, unsigned ms) __attribute__((weak));
extern int pc_present_hotkey_held(int hk) __attribute__((weak));
extern void pc_watchdog_presented(void) __attribute__((weak));
extern void pad_suppress_until_release(void) __attribute__((weak));
extern int hal_is_running(void) __attribute__((weak));
extern void hal_request_quit(void) __attribute__((weak));
extern unsigned long spu_audio_hash_calls(void) __attribute__((weak));
/* pc/hal/memcard.c: a card request / transfer in flight (weak: harnesses) */
extern int memcard_busy(void) __attribute__((weak));

#define MAX_RANGES 2048
#define VBLANK_HZ 59.94

/* the live memory card: the player's storage, kept as a quickload keeps it */
static const char *const k_exclude[] = { "memcard.c:g_card", "memcard.c:g_card_dirty", NULL };

typedef struct {
    unsigned char *data;        /* the delta to the next newer snapshot */
    size_t len;
    s32 frame;                  /* this snapshot's frame_counter / VBlank */
    u32 vblank;
} Entry;

static int negative_mode(void);
static int g_init;              /* 0 not tried, 1 ready, -1 off / failed */
static DbgRange g_ranges[MAX_RANGES];
static int g_nranges;
static size_t g_words;          /* the image, 64-bit words (zero padded) */
static u64w *g_head, *g_spare;
static int g_have_head;
static s32 g_head_frame, g_spare_frame;
static u32 g_head_vblank, g_spare_vblank;
static unsigned char *g_enc;    /* the worker's coding buffer */
static size_t g_enc_cap;

static Entry *g_ring;           /* oldest .. newest, circular */
static int g_cap, g_first, g_count;
static size_t g_bytes;          /* sum of the deltas */
static size_t g_max_bytes;
static s32 g_next_frame;        /* the next snapshot is due here */
static int g_log;

/* statistics (PC_REWIND_LOG, the tests) */
static unsigned long g_snaps, g_skipped, g_busy, g_dropped;
static int g_restores;          /* restores made (tests: `when rewind.c:g_restores=N`) */
static double g_copy_ms_sum, g_copy_ms_max, g_code_ms_sum, g_code_ms_max;
static size_t g_delta_max;

/* the worker */
static pthread_t g_thread;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_job;               /* 1: code g_head xor g_spare (under g_mu) */
static int g_thread_ok;

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* ------------------------------------------------------------------ */
/* switches                                                            */
/* ------------------------------------------------------------------ */

/* The environment's TOKI_REWIND, when set, decides (tests, developers);
 * otherwise the enhanced profile's switch, the player's [enhanced] rewind
 * (read live: the settings menu turns it off) and never a harness run. */
static int forced = -1;
int rw_enabled(void) {
    const PcSettings *s = pc_settings_get();
    if (forced < 0) {
        const char *e = getenv("TOKI_REWIND");
        forced = e && *e ? (*e != '0' ? 1 : 0) : 2;
    }
    if (forced != 2) return forced;
    return hal_feature_on("TOKI_REWIND") && !s->harness && s->rewind;
}

/* dbg_cmds.c, before the main loop: the boundary must run every frame */
int rw_wanted(void) { return rw_enabled(); }

static int interval_frames(void) {
    long v = hal_setting_long("TOKI_REWIND_INTERVAL", 0);
    if (v <= 0) v = pc_settings_get()->rewind_interval;
    return v < 1 ? 1 : v > 600 ? 600 : (int)v;
}

static int capacity(void) {
    long secs = hal_setting_long("TOKI_REWIND_SECONDS", 0);
    long n;
    if (secs <= 0) secs = pc_settings_get()->rewind_seconds;
    n = (long)(secs * 60 / interval_frames());
    return n < 1 ? 1 : n > 100000 ? 100000 : (int)n;
}

/* ------------------------------------------------------------------ */
/* the ring                                                            */
/* ------------------------------------------------------------------ */

static Entry *ring_at(int k) { return &g_ring[(g_first + k) % g_cap]; }

static void drop_oldest(void) {
    Entry *e = ring_at(0);
    g_bytes -= e->len;
    dbg_mem_free(e->data);
    memset(e, 0, sizeof *e);
    g_first = (g_first + 1) % g_cap;
    g_count--;
    g_dropped++;
}

/* (g_mu held) */
static void push(unsigned char *data, size_t len, s32 frame, u32 vblank) {
    Entry *e;
    while (g_count && (g_count >= g_cap || g_bytes + len > g_max_bytes)) drop_oldest();
    e = ring_at(g_count);
    e->data = data;
    e->len = len;
    e->frame = frame;
    e->vblank = vblank;
    g_count++;
    g_bytes += len;
}

/* Code HEAD xor SPARE into a delta, keep it, make the spare the head.
 * The worker thread's job (or the boundary's, without a thread). */
static void code_job(void) {
    double t0 = now_ms(), ms;
    size_t len, changed = 0;
    unsigned char *copy;
    u64w *t;
    /* the delta that turns the new snapshot (spare) back into the head */
    len = rw_delta_encode(g_head, g_spare, g_words, g_enc, g_enc_cap, &changed);
    copy = len ? dbg_mem_alloc(len) : NULL;
    if (copy) memcpy(copy, g_enc, len);
    ms = now_ms() - t0;
    pthread_mutex_lock(&g_mu);
    if (copy) {
        push(copy, len, g_head_frame, g_head_vblank);
        if (len > g_delta_max) g_delta_max = len;
    } else {
        /* no memory: the history before the new head is lost */
        while (g_count) drop_oldest();
    }
    t = g_head;
    g_head = g_spare;
    g_spare = t;
    g_head_frame = g_spare_frame;
    g_head_vblank = g_spare_vblank;
    g_code_ms_sum += ms;
    if (ms > g_code_ms_max) g_code_ms_max = ms;
    pthread_mutex_unlock(&g_mu);
    if (g_log) {
        printf("[REWIND] coded frame %ld: delta %zu bytes (%zu words changed) in %.2f ms; "
               "%d snapshots, %.1f MB\n", (long)g_head_frame, len, changed, ms, g_count + 1,
               (g_bytes + g_words * 8.0 * 2) / 1048576.0);
        fflush(stdout);
    }
}

static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (!g_job) pthread_cond_wait(&g_cv, &g_mu);
        pthread_mutex_unlock(&g_mu);
        code_job();
        pthread_mutex_lock(&g_mu);
        g_job = 0;
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
    }
    return NULL;
}

static int worker_busy(void) {
    int b;
    pthread_mutex_lock(&g_mu);
    b = g_job;
    pthread_mutex_unlock(&g_mu);
    return b;
}

static void worker_wait(void) {
    pthread_mutex_lock(&g_mu);
    while (g_job) pthread_cond_wait(&g_cv, &g_mu);
    pthread_mutex_unlock(&g_mu);
}

/* ------------------------------------------------------------------ */
/* setup                                                               */
/* ------------------------------------------------------------------ */

static size_t image_bytes(void) {
    size_t n = 0;
    int i;
    for (i = 0; i < g_nranges; i++) n += g_ranges[i].size;
    return n;
}

static int setup(void) {
    size_t bytes;
    long mb;
    if (g_init) return g_init > 0;
    g_init = -1;
    g_log = getenv("PC_REWIND_LOG") && *getenv("PC_REWIND_LOG") == '1';
    g_nranges = dbg_state_ranges(g_ranges, MAX_RANGES, negative_mode() == 3 ? NULL : k_exclude);
    if (g_nranges <= 0 || g_nranges > MAX_RANGES) {
        fprintf(stderr, "[REWIND] cannot list the machine's ranges (%d): rewind off\n", g_nranges);
        return 0;
    }
    bytes = image_bytes();
    g_words = (bytes + 7) / 8;
    g_head = dbg_mem_alloc(g_words * 8);
    g_spare = dbg_mem_alloc(g_words * 8);
    g_enc_cap = rw_delta_bound(g_words);
    g_enc = dbg_mem_alloc(g_enc_cap);
    g_cap = capacity();
    g_ring = dbg_mem_alloc((unsigned long)g_cap * sizeof *g_ring);
    mb = hal_setting_long("TOKI_REWIND_MAX_MB", 512);
    g_max_bytes = (size_t)(mb < 16 ? 16 : mb) << 20;
    if (!g_head || !g_spare || !g_enc || !g_ring) {
        fprintf(stderr, "[REWIND] out of memory: rewind off\n");
        return 0;
    }
    memset(g_head, 0, g_words * 8);    /* the padding words stay zero */
    memset(g_spare, 0, g_words * 8);
    memset(g_ring, 0, (size_t)g_cap * sizeof *g_ring);
    if (!g_thread_ok && pthread_create(&g_thread, NULL, worker, NULL) == 0) {
        pthread_detach(g_thread);
        g_thread_ok = 1;          /* one worker for the process; it waits when idle */
    }
    g_init = 1;
    printf("[REWIND] on: %d ranges, %.1f MB a snapshot, one every %d frames, %d kept (%.0f s), "
           "deltas up to %ld MB\n", g_nranges, bytes / 1048576.0, interval_frames(), g_cap,
           g_cap * interval_frames() / 60.0, mb);
    fflush(stdout);
    return 1;
}

/* ------------------------------------------------------------------ */
/* copies                                                              */
/* ------------------------------------------------------------------ */

static void copy_out(u64w *img) {
    unsigned char *p = (unsigned char *)img;
    int i;
    spu_hal_audio_lock();
    for (i = 0; i < g_nranges; i++) {
        memcpy(p, (const void *)g_ranges[i].addr, g_ranges[i].size);
        p += g_ranges[i].size;
    }
    spu_hal_audio_unlock();
}

/* PC_REWIND_NEGATIVE (negative controls of pc/tools/test_rewind.py only):
 *   spu   the SPU RAM (sample memory, the CD capture buffers) is not restored
 *   pram  PS1 RAM is not restored
 *   card  the memory card image is captured and restored (setup) */
static int negative_mode(void) {
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("PC_REWIND_NEGATIVE");
        mode = !e ? 0 : !strcmp(e, "spu") ? 1 : !strcmp(e, "pram") ? 2 : !strcmp(e, "card") ? 3 : 0;
        if (mode) fprintf(stderr, "[REWIND] NEGATIVE CONTROL %s: restores are broken on purpose\n", e);
    }
    return mode;
}

static void negative_hole(unsigned long *lo, unsigned long *hi) {
    unsigned long sz = 0;
    *lo = *hi = 0;
    if (negative_mode() == 1) {
        *lo = (unsigned long)dbg_sym_lookup("spu.cpp:_ZN3SPUL5s_ramE", &sz);
        *hi = *lo + sz;
    } else if (negative_mode() == 2) {
        *lo = 0x80000000ul;
        *hi = 0x80200000ul;
    }
}

static void copy_in(const u64w *img) {
    const unsigned char *p = (const unsigned char *)img;
    unsigned long hole_lo, hole_hi;
    int i;
    negative_hole(&hole_lo, &hole_hi);
    spu_hal_audio_lock();
    for (i = 0; i < g_nranges; i++) {
        unsigned long a = g_ranges[i].addr, e = a + g_ranges[i].size;
        if (hole_hi > a && hole_lo < e) {
            unsigned long l = hole_lo > a ? hole_lo : a, h = hole_hi < e ? hole_hi : e;
            memcpy((void *)a, p, l - a);
            memcpy((void *)h, p + (h - a), e - h);
        } else {
            memcpy((void *)a, p, g_ranges[i].size);
        }
        p += g_ranges[i].size;
    }
    spu_hal_audio_unlock();
}

/* ------------------------------------------------------------------ */
/* snapshots and restores (the boundary)                               */
/* ------------------------------------------------------------------ */

static void snapshot(void) {
    double t0, ms;
    const char *busy = dbg_state_busy();
    if (busy) { g_busy++; return; }            /* a DMA / movie in flight: next boundary */
    if (g_have_head && g_thread_ok && worker_busy()) { g_skipped++; return; }
    t0 = now_ms();
    if (!g_have_head) {
        copy_out(g_head);
        g_head_frame = frame_counter;
        g_head_vblank = hal_clock_vblanks();
        g_have_head = 1;
    } else {
        copy_out(g_spare);
        g_spare_frame = frame_counter;
        g_spare_vblank = hal_clock_vblanks();
        if (!g_thread_ok) {
            code_job();                        /* no thread: on the frame */
        } else {
            pthread_mutex_lock(&g_mu);
            g_job = 1;
            pthread_cond_broadcast(&g_cv);
            pthread_mutex_unlock(&g_mu);
        }
    }
    ms = now_ms() - t0;
    g_snaps++;
    g_copy_ms_sum += ms;
    if (ms > g_copy_ms_max) g_copy_ms_max = ms;
    if (g_log) {
        printf("[REWIND] snapshot frame %ld vblank %lu calls %lu copy %.2f ms\n",
               (long)frame_counter, (unsigned long)hal_clock_vblanks(),
               spu_audio_hash_calls ? spu_audio_hash_calls() : 0ul, ms);
        fflush(stdout);
    }
    g_next_frame = frame_counter + interval_frames();
}

/* One step back: with POP, apply the newest delta (the head becomes that
 * older snapshot); with RESTORE, the machine := the head.  -> 0, or -1
 * with nothing kept. */
static int step_back(int pop, int restore) {
    if (!g_have_head) return -1;
    if (pop) {
        Entry *e;
        if (!g_count) return -1;
        e = ring_at(g_count - 1);
        if (rw_delta_apply(g_head, g_words, e->data, e->len)) {
            fprintf(stderr, "[REWIND] a delta does not apply: the buffer is dropped\n");
            while (g_count) drop_oldest();
            return -1;
        }
        g_head_frame = e->frame;
        g_head_vblank = e->vblank;
        g_bytes -= e->len;
        dbg_mem_free(e->data);
        memset(e, 0, sizeof *e);
        g_count--;
    }
    if (restore) copy_in(g_head);
    return 0;
}

static void after_restore(void) {
    g_restores++;
    /* every observer, as a quickload tells them (trace clock, extras,
     * hi-res shadow, text assist, pacing, autosave): pc/hal/dbg_state.c */
    dbg_state_after_restore(DBG_RESTORE_REWIND);
    g_next_frame = frame_counter + interval_frames();
    if (g_log) {
        printf("[REWIND] restored frame %ld vblank %lu calls %lu (%d older kept)\n",
               (long)frame_counter, (unsigned long)hal_clock_vblanks(),
               spu_audio_hash_calls ? spu_audio_hash_calls() : 0ul, g_count);
        fflush(stdout);
    }
}

/* Toolkit: go back N steps (0 = the newest snapshot) or to the newest
 * snapshot at or before FRAME; acted on at this boundary.  -> the frame
 * restored, -1 when there is none. */
int rw_command(int steps, long frame) {
    int k;
    if (!rw_enabled() || !setup()) return -1;
    worker_wait();
    if (!g_have_head) return -1;
    if (frame >= 0) {
        if (g_head_frame <= frame) steps = 0;
        else {
            for (k = g_count - 1; k >= 0 && ring_at(k)->frame > frame; k--) {}
            if (k < 0) return -1;
            steps = g_count - k;
        }
    }
    if (steps > g_count) return -1;
    for (k = 0; k < steps; k++) step_back(1, 0);
    step_back(0, 1);                    /* one copy into the machine */
    after_restore();
    return (int)g_head_frame;
}

/* TOKI_REWIND_HOLD=F:N[:quit] (headless tests, like TOKI_FF_HOLD): at the
 * first boundary at or after game frame F the rewind key counts as held for
 * N shown steps, then released.  Once per run.  `:quit`: the window's close
 * button is pressed after the first step (an SDL_QUIT event, as the window
 * manager sends it; pc/tools/test_state_machines.py). */
static long g_sim_frame = -2;
static int g_sim_steps, g_sim_active, g_sim_quit;

static int sim_due(void) {
    if (g_sim_frame == -2) {
        const char *e = getenv("TOKI_REWIND_HOLD");
        char tail[8] = "";
        g_sim_frame = -1;
        if (e && *e && sscanf(e, "%ld:%d:%7s", &g_sim_frame, &g_sim_steps, tail) < 2) g_sim_frame = -1;
        g_sim_quit = !strcmp(tail, "quit");
    }
    if (g_sim_frame < 0 || frame_counter < g_sim_frame) return 0;
    g_sim_frame = -1;
    g_sim_active = 1;
    return 1;
}

static int any_key_down(void) {
    int n = 0, i;
    const Uint8 *k = SDL_GetKeyboardState(&n);
    for (i = 0; k && i < n; i++)
        if (k[i]) return 1;
    return 0;
}

static int key_held(void) {
    return pc_present_hotkey_held && pc_present_hotkey_held(PC_HK_REWIND);
}

/* The held key: the interactive rewind.  Holds the game at this boundary. */
static void interactive(void) {
    double step_ms = interval_frames() * 1000.0 / 60.0 / 3.0;   /* ~3x real time */
    double next;
    u32 live_vblank = hal_clock_vblanks();
    int first = 1, shown = 0, steps = 0;
    char osd[64];
    if (step_ms < 16.0) step_ms = 16.0;
    worker_wait();
    if (!g_have_head) {
        g_sim_active = 0;       /* TOKI_REWIND_HOLD fired with nothing kept: spent */
        return;
    }
    if (spu_hal_set_paused) spu_hal_set_paused(1);
    printf("[REWIND] rewinding from frame %ld\n", (long)frame_counter);
    fflush(stdout);
    next = now_ms();
    for (;;) {
        SDL_Event q;
        if (now_ms() >= next) {
            if (first || g_count) {
                step_back(!first, 1);
                first = 0;
                shown = 1;
                steps++;
                snprintf(osd, sizeof osd, "<< Rewind  -%.1f s", (live_vblank - g_head_vblank) / VBLANK_HZ);
                if (pc_present_osd) pc_present_osd(osd, 400);
            } else if (pc_present_osd) {
                pc_present_osd("<< Rewind  (oldest point)", 400);
            }
            next += step_ms;
            if (gpu_display) gpu_display();
            if (hal_get_renderer && hal_get_renderer()) SDL_RenderPresent(hal_get_renderer());
            if (pc_watchdog_presented) pc_watchdog_presented();
        }
        SDL_Delay(4);
        if (g_sim_active && g_sim_quit && steps >= 1) {
            SDL_Event qe;
            memset(&qe, 0, sizeof qe);
            qe.type = SDL_QUIT;
            SDL_PushEvent(&qe);
            g_sim_quit = 0;
        }
        SDL_PumpEvents();
        /* The close button: taken OUT of the queue (SDL_GETEVENT) and handed
         * to hal_request_quit once.  Until 2026-11-03 it was only peeked at:
         * the event stayed queued, the next frame's event poll delivered it
         * again, and that second request quit at once -- before the resume
         * state the first one had deferred the quit for was written (when
         * the save writer was busy at the boundary). */
        if (SDL_PeepEvents(&q, 1, SDL_GETEVENT, SDL_QUIT, SDL_QUIT) > 0) {
            if (hal_request_quit) hal_request_quit();
            break;
        }
        if (g_sim_active) {
            if (steps >= g_sim_steps) break;
        } else if (!key_held()) {
            break;
        }
        if (hal_is_running && !hal_is_running()) break;
    }
    g_sim_active = 0;
    if (spu_hal_set_paused) spu_hal_set_paused(0);
    /* a key still down (Z of Ctrl+Z, ...) reaches the game only once released */
    if (pad_suppress_until_release && any_key_down()) pad_suppress_until_release();
    if (shown) after_restore();
    if (pc_present_osd) pc_present_osd(shown ? "Playing from here" : "Nothing to rewind", 1200);
    printf("[REWIND] resumed at frame %ld after %d step(s)\n", (long)frame_counter, steps);
    fflush(stdout);
}

static void release_all(void) {
    worker_wait();
    pthread_mutex_lock(&g_mu);
    while (g_count) drop_oldest();
    g_have_head = 0;
    pthread_mutex_unlock(&g_mu);
}

/* Turned off (the settings menu): every buffer goes; turned on again, the
 * next boundary sets up afresh. */
static void free_all(void) {
    release_all();
    dbg_mem_free(g_head);
    dbg_mem_free(g_spare);
    dbg_mem_free(g_enc);
    dbg_mem_free(g_ring);
    g_head = g_spare = NULL;
    g_enc = NULL;
    g_ring = NULL;
    g_cap = g_first = 0;
    g_bytes = 0;
    g_init = 0;
    printf("[REWIND] off: memory released\n");
    fflush(stdout);
}

/* Every main-loop boundary (dbg_cmds.c boundary_body). */
void rw_boundary(void) {
    static int was_on;
    int on = rw_enabled();
    if (!on) {
        /* turned off in the menu: the memory goes, and a set-up that
         * failed is tried again when it is turned back on */
        if (was_on && g_init != 0) free_all();
        was_on = 0;
        return;
    }
    was_on = 1;
    if (!setup()) return;
    /* The memory card has a transfer in flight: no snapshot (its restore
     * would finish an old transfer onto the newer live card, which a
     * restore keeps) and no rewind yet (it would abandon the live transfer
     * half-written).  Card transfers take a few frames: the next boundary. */
    if (memcard_busy && memcard_busy()) {
        g_busy++;
        return;
    }
    if (key_held() || sim_due()) {
        interactive();
        return;
    }
    if (!g_have_head || frame_counter >= g_next_frame || frame_counter < g_head_frame)
        snapshot();
}

/* After any other load (quickload, the save browser, resume): the buffer
 * describes another time line -- start again.  pc/hal/quicksave.c. */
void rw_after_load(void) {
    if (g_init <= 0) return;
    release_all();
    g_next_frame = 0;
}

void rw_stats(char *out, size_t cap) {
    snprintf(out, cap, "snapshots %lu (skipped %lu busy, %lu worker busy), kept %d + head, "
             "deltas %.1f MB (max %.2f MB), image %.1f MB, copy %.2f ms avg %.2f max, "
             "code %.2f ms avg %.2f max, dropped %lu",
             g_snaps, g_busy, g_skipped, g_count, g_bytes / 1048576.0, g_delta_max / 1048576.0,
             g_words * 8 / 1048576.0, g_snaps ? g_copy_ms_sum / g_snaps : 0.0, g_copy_ms_max,
             g_snaps > 1 ? g_code_ms_sum / (g_snaps - 1) : 0.0, g_code_ms_max, g_dropped);
}
