/* SPDX-License-Identifier: GPL-3.0-or-later */
/* save_auto.c -- the saves the PC layer makes on its own (quicksave.h):
 *
 *  - the WEEKLY AUTOSAVE: at each week's planning screen, into the rotating
 *    entries auto1..autoN ([saves] autosave, autosave_slots);
 *  - the ENDING save: when the ending starts ([saves] autosave_ending);
 *  - the RESUME state: on quit, offered at the next launch ([saves] resume,
 *    the launcher's --resume = TOKI_RESUME=1).
 *
 * Evidence for the two game moments (docs/pc-port/ENHANCED-MODE.md
 * "Autosave and resume"):
 *  - The planner is now_step 0x34.  Each week the game enters it as
 *    0x34/0/0..3 (set-up and fade-in), then waits for the player in
 *    0x34/1/0 and leaves to 0x30 once the week is chosen.  Measured on
 *    spring_events.txt: entered on every Sunday night 9 Apr .. 11 Jun 1995
 *    and on the first day (Tue 4 Apr, via 0x34/0xFF), once per date, with
 *    the calendar (pass_days 0x800EE7A2) already on the new date.  The
 *    week key is pass_days: one autosave per planner date.
 *  - The ending is now_step 99: schedule_day_main's schedule_exit calls
 *    goto_step(99) the first time it runs with the date >= 1998-03-01, and
 *    the ending is chosen inside the ENDING overlay (ending_select_init),
 *    from the save block alone (docs/game-flows/ENDINGS-GUIDE.md section 1).
 *    A state taken at the first quiet boundary of step 99 therefore replays
 *    graduation day and the confession.
 *
 * Safety.  A save is taken at a main-loop boundary only (no game code on
 * the stack, quicksave.c), only while the memory card has no request in
 * flight (memcard_busy) and the CD drive no command, seek or CdRead
 * (hal_cd_quiet), and dbg_state_capture itself defers while an SPU DMA or a
 * movie stream holds a heap buffer.  Writing a state only READS the
 * machine (the capture copies memory; the file is written on a worker
 * thread), so the game runs exactly as without it -- checked frame for
 * frame (pc/tools/test_autosave.py).  Never in automated runs (the
 * settings' harness rule) unless a test forces it (PC_AUTOSAVE_FORCE=1,
 * PC_RESUME_FORCE=1).  The detector (sa_frame) reads four bytes of PS1 RAM
 * per VBlank and wakes the boundary (dbg_toolkit_activate) only when a
 * save is due.
 *
 * Statics are the player's session: kept out of a restore (dbg_state.c
 * KEEP_FILES), and adjusted after a load (sa_after_load) so loading a state
 * taken AT the planner does not autosave the same week again at once.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "types.h"
#include "dbg_toolkit.h"
#include "pc_settings.h"
#include "quicksave.h"

extern s32 frame_counter;
extern u8 now_step, now_sub_step, now_sub_sub_step;
extern const char *pc_save_dir(void);
extern void hal_quit_now(void);
extern void dbg_toolkit_activate(void);
/* pc/hal/cd.c, memcard.c, present.c; weak for the harness binaries */
extern int hal_cd_quiet(void) __attribute__((weak));
extern int memcard_busy(void) __attribute__((weak));
/* a note (queued, timed while on screen: pc/include/pc_present.h "Notes") */
extern void pc_present_note(const char *text, unsigned ms) __attribute__((weak));

/* PS1 RAM reads (the harness, pc/tests/save_auto_harness.c, builds with
 * -DSA_HARNESS_RAM=name: an array of its own from 0x800EE79E) */
#ifdef SA_HARNESS_RAM
extern unsigned char SA_HARNESS_RAM[];
#define SA_RAM8(a) (SA_HARNESS_RAM[(unsigned long)(a) - 0x800EE79Eul])
#define SA_RAM16(a) ((int)(SA_RAM8(a) | SA_RAM8((a) + 1) << 8))
#else
#define SA_RAM8(a) (*(volatile const u8 *)(unsigned long)(a))
#define SA_RAM16(a) (*(volatile const u16 *)(unsigned long)(a))
#endif
#define PASS_DAYS() ((int)SA_RAM16(0x800EE7A2))
#define NOW_YEAR() ((int)SA_RAM8(0x800EE79E))      /* now_year */
#define NOW_MONTH() ((int)SA_RAM8(0x800EE79F))     /* now_month */

/* The autosave period's key: the calendar month of the planner's date
 * (default: one autosave at the first planner of each month, the week that
 * crosses into it) or pass_days (every week's planner). */
static int every_week(void) {
    /* TOKI_AUTOSAVE_EVERY=week|month wins (automated runs ignore settings.ini) */
    const char *e = getenv("TOKI_AUTOSAVE_EVERY");
    if (e && *e) return !strcmp(e, "week");
    return pc_settings_get()->autosave_every == PC_AUTOSAVE_WEEK;
}

static int period_key(void) {
    if (every_week()) return PASS_DAYS();
    return 100000 + NOW_YEAR() * 12 + NOW_MONTH();
}

#define STEP_PLANNER 0x34
#define STEP_ENDING 99
#define QUIT_WAIT_MS 4000       /* longest a quit waits for the resume state */
#define ARM_LIMIT 1800          /* game frames an armed ending save waits for quiet */

/* ---- the machine (docs/pc-port/STATE-MACHINES.md "Autosave, ending save,
 * resume and quit").  One state; every change goes through sa_go(), which
 * refuses (and logs) a change the table below does not allow.  Until
 * 2026-11-03 it was six flags (g_week_armed, g_ending_armed,
 * g_quit_pending 0/1/2, g_resume_load, ...) set from five functions.
 *
 *   from \ to       IDLE  RESUME_LOAD WEEK_ARMED ENDING_ARMED QUIT_PENDING QUITTING
 *   IDLE             -        x          x           x            x          x
 *   RESUME_LOAD      x        -          .           .            x          x
 *   WEEK_ARMED       x        .          -           x            x          x
 *   ENDING_ARMED     x        .          .           -            x          x
 *   QUIT_PENDING     .        .          .           .            -          x
 *   QUITTING         .        .          .           .            .          -
 *
 * Events: sa_frame (every VSync: the resume request at the first one, the
 * planner / ending detectors, the quit timeout), sa_boundary (every
 * main-loop boundary: the resume load, the armed save, the resume write),
 * sa_after_load (any restore: an armed save is dropped), sa_defer_quit (the
 * window's close button / Ctrl+Q).  A quit supersedes an armed save; a
 * load never cancels a quit. */
typedef enum {
    SA_IDLE,            /* watching for the planner and the ending */
    SA_RESUME_LOAD,     /* TOKI_RESUME=1: load `resume` at the next boundary */
    SA_WEEK_ARMED,      /* the planner: save at its first quiet 0x34/1 boundary */
    SA_ENDING_ARMED,    /* the ending began: save at its first quiet boundary */
    SA_QUIT_PENDING,    /* a quit waits for the resume state (next boundary, or the timeout) */
    SA_QUITTING,        /* hal_quit_now() was called */
    SA_STATES
} SaState;

static const char *const k_sa_names[SA_STATES] = {
    "idle", "resume-load", "week-armed", "ending-armed", "quit-pending", "quitting"
};

static const unsigned char k_sa_allowed[SA_STATES][SA_STATES] = {
    /* IDLE */         { 0, 1, 1, 1, 1, 1 },
    /* RESUME_LOAD */  { 1, 0, 0, 0, 1, 1 },
    /* WEEK_ARMED */   { 1, 0, 0, 1, 1, 1 },
    /* ENDING_ARMED */ { 1, 0, 0, 0, 1, 1 },
    /* QUIT_PENDING */ { 0, 0, 0, 0, 0, 1 },
    /* QUITTING */     { 0, 0, 0, 0, 0, 0 },
};

static SaState g_state = SA_IDLE;
static unsigned g_illegal;      /* refused transitions (tests) */
static int g_last_week = -1;    /* period_key() of the last autosave (or skip) */
static int g_ending_prev_step = -1;
static int g_waited;            /* game frames the armed save has waited for quiet */
static s32 g_waited_frame = -1; /* ... counted once per game frame (a boundary may run
                                 * sa_boundary more than once: pc/hal/dbg_cmds.c) */
static struct timespec g_quit_t0;
static int g_frame_init;
static int g_seen_game;         /* this session has been in a game */

/* The one transition function: 1 = done, 0 = refused (logged). */
static int sa_go(SaState to, const char *why) {
    if (to == g_state) return 1;
    if (!k_sa_allowed[g_state][to]) {
        g_illegal++;
        printf("[AUTOSAVE] state %s -> %s refused (%s)\n", k_sa_names[g_state], k_sa_names[to], why);
        fflush(stdout);
        return 0;
    }
    if (getenv("PC_SA_LOG"))
        printf("[AUTOSAVE] state %s -> %s (%s)\n", k_sa_names[g_state], k_sa_names[to], why);
    g_state = to;
    if (to == SA_WEEK_ARMED || to == SA_ENDING_ARMED) {
        g_waited = 0;
        g_waited_frame = -1;
    }
    return 1;
}

/* An armed save found the machine busy at this boundary: one more frame
 * waited (not one more call). */
static void waited_one(void) {
    if (g_waited_frame == frame_counter) return;
    g_waited_frame = frame_counter;
    g_waited++;
}

/* tests (pc/tests/save_auto_harness.c) */
const char *sa_state_name(void) { return k_sa_names[g_state]; }
unsigned sa_illegal_transitions(void) { return g_illegal; }

static int forced(const char *var) {
    const char *e = getenv(var);
    return e && *e == '1';
}

/* Save states work in this executable (pc/hal/dbg_state.c): an
 * executable that cannot take them never arms an automatic save, so it
 * cannot fail one every week; the reason is logged once. */
static int states_work(void) {
    static int known = -1;
    if (known < 0) {
        const char *why = dbg_state_unavailable();
        known = !why;
        if (why) printf("[AUTOSAVE] save states are not available (%s): no autosave, ending save "
                        "or resume state\n", why);
    }
    return known;
}

int sa_autosave_enabled(void) {
    const PcSettings *s = pc_settings_get();
    return s->autosave && (!s->harness || forced("PC_AUTOSAVE_FORCE")) && states_work();
}

static int ending_enabled(void) {
    const PcSettings *s = pc_settings_get();
    return s->autosave_ending && (!s->harness || forced("PC_AUTOSAVE_FORCE")) && states_work();
}

int sa_resume_enabled(void) {
    const PcSettings *s = pc_settings_get();
    return s->resume && (!s->harness || forced("PC_RESUME_FORCE")) && states_work();
}

static double ms_since(const struct timespec *t0) {
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (t1.tv_sec - t0->tv_sec) * 1000.0 + (t1.tv_nsec - t0->tv_nsec) / 1e6;
}

/* PC_QUIT_WAIT_MS: the tests' shorter wait */
static double quit_wait_ms(void) {
    static long ms = -1;
    if (ms < 0) {
        const char *e = getenv("PC_QUIT_WAIT_MS");
        ms = e && *e ? atol(e) : QUIT_WAIT_MS;
        if (ms < 0) ms = QUIT_WAIT_MS;
    }
    return (double)ms;
}

static int machine_quiet(const char **why) {
    if (memcard_busy && memcard_busy()) { *why = "memory card busy"; return 0; }
    if (hal_cd_quiet && !hal_cd_quiet()) { *why = "CD command in flight"; return 0; }
    return 1;
}

/* the weekly slot to write: the first empty one, else the oldest */
static void pick_auto(char *id, size_t cap) {
    int n = pc_settings_get()->autosave_slots, i, best = 1;
    long long oldest = -1;
    if (n < 1) n = 1;
    if (n > 9) n = 9;
    for (i = 1; i <= n; i++) {
        QsEntry e;
        char cand[QS_ID_MAX];
        snprintf(cand, sizeof cand, "auto%d", i);
        if (!qs_entry_read(cand, &e) || !e.has_state) { best = i; break; }
        if (oldest < 0 || e.saved_ms < oldest) { oldest = e.saved_ms; best = i; }
    }
    snprintf(id, cap, "auto%d", best);
}

/* The quit is over: the loop ends at its next check. */
static void quit_done(void) {
    if (sa_go(SA_QUITTING, "quit")) hal_quit_now();
}

/* Called from hal_frame_begin (pc/hal/system.c) at every VSync: cheap. */
void sa_frame(void) {
    if (!g_frame_init) {
        const char *r = getenv("TOKI_RESUME");
        g_frame_init = 1;
        if (r && *r == '1' && sa_go(SA_RESUME_LOAD, "TOKI_RESUME=1"))
            dbg_toolkit_activate();
    }
    if (g_state == SA_QUIT_PENDING && ms_since(&g_quit_t0) > quit_wait_ms()) {
        QsEntry e;
        printf("[RESUME] no main-loop boundary within %.0f ms: quitting without a resume state\n",
               quit_wait_ms());
        /* the previous session's resume would be offered next time as
         * "where you left off": never an older moment (as when the write
         * fails, sa_boundary) */
        qs_writer_wait();
        if (qs_entry_read("resume", &e)) {
            qs_delete_id("resume");
            printf("[RESUME] the older resume state is deleted\n");
        }
        fflush(stdout);
        quit_done();
        return;
    }
    if (!g_seen_game && qs_game_in_progress()) g_seen_game = 1;
    if (g_state == SA_IDLE && now_step == STEP_PLANNER && sa_autosave_enabled() &&
        qs_game_in_progress() && period_key() != g_last_week &&
        sa_go(SA_WEEK_ARMED, "the planner"))
        dbg_toolkit_activate();
    if ((g_state == SA_IDLE || g_state == SA_WEEK_ARMED) && now_step == STEP_ENDING &&
        g_ending_prev_step >= 0 && g_ending_prev_step != STEP_ENDING && ending_enabled() &&
        SA_RAM8(0x800EE79E) == 98) {
        if (g_state == SA_WEEK_ARMED) {
            /* the planner was left for the ending before a boundary saw it */
            printf("[AUTOSAVE] planner left before a quiet boundary (pass_days %d): no autosave "
                   "this week\n", PASS_DAYS());
            g_last_week = period_key();
        }
        if (sa_go(SA_ENDING_ARMED, "the ending began")) dbg_toolkit_activate();
    }
    g_ending_prev_step = now_step;
}

static void date_text(char *out, size_t cap) {
    snprintf(out, cap, "%u-%02u-%02u", 1900u + SA_RAM8(0x800EE79E), SA_RAM8(0x800EE79F),
             SA_RAM8(0x800EE7A0));
}

static void boundary_resume_load(void) {
    QsEntry e;
    sa_go(SA_IDLE, "resume loaded");
    if (!qs_entry_read("resume", &e) || !e.has_state) {
        qs_notify("Nothing to resume");
    } else if (qs_load_now("resume", 0) == 0) {
        printf("[RESUME] resumed: %s %s\n", e.saved, e.in_progress ? "(in a game)" : "");
    }
    fflush(stdout);
}

static void boundary_quit(void) {
    int r;
    if (!qs_game_in_progress()) {
        /* left the game since the quit was asked (a load acted on first) */
        QsEntry e;
        qs_writer_wait();
        if (g_seen_game && qs_entry_read("resume", &e)) qs_delete_id("resume");
        printf("[RESUME] no game in progress at the boundary: quitting without a resume state\n");
        fflush(stdout);
        quit_done();
        return;
    }
    r = qs_save_now("resume", 1);
    if (r > 0) return;                          /* busy: the next boundary (or the timeout) */
    qs_writer_wait();
    printf("[RESUME] %s at frame %ld (scene %u/%u/%u); quitting\n",
           r == 0 ? "resume state written" : "resume state NOT written", (long)frame_counter,
           now_step, now_sub_step, now_sub_sub_step);
    if (r) qs_delete_id("resume");              /* never offer an older moment */
    fflush(stdout);
    quit_done();
}

static void boundary_week(void) {
    const char *why = "";
    char id[QS_ID_MAX], date[16];
    int r;
    if (now_step != STEP_PLANNER || !sa_autosave_enabled() || !qs_game_in_progress()) {
        printf("[AUTOSAVE] planner left before a quiet boundary (pass_days %d): no autosave "
               "this week\n", PASS_DAYS());
        g_last_week = period_key();
        sa_go(SA_IDLE, "planner left");
    } else if (now_sub_step == 1) {
        /* the planner waiting for the player (0x34/1): the screen is up */
        if (!machine_quiet(&why)) {
            if (++g_waited == 1 || g_waited % 300 == 0)
                printf("[AUTOSAVE] waiting: %s\n", why);
        } else {
            qs_writer_wait();           /* the slots' indexes are current */
            pick_auto(id, sizeof id);
            r = qs_save_now(id, 0);
            if (r <= 0) {
                date_text(date, sizeof date);
                printf("[AUTOSAVE] %s %s (pass_days %d) -> %s at frame %ld, scene %u/%u/%u, "
                       "after %d busy boundaries%s\n",
                       every_week() ? "week of" : "month of",
                       date, PASS_DAYS(), id, (long)frame_counter,
                       now_step, now_sub_step, now_sub_sub_step, g_waited, r ? ": FAILED" : "");
                g_last_week = period_key();
                sa_go(SA_IDLE, "week saved");
            } else {
                g_waited++;
            }
        }
    }
    fflush(stdout);
}

static void boundary_ending(void) {
    const char *why = "";
    char date[16];
    int r;
    if (now_step != STEP_ENDING || g_waited > ARM_LIMIT) {
        printf("[AUTOSAVE] ending: no quiet boundary in step %d: not saved\n", STEP_ENDING);
        sa_go(SA_IDLE, "ending: no quiet boundary");
    } else if (machine_quiet(&why)) {
        r = qs_save_now("ending", 0);
        if (r <= 0) {
            date_text(date, sizeof date);
            printf("[AUTOSAVE] ending (%s) -> ending at frame %ld, scene %u/%u/%u%s\n", date,
                   (long)frame_counter, now_step, now_sub_step, now_sub_sub_step,
                   r ? ": FAILED" : "");
            sa_go(SA_IDLE, "ending saved");
        } else {
            waited_one();
        }
    } else {
        waited_one();
    }
    fflush(stdout);
}

/* qs_boundary calls this at every main-loop boundary (toolkit stack). */
void sa_boundary(void) {
    if (g_state == SA_RESUME_LOAD) boundary_resume_load();
    switch (g_state) {
    case SA_QUIT_PENDING: boundary_quit(); break;
    case SA_WEEK_ARMED: boundary_week(); break;
    case SA_ENDING_ARMED: boundary_ending(); break;
    default: break;
    }
}

void sa_after_load(void) {
    if (qs_game_in_progress()) g_seen_game = 1;
    /* the week (or ending) of the state just loaded is already saved */
    g_last_week = qs_game_in_progress() ? period_key() : -1;
    if (g_state == SA_WEEK_ARMED || g_state == SA_ENDING_ARMED) sa_go(SA_IDLE, "a state was loaded");
    g_ending_prev_step = now_step;
}

/* ---- quitting ---- */

/* hal_request_quit (window close, Ctrl+Q): 1 = deferred until the resume
 * state is written at the next boundary; 0 = quit now. */
int sa_defer_quit(void) {
    if (g_state == SA_QUIT_PENDING || g_state == SA_QUITTING) return 0;   /* a second request: at once */
    if (!sa_resume_enabled()) return 0;
    if (!qs_game_in_progress()) {
        /* nothing to resume: the last quit was here, not in a game.  A
         * session that never got into a game (closed at the "continue?"
         * prompt, or at the title) leaves the old resume state alone. */
        QsEntry e;
        qs_writer_wait();                       /* a save of `resume` in flight */
        if (g_seen_game && qs_entry_read("resume", &e)) qs_delete_id("resume");
        return 0;
    }
    if (!sa_go(SA_QUIT_PENDING, "quit asked")) return 0;
    clock_gettime(CLOCK_MONOTONIC, &g_quit_t0);
    qs_notify("Saving where you are...");
    dbg_toolkit_activate();
    return 1;
}

int sa_quit_pending(void) { return g_state == SA_QUIT_PENDING; }

/* ---- launch ---- */

/* At start-up (the presenter, once the menu exists): offer the resume
 * state, or say why it cannot be used. */
void sa_launch(void) {
    QsEntry e;
    const char *r = getenv("TOKI_RESUME");
    /* the first save's symbol-table load now, not on a frame (interactive
     * runs only: an automated run stays as it was) */
    if (!pc_settings_get()->harness) dbg_state_prepare();
    if (r && *r == '1') return;                 /* --resume: loaded at the first boundary */
    if (!sa_resume_enabled()) return;
    if (!qs_entry_read("resume", &e) || !e.has_state) return;
    if (!e.has_index || !e.same_build) {
        /* another build's (or one without its index, which cannot be
         * checked): never offered, set aside where the player can see it */
        char a[PC_SETTINGS_PATH_MAX + 64], b[PC_SETTINGS_PATH_MAX + 64];
        static const char *const exts[] = { "tks", "png", "ini" };
        int i;
        printf("[RESUME] the resume state was saved by another build%s; kept as resume-old.*\n",
               e.has_index ? "" : " (or has no index)");
        for (i = 0; i < 3; i++) {
            snprintf(a, sizeof a, "%s/quicksave/resume.%s", pc_save_dir(), exts[i]);
            snprintf(b, sizeof b, "%s/quicksave/resume-old.%s", pc_save_dir(), exts[i]);
            rename(a, b);
        }
        if (pc_present_note)
            pc_present_note("Your last session cannot be resumed:\n"
                           "it was saved by another build of the game.\n"
                           "Continue from your memory card save.", 8000);
        fflush(stdout);
        return;
    }
    if (pc_settings_get()->harness) {
        /* an automated run (PC_RESUME_FORCE) has no one to answer: the
         * question would hold it in the pause loop for ever */
        printf("[RESUME] a resume state exists (%s); not offered in an automated run\n", e.saved);
        fflush(stdout);
        return;
    }
    printf("[RESUME] offering the resume state (%s)\n", e.saved);
    fflush(stdout);
    save_browser_resume_prompt();
}
