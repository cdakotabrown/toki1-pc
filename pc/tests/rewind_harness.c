/* SPDX-License-Identifier: GPL-3.0-or-later */
/* rewind_harness.c -- the rewind's switch and boundary machine
 * (pc/hal/rewind.c) against stubs: no game, no SDL window, no save-state
 * module.  The "machine" is one small buffer; the stubs record what the
 * rewind does.  pc/tools/test_ui_state_machines.py builds it with
 * rewind.c and rewind_codec.c (and with source mutations, its negative
 * controls) and runs each mode; each case prints "ok NAME" or
 * "FAIL NAME: why", the exit status is the number of failures.
 * docs/pc-port/STATE-MACHINES.md R7 (memory card), R13 (sticky statics). */
#include "types.h"
#include "dbg_toolkit.h"
#include "pc_settings.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- the game's globals and the machine ---- */
s32 frame_counter;
static unsigned char g_machine[4096];

/* ---- stubs and their records ---- */
static PcSettings g_set;
const PcSettings *pc_settings_get(void) { return &g_set; }
int hal_switch(const char *name) { (void)name; return 1; }     /* the enhanced profile */
long hal_setting_long(const char *name, long dflt) { (void)name; return dflt; }
u32 hal_clock_vblanks(void) { return (u32)frame_counter; }
void *dbg_mem_alloc(unsigned long n) { return calloc(1, n); }
void dbg_mem_free(void *p) { free(p); }
void spu_hal_audio_lock(void) {}
void spu_hal_audio_unlock(void) {}
void *dbg_sym_lookup(const char *name, unsigned long *size) { (void)name; *size = 0; return NULL; }
const char *dbg_state_busy(void) { return NULL; }
static int g_restores_told;
void dbg_state_after_restore(int how) { (void)how; g_restores_told++; }

static int g_ranges_fail;           /* dbg_state_ranges fails this many times */
static int g_ranges_calls;
int dbg_state_ranges(DbgRange *out, int cap, const char *const *exclude) {
    (void)exclude;
    g_ranges_calls++;
    if (g_ranges_fail > 0) { g_ranges_fail--; return -1; }
    if (cap < 1) return -1;
    out[0].addr = (unsigned long)g_machine;
    out[0].size = sizeof g_machine;
    return 1;
}

static int g_card_busy;             /* memcard_busy */
int memcard_busy(void) { return g_card_busy; }

static int g_key_polls;             /* the rewind key counts as held this many polls */
int pc_present_hotkey_held(int hk) { (void)hk; return g_key_polls > 0 ? (g_key_polls--, 1) : 0; }
static int g_quit_requests;
void hal_request_quit(void) { g_quit_requests++; }

/* SDL: the interactive loop's calls */
static int g_quit_events;           /* SDL_QUIT events pushed */
void SDL_Delay(Uint32 ms) { struct timespec t = { 0, (long)ms * 1000000L }; nanosleep(&t, NULL); }
void SDL_PumpEvents(void) {}
int SDL_PushEvent(SDL_Event *e) { if (e->type == SDL_QUIT) g_quit_events++; return 1; }
int SDL_PeepEvents(SDL_Event *e, int n, SDL_eventaction a, Uint32 lo, Uint32 hi) {
    (void)n; (void)a; (void)lo; (void)hi;
    if (g_quit_events > 0) { g_quit_events--; memset(e, 0, sizeof *e); e->type = SDL_QUIT; return 1; }
    return 0;
}
const Uint8 *SDL_GetKeyboardState(int *n) { static Uint8 k[512]; if (n) *n = 512; return k; }
void SDL_RenderPresent(SDL_Renderer *r) { (void)r; }

/* rewind.c */
int rw_enabled(void);
void rw_boundary(void);
void rw_stats(char *out, size_t cap);

static int g_fail;
static void check(const char *name, int ok, const char *why) {
    if (ok) printf("ok %s\n", name);
    else { printf("FAIL %s: %s\n", name, why); g_fail++; }
}

static unsigned long snapshots(void) {
    char s[512];
    unsigned long n = 0;
    rw_stats(s, sizeof s);
    sscanf(s, "snapshots %lu", &n);
    return n;
}

static void frames(int n) {
    int i;
    for (i = 0; i < n; i++) {
        frame_counter++;
        g_machine[frame_counter % sizeof g_machine]++;
        rw_boundary();
        SDL_Delay(2);                   /* a frame's time: the coder thread runs */
    }
}

/* R13: a set-up that failed (no memory, no ranges) is tried again when the
 * player turns the rewind off and on in the menu.  Before: g_init stayed
 * -1 for the session and the rewind stayed off. */
static void mode_retry(void) {
    g_set.rewind = 1;
    g_set.rewind_interval = 1;
    g_set.rewind_seconds = 10;
    g_ranges_fail = 1;
    frames(5);
    check("setup_failed_first", g_ranges_calls == 1 && snapshots() == 0, "the failed set-up was retried every boundary");
    g_set.rewind = 0;                   /* off in the menu */
    frames(2);
    g_set.rewind = 1;                   /* and on again */
    frames(5);
    check("setup_retried_after_toggle", g_ranges_calls == 2 && snapshots() > 0,
          "turned off and on again, the rewind stayed off");
}

/* R7: no snapshot while the memory card has a transfer in flight (and no
 * rewind started then). */
static void mode_card(void) {
    unsigned long before;
    g_set.rewind = 1;
    g_set.rewind_interval = 1;
    g_set.rewind_seconds = 10;
    frames(3);
    before = snapshots();
    g_card_busy = 1;
    frames(4);
    check("no_snapshot_while_card_busy", snapshots() == before, "a snapshot was taken mid-transfer");
    g_key_polls = 3;                    /* the rewind key, while the card is busy */
    frames(1);
    check("no_rewind_while_card_busy", g_restores_told == 0, "a rewind restored mid-transfer");
    g_key_polls = 0;
    g_card_busy = 0;
    frames(2);
    check("snapshots_resume_when_idle", snapshots() > before, "no snapshot after the card went idle");
}

/* R13: TOKI_REWIND_HOLD=0:1:quit fires at the first boundary, when nothing
 * is kept yet: it is spent there.  Before, its "active" flag survived the
 * early return, and the player's first real rewind later ran as the test
 * hold -- pressing the close button for them after one step. */
static void mode_sim(void) {
    g_set.rewind = 1;
    g_set.rewind_interval = 1;
    g_set.rewind_seconds = 10;
    frames(1);                          /* the hold fires: nothing kept */
    frames(5);                          /* snapshots */
    g_key_polls = 40;                   /* the player holds the rewind key a moment */
    frame_counter++;
    rw_boundary();                      /* the interactive rewind, until released */
    check("player_rewind_is_not_the_test_hold", g_quit_requests == 0,
          "the player's rewind pressed the close button (a stale test hold)");
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "";
    unsetenv("TOKI_REWIND");            /* the settings decide (the player's case) */
    if (!strcmp(mode, "sim")) setenv("TOKI_REWIND_HOLD", "0:1:quit", 1);
    else unsetenv("TOKI_REWIND_HOLD");
    if (!strcmp(mode, "retry")) mode_retry();
    else if (!strcmp(mode, "card")) mode_card();
    else if (!strcmp(mode, "sim")) mode_sim();
    else { fprintf(stderr, "usage: %s retry|card|sim\n", argv[0]); return 2; }
    return g_fail;
}
