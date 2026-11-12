/* SPDX-License-Identifier: GPL-3.0-or-later */
/* extras.h -- persistent extras: the endings seen, the gallery of
 * pictures, achievements (the enhanced profile's TOKI_EXTRAS;
 * docs/pc-port/ENHANCED-MODE.md "Extras", PLAYER-GUIDE.md "Extras:
 * endings, gallery, achievements").
 *
 * Everything is OBSERVED: once per game frame the detectors read the game
 * (pc/hal/extras_state.h) and the gallery watches uploads and draws
 * (pc/hal/extras_gallery.c); nothing writes PS1 RAM, VRAM or any state the
 * game reads, so a run with extras on is frame for frame the run with them
 * off (pc/tools/test_extras.py).
 *
 * Storage: <save dir>/extras/ (pc_save_dir(), pc/hal/save_paths.c):
 *   extras.txt        the record (format below), rewritten atomically
 *                     (extras.txt.tmp, then rename) on every change;
 *   gallery/HASH.png  each picture, 24-bit, written the same way.
 *
 * extras.txt, one record per line, fields separated by single spaces:
 *   # comment
 *   format 1
 *   ending UNIX ID YYYY-MM-DD MET TOKIMEKI YUUKOU BOMB P0 .. P8
 *       one line per ending seen, oldest first: host time, now_girl,
 *       the in-game date, girls met, the chosen girl's three gauges
 *       (0 for Yoshio), the nine parameters
 *   achievement KEY UNIX
 *   picture HASH KIND W H UNIX STEP GIRL YYYY-MM-DD SOURCE
 *       KIND 0 = a still picture (BS/), 1 = an event scene (EVENT/);
 *       HASH (16 hex digits) names gallery/HASH.png; GIRL -1 = none
 * Unknown lines are kept as they are (a newer build's records survive an
 * older build's rewrite). */
#ifndef PC_EXTRAS_H
#define PC_EXTRAS_H

#include "extras_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The profile switch TOKI_EXTRAS is on and the run is not automated
 * (the settings' harness rule) unless PC_EXTRAS_FORCE=1. */
int extras_active(void);

/* hal_frame_end (pc/hal/system.c): one game frame -- read the view and run
 * the detectors; the gallery's frame end. */
void extras_frame_end(void);
/* quicksave.c after a state load: the detectors start over from the
 * loaded moment (no "new" ending or picture from the jump itself). */
void extras_after_load(void);

/* ---- the record (the view pages read it) ---- */
#define EX_MAX_ENDING_RUNS 256
#define EX_MAX_PICTURES 1024
#define EX_SOURCE_MAX 40

typedef struct {
    long long unix_time;
    int id;
    int year, month, day;
    int met;
    int tokimeki, yuukou, bomb;
    int params[EX_PARAMS];
} ExEndingRun;

typedef struct {
    char hash[17];
    int kind;                   /* EX_PIC_STILL / EX_PIC_EVENT */
    int w, h;
    long long unix_time;
    int step, girl;
    int year, month, day;
    char source[EX_SOURCE_MAX]; /* "BS/D_09_02.BS", "EVENT/EV_FU01.BIN" */
} ExPicture;
enum { EX_PIC_STILL, EX_PIC_EVENT, EX_PIC_KINDS };

typedef struct {
    const char *key;            /* stable id in extras.txt */
    const char *name;           /* shown (5x7 font) */
    const char *how;            /* one line: what unlocks it */
} ExAchievement;

/* Read the record now (the pages: shown under either profile). */
void extras_store_ensure(void);
const char *ex_dir(void);                       /* <save dir>/extras */
int ex_ending_runs(void);
const ExEndingRun *ex_ending_run(int i);
/* times ending E was seen; *last = its newest run (NULL when none) */
int ex_ending_seen(int e, const ExEndingRun **last);
int ex_endings_distinct(void);
const char *ex_ending_name(int e);              /* 14 names */

int ex_achievement_count(void);
const ExAchievement *ex_achievement(int i);
long long ex_achievement_time(int i);           /* 0 = locked */
int ex_achievements_unlocked(void);
/* achievements are opt-in: [enhanced] achievements (TOKI_ACHIEVEMENTS wins) */
int ex_achievements_on(void);

int ex_picture_count(void);
const ExPicture *ex_picture(int i);
/* distinct disc sources of KIND seen / on the disc (g_file_lba_table) */
int ex_pictures_seen(int kind);
int ex_pictures_total(int kind);
/* gallery/HASH.png into RGB (W*H*3); 0 on success */
int ex_picture_read(const ExPicture *p, unsigned char *rgb);

/* ---- the engine, exposed for pc/tests/test_extras.c ---- */
/* Load the record from DIR ("" = forget everything); 0 = ok or none yet. */
int ex_store_open(const char *dir);
int ex_store_save(void);
/* One game frame's view. */
void ex_observe(const ExView *v);
/* After a load: re-arm from V without recording anything. */
void ex_rearm(const ExView *v);
/* The gallery reports a picture: written (PNG + record) unless its source
 * is already in the gallery.  rgb = W*H*3.  1 = added, 0 = known, -1 =
 * error. */
int ex_picture_add(const ExPicture *p, const unsigned char *rgb);
/* A known source? */
int ex_picture_known(const char *source);
/* Unlock by key (1 = newly unlocked). */
int ex_unlock(const char *key, long long when);
/* The host clock (tests may pin it with TOKI_EXTRAS_CLOCK=UNIX). */
long long ex_now(void);

#ifdef __cplusplus
}
#endif
#endif
