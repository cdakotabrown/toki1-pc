/* SPDX-License-Identifier: GPL-3.0-or-later */
/* save_auto_harness.c -- the autosave / ending save / resume / quit machine
 * (pc/hal/save_auto.c) against stubs: no game, no save states, no SDL.
 * pc/tools/test_state_machines.py builds it (with -DSA_RAM8/-DSA_RAM16
 * pointing PS1 RAM reads at g_ram below) and runs every case; each prints
 * "ok NAME" or "FAIL NAME: why".  docs/pc-port/STATE-MACHINES.md. */
#include "types.h"
#include "pc_settings.h"
#include "quicksave.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- PS1 RAM the machine reads: the date (0x800EE79E..A0), pass_days (A2) */
unsigned char g_ram[16];
#define RAMOFF(a) ((unsigned long)(a) - 0x800EE79Eul)

/* ---- the game's globals ---- */
s32 frame_counter;
u8 now_step, now_sub_step, now_sub_sub_step;

/* ---- stubs and their records ---- */
static PcSettings g_set;
const PcSettings *pc_settings_get(void) { return &g_set; }
const char *pc_save_dir(void) { return "/nonexistent"; }
const char *dbg_state_unavailable(void) { return NULL; }
void dbg_toolkit_activate(void) {}
void dbg_state_prepare(void) {}
void save_browser_resume_prompt(void) {}

static int g_in_game = 1;           /* qs_game_in_progress */
static int g_busy;                  /* qs_save_now answers "busy" this many times */
static int g_quits, g_saves, g_loads, g_deletes, g_resume_exists;
static char g_last_save[QS_ID_MAX];
int qs_game_in_progress(void) { return g_in_game; }
int qs_entry_read(const char *id, QsEntry *e) {
    memset(e, 0, sizeof *e);
    if (!strcmp(id, "resume") && g_resume_exists) { e->has_state = 1; return 1; }
    return 0;
}
int qs_save_now(const char *id, int quiet) {
    (void)quiet;
    if (g_busy > 0) { g_busy--; return 1; }
    g_saves++;
    snprintf(g_last_save, sizeof g_last_save, "%s", id);
    if (!strcmp(id, "resume")) g_resume_exists = 1;
    return 0;
}
int qs_load_now(const char *id, int quiet) { (void)id; (void)quiet; g_loads++; return 0; }
int qs_delete_id(const char *id) { if (!strcmp(id, "resume")) g_resume_exists = 0; g_deletes++; return 0; }
void qs_writer_wait(void) {}
void qs_notify(const char *t) { (void)t; }
void hal_quit_now(void) { g_quits++; }

/* save_auto.c */
void sa_frame(void);
void sa_boundary(void);
void sa_after_load(void);
int sa_defer_quit(void);
int sa_quit_pending(void);
const char *sa_state_name(void);
unsigned sa_illegal_transitions(void);

static int g_fail;
#define CHECK(name, cond) do { if (!(cond)) { printf("FAIL %s: %s (line %d)\n", name, #cond, __LINE__); g_fail = 1; return; } } while (0)

static int g_month = 4;
static void set_date(int year, int pass_days) {
    g_ram[0] = (unsigned char)year;
    g_ram[1] = (unsigned char)g_month;
    g_ram[2] = 9;
    g_ram[4] = (unsigned char)pass_days;
    g_ram[5] = (unsigned char)(pass_days >> 8);
}

/* one game frame: a VSync (sa_frame) and a main-loop boundary, where the
 * toolkit runs qs_boundary -> sa_boundary twice (pc/hal/dbg_cmds.c) */
static void frame(int step, int sub) {
    now_step = (u8)step;
    now_sub_step = (u8)sub;
    frame_counter++;
    sa_frame();
    sa_boundary();
    sa_boundary();
}

static void reset_counts(void) { g_quits = g_saves = g_loads = g_deletes = 0; g_busy = 0; }

/* the planner autosaves once per date, at its first 0x34/1 boundary */
static void week(void) {
    reset_counts();
    set_date(95, 10);
    frame(0x30, 0);
    frame(0x34, 0);
    CHECK("week", !strcmp(sa_state_name(), "week-armed"));
    frame(0x34, 0);
    CHECK("week", g_saves == 0);
    frame(0x34, 1);
    CHECK("week", g_saves == 1 && !strncmp(g_last_save, "auto", 4));
    CHECK("week", !strcmp(sa_state_name(), "idle"));
    frame(0x34, 1);
    frame(0x30, 0);
    frame(0x34, 1);
    CHECK("week", g_saves == 1);            /* the same date: not again */
    set_date(95, 17);
    frame(0x30, 0);
    frame(0x34, 1);
    frame(0x34, 1);
    CHECK("week", g_saves == 2);
    printf("ok week\n");
}

/* left before a quiet boundary: no save, the date counts as done */
static void week_left(void) {
    reset_counts();
    set_date(95, 24);
    frame(0x34, 0);
    frame(0x30, 0);
    CHECK("week_left", g_saves == 0 && !strcmp(sa_state_name(), "idle"));
    frame(0x34, 1);
    frame(0x34, 1);
    CHECK("week_left", g_saves == 0);
    printf("ok week_left\n");
}

/* [saves] autosave_every = month (the default): the first planner of each
 * calendar month only */
static void month(void) {
    reset_counts();
    g_set.autosave_every = PC_AUTOSAVE_MONTH;
    g_month = 5;
    set_date(95, 40);
    frame(0x30, 0);
    frame(0x34, 1);
    frame(0x34, 1);
    CHECK("month", g_saves == 1);
    set_date(95, 47);                       /* the next week, the same month */
    frame(0x30, 0);
    frame(0x34, 1);
    frame(0x34, 1);
    CHECK("month", g_saves == 1);
    g_month = 6;
    set_date(95, 54);                       /* a new month */
    frame(0x30, 0);
    frame(0x34, 1);
    frame(0x34, 1);
    CHECK("month", g_saves == 2);
    g_set.autosave_every = PC_AUTOSAVE_WEEK;
    g_month = 4;
    printf("ok month\n");
}

/* a load drops an armed save; the loaded date counts as saved */
static void load_disarms(void) {
    reset_counts();
    set_date(95, 31);
    frame(0x34, 0);
    CHECK("load_disarms", !strcmp(sa_state_name(), "week-armed"));
    sa_after_load();
    CHECK("load_disarms", !strcmp(sa_state_name(), "idle"));
    frame(0x34, 1);
    frame(0x34, 1);
    CHECK("load_disarms", g_saves == 0);
    printf("ok load_disarms\n");
}

/* the ending: armed on entering step 99 in 1998, saved at a quiet
 * boundary; a busy machine is waited for ARM_LIMIT game FRAMES -- not
 * boundary calls (the toolkit calls sa_boundary twice per boundary; until
 * 2026-11-03 that halved the wait) */
static void ending_wait_counts_frames(void) {
    int i;
    reset_counts();
    set_date(98, 1000);
    frame(0x30, 0);
    g_busy = 1300 * 2;                      /* 1300 frames of busy captures */
    frame(99, 0);
    CHECK("ending_wait", !strcmp(sa_state_name(), "ending-armed"));
    for (i = 0; i < 1400 && g_saves == 0; i++) frame(99, 0);
    CHECK("ending_wait", g_saves == 1 && !strcmp(g_last_save, "ending"));
    printf("ok ending_wait_counts_frames\n");
}

/* a quit during a game waits for the boundary, writes `resume`, quits once */
static void quit_writes_resume(void) {
    reset_counts();
    g_resume_exists = 0;
    frame(0x30, 0);
    CHECK("quit", sa_defer_quit() == 1 && sa_quit_pending());
    g_busy = 2;
    frame(0x30, 0);
    CHECK("quit", g_quits == 0 && sa_quit_pending());
    frame(0x30, 0);
    CHECK("quit", g_quits == 1 && g_resume_exists && !strcmp(sa_state_name(), "quitting"));
    CHECK("quit", sa_defer_quit() == 0);    /* a second request: at once */
    frame(0x30, 0);
    CHECK("quit", g_quits == 1);
    printf("ok quit_writes_resume\n");
}

/* a quit that never reaches a boundary (PC_QUIT_WAIT_MS=0 here) quits on
 * the timeout -- and the previous session's resume state is not left to be
 * offered as "where you left off" (fixed 2026-11-03) */
static void quit_timeout_deletes_older_resume(void) {
    reset_counts();
    g_resume_exists = 1;                    /* the previous session's */
    CHECK("quit_timeout", sa_defer_quit() == 1);
    now_step = 0x30;
    sa_frame();                             /* no boundary: the timeout */
    CHECK("quit_timeout", g_quits == 1);
    CHECK("quit_timeout", !g_resume_exists);
    printf("ok quit_timeout_deletes_older_resume\n");
}

/* every refused transition is counted: none in the cases above */
static void no_illegal(void) {
    CHECK("no_illegal", sa_illegal_transitions() == 0);
    printf("ok no_illegal\n");
}

int main(int argc, char **argv) {
    const char *only = argc > 1 ? argv[1] : NULL;
    g_set.autosave = g_set.autosave_ending = g_set.resume = 1;
    g_set.autosave_slots = 3;
    g_set.autosave_every = PC_AUTOSAVE_WEEK;
    g_set.harness = 0;
    if (!only || !strcmp(only, "machine")) {
        week();
        week_left();
        month();
        load_disarms();
        ending_wait_counts_frames();
        quit_writes_resume();
        no_illegal();
    }
    if (only && !strcmp(only, "timeout")) quit_timeout_deletes_older_resume();
    return g_fail;
}
