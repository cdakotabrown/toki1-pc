/* SPDX-License-Identifier: GPL-3.0-or-later */
/* guide_model.h -- the game-mechanics guide: the rules, the numbers and the
 * text of the GUIDE pages of the settings menu (pc/hal/guide_view.c).
 *
 * Every number shown is either read at run time from the player's game (a
 * table of the executable in PS1 RAM, or of an overlay file in the
 * player's extracted disc) or a rule transcribed from the matched C
 * (constants of the code, written here as our own rule text).  Each rule
 * function below has a counterpart in the game, and
 * pc/tests/guide_harness.c runs both on the same RAM and compares them
 * (pc/tools/test_guide.py).  The written evidence -- function, address,
 * table -- is in docs/game-flows/MECHANICS-GUIDE.md.
 *
 * Read only: nothing here writes PS1 memory or calls game code.  SDL-free.
 */
#ifndef GUIDE_MODEL_H
#define GUIDE_MODEL_H

#include "game_state_view.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the rules (each checked against the game's function) ---- */

/* parameter_up_down (0x80074890): the 16.16 change of parameter STAT
 * (0 stamina .. 8 stress) for one session of activity ACT (0..7; 4 = the
 * club CLUB 0..11), on a holiday (x4), on a poor day, with illness ILL
 * (byouki_kind & 0xF).  The row is read from PS1 RAM. */
int guide_act_delta(int act, int club, int stat, int holiday, int poor, int ill);
/* the raw table entry (1/16 point a session) */
int guide_act_row(int act, int club, int stat);

/* biorhythm_sf_set (0x8006BE48) for the day OFFSET days after pass_days
 * with this week's activity (sched_icon) and club, from PS1 RAM: how many
 * of the 32768 values rand() can return make it a poor day. */
int guide_poor_count(int offset);
/* biorhythm_sf2 (0x8006C178): the three curves on day pass_days + OFFSET,
 * -4096..4096 (body 23 days, mood 28, mind 33) */
void guide_bio_curves(int offset, int out[3]);
/* biorhythm_sf2(u, v, w, 16) on day pass_days + OFFSET: 0..16 */
int guide_bio16(int offset, int u, int v, int w);

/* score_grade_calc (ETC 0x80146D40): the exam value of weekday YOUBI
 * (0 Monday: humanities .. 4 Friday: art) before bonuses, and the score
 * when every rand() returns R (LIB: dated at the library this month);
 * get_etc_hosei's bonus for the exam of YEAR / MONTH (-1: no ETC.EXN) */
int guide_exam_raw(int youbi);
int guide_exam_score(int youbi, int year, int month, int lib, int r);
int guide_exam_bonus(int year, int month);

/* touzyou_check (MODE 0, fewer than 2 of girls 1..9 met),
 * touzyou_check2 (1), touzyou_basyo_check (2): the girl whose first
 * meeting the check would schedule when its die passes (0 = none), and
 * *sub its variant; the die is 1 in 3 for modes 0 and 2, 1 in 8 for 1. */
int guide_debut_pick(int mode, int *sub);
/* the debut die: the chance in 1/24 (8 = 1 in 3, 3 = 1 in 8) */
int guide_debut_die24(int mode);

/* byouki_hassei (0x80072E30), healthy: of the 256 x 256 (rand() & 0xFF,
 * album_work & 0xFF) pairs, how many give state 1 (cold), 2 (stress) and
 * 3 (both) */
void guide_ill_onset(int out[4]);
/* the threshold kon / 8 + 204 (at most 254) */
int guide_ill_limit(void);
/* byouki_hassei: the next state (0 healthy, 1 cold, 2 nerves, 3 both)
 * from STATE with the day's draws A = rand() & 0xFF, B = album_work & 0xFF */
int guide_ill_next(int state, int a, int b);
/* syoushin_up (0x80070ED0): the bomb a term planning night adds */
int guide_neglect(int girl);
/* tokimeki % as the guide explains it (get_h_tokimeki's rule) */
int guide_tokimeki_pct(int g, const int stats[9], int base);

/* girl_counter_periodic_decrement (0x80070D04): friendship loss of GIRL
 * at a term planning night on day-of-month DAY */
int guide_decay(int girl, int day);
/* get_g_zyotai_h (0x8005DDD0) / get_g_zyotai_s (0x80051E88): her mood */
int guide_mood_h(int yuukou_pct, int tokimeki_pct);
int guide_mood_s(int yuukou_pct, int tokimeki_pct);

/* ---- phone, invitations, dates (guide_rules_dates.c) ---- */

/* an overlay's bytes at a console address, from the player's EXN file */
int guide_exn_read(const char *ov, unsigned addr, void *buf, int n);
/* tel_basyo_suki_tbl: her liking of place P when you phone (-1: no file) */
int guide_tel_like(int g, int p);
/* D_8015B780: the date grade of place P for her, 0 the best (-1: no file) */
int guide_date_cell(int g, int p);
/* tel_dialog20 step 6: yes when (rand() & 0xFF) < a; returns a */
int guide_accept_a(int g, int p, int icon);
/* date_rank_grade_get */
void guide_date_ab(int g, int p, int mode, int *A, int *B);
void guide_date_ab_cell(int cell, int mode, int *A, int *B);
int guide_date_delta_cell(int cell, int imp, int which);
/* date_score_apply: 16.16 change of tokimeki (0), friendship (1), bomb (2) */
int guide_date_delta(int g, int p, int imp, int which);
/* date_sasoi_judge (KIND 0) / geko_judge (KIND 1) */
int guide_invite_day_ok(void);
int guide_invite_ok(int kind, int g, int icon);
int guide_invite_pct(int kind);
int guide_invite_pick(int kind, const unsigned char icon[11]);
/* CALROM.NBN, month K (0 = April 1995 .. 35 = March 1998): days, first
 * weekday, each day's kind (0 school, 1 Sunday, 2 holiday, 3 vacation) and
 * event number; 0 when the file is not there */
int guide_cal_month(int k, int *n, int *first, unsigned char kind[31], unsigned char ev[31]);

/* ---- the pages ---- */

enum { GUIDE_ACTIVITIES, GUIDE_GIRLS, GUIDE_EVENTS, GUIDE_RULES, GUIDE_TABS };
const char *guide_tab_name(int tab);

#define GUIDE_TEXT 160
#define GUIDE_CELLS 10
#define GUIDE_MAX_LINES 700

enum {
    GL_HEAD,        /* a section title */
    GL_TEXT,        /* a wrapped paragraph line */
    GL_ROW,         /* a table row: text = first column, cells right-aligned at cx */
    GL_SRC,         /* where it comes from (dim) */
    GL_GAP          /* half a line of space */
};

typedef struct {
    unsigned char kind, ncell;
    short indent;
    unsigned col;                       /* 0xRRGGBBAA */
    char text[GUIDE_TEXT];
    short cx[GUIDE_CELLS];              /* right edge of each cell */
    unsigned ccol[GUIDE_CELLS];         /* 0 = the line's colour */
    char cell[GUIDE_CELLS][10];
} GuideLine;

typedef struct {
    int n, width;
    GuideLine l[GUIDE_MAX_LINES];
} GuideDoc;

/* Build a page: TAB, GIRL for the girls tab (-1 = the list), the live
 * state S (may be NULL or not in a game: no live notes), WIDTH units. */
void guide_build(GuideDoc *d, int tab, int girl, const GsvState *s, int width);

/* the girls the GIRLS list offers (0..12) and their names */
#define GUIDE_NGIRLS 13
const char *guide_girl_name(int g);
/* the list line's live note for girl G ("met", "you qualify now", ...) */
void guide_girl_note(int g, const GsvState *s, char *out, int cap);

/* plain text of a page (the tests, TOKI_GUIDE_LOG) */
int guide_doc_text(const GuideDoc *d, char *buf, int cap);

/* the colours (pc_ui.h's palette) */
#define GUIDE_C_TEXT 0xF8F8F8FFu
#define GUIDE_C_DIM 0xB4CCA8FFu
#define GUIDE_C_HEAD 0xF8D860FFu
#define GUIDE_C_GOOD 0x88E0FFFFu
#define GUIDE_C_WARN 0xFFB060FFu
#define GUIDE_C_BAD 0xFF7878FFu

#ifdef __cplusplus
}
#endif
/* 1: emit the "Source:" provenance lines (off for players) */
extern int guide_show_sources;

#endif /* GUIDE_MODEL_H */
