/* SPDX-License-Identifier: GPL-3.0-or-later */
/* game_state_view.h -- a READ-ONLY decode of the game's state from PS1 RAM,
 * for the PC-side status overlay (pc/hal/status_view.c) and its tests.
 *
 * Every value is read from a named PS1 global (pc/ld/psx_globals.syms) or
 * computed from them with the game's own rules, transcribed from the
 * matched C and checked against those functions themselves
 * (pc/tests/game_state_view_harness.c links get_grp & co. and compares).
 * Nothing here writes game memory or calls game code.
 *
 * SDL-free and allocation-free: gsv_read() fills a caller's struct.
 */
#ifndef GAME_STATE_VIEW_H
#define GAME_STATE_VIEW_H

#ifdef __cplusplus
extern "C" {
#endif

#define GSV_GIRLS 11            /* the scored girls 0..10 (girl_tbl records) */
#define GSV_STATS 9             /* para_tai .. para_str */
#define GSV_MAX_EVENTS 24
#define GSV_MAX_PLAN 32
#define GSV_MAX_DATES 8

/* game-side constants (named here, evidence in the doc) */
#define GSV_BOMB_ICON 60        /* tel_girl_status_init: bomb sprite at >= 60 */
#define GSV_BOMB_RUMOUR 61      /* schedule_day_main: rumour screen (step 68) */
#define GSV_BOMB_SCENE 76       /* schedule_day_main: GEKO scene 89 */
#define GSV_BOMB_CALL 91        /* schedule_day_main: Yoshio's call, the explosion */
#define GSV_BOMB_REARM 50       /* girls_param_clamp: latches cleared below 50 */
#define GSV_SCORE_MIN 500       /* ending_select_init: best starts at 499 */
#define GSV_SHIORI_SCORE 785    /* get_grp_point */

typedef struct {
    int index;                  /* 0..10 = girl_tbl record */
    int met;                    /* flag word bit 1 */
    int tokimeki, yuukou, bomb; /* s16 integer halves at +2, +6, +10 */
    unsigned flags;             /* flag word at +12 */
    int birth_month, birth_day; /* +16: bits 0..3, 4..8 */
    int birthday_known;         /* flag bit 6 (schedule_make's birthday entry) */
    int dates;                  /* sum of flags[1..35] (+21..+55): ordinary dates */
    int last_spot;              /* last_date_spot[girl] (date_exit), 0 = none */
    int spot_timer;             /* last_date_spot_timer[girl], 12 after a date */
    /* the game's derived values (get_h_yuukou / get_h_tokimeki / get_grp) */
    int yuukou_pct, tokimeki_pct;
    int icon;                   /* get_grp out[i]: 0..95 */
    int score;                  /* get_grp score[i] (ending score; 0/785 for Shiori) */
    int face;                   /* Yoshio's screen face level 0..6 (0 = qualifies) */
    int latch_rumour;           /* flag bit 17: the 61 rumour was shown */
    int latch_scene;            /* flag bit 16: the 76 scene was shown */
    int neglect;                /* yuukou% < 10: +5 bomb per term planning night */
    int lit;                    /* bomb >= GSV_BOMB_ICON */
    int liked[GSV_STATS];       /* times stat k counts in grp_table_calc (T1 + T2) */
    int ladder[GSV_STATS];      /* stat k is in her tokimeki ladder (tbl1 mask) */
    int ladder_mode;            /* get_h_tokimeki_tbl2 */
    int bomb_weight;            /* grp_table_calc_tbl0[k] (0 for Shiori) */
} GsvGirl;

enum { GSV_EV_CAL, GSV_EV_BIRTHDAY, GSV_EV_PLAYER_BIRTHDAY, GSV_EV_DATE };
typedef struct {
    int kind;                   /* GSV_EV_* */
    int year, month, day;
    int days_ahead;             /* 0 = today */
    int code;                   /* cal event number / girl / place */
    int girl;                   /* birthday / booked date: the girl, else -1 */
    const char *label;          /* romanised (PC-side text table) */
} GsvEvent;

typedef struct {
    int day;                    /* days from today (the cursor's block is today) */
    int type, a, b;             /* schedule_buf entry */
    const char *label;
} GsvPlan;

typedef struct {
    int place, year, month, day, girl, machiawase;
} GsvDate;

typedef struct {
    int in_game;                /* a game is running (quicksave's rule) */
    int year, month, day, youbi, season, pass_days, week;
    int stats[GSV_STATS];       /* para_* integer halves */
    int club;                   /* basyo_pack >> 12, 11 = none */
    int ill;                    /* byouki_kind & 0xF */
    int activity;               /* sched_icon & 0xF: this week's plan */
    int birth_month, birth_day; /* p_birthday */
    GsvGirl girl[GSV_GIRLS];
    int nmet;
    /* the ending as ending_select_init would pick it today */
    int rei_calls, rei_answer;  /* denwa_cnt, ev_flag4f */
    int rei_wins;               /* score[0] < 500 && calls >= 70 && answer == 2 */
    int miharu_count;           /* girl11_flag bits 9..11 */
    int miharu_left;            /* ev_flag4d */
    int leader;                 /* 0..10, or -1 (nobody >= 500) */
    int leader_score;
    int ending;                 /* now_girl the rule gives: 0..10, 11 maybe, 12, 13 */
    int most_bomb;              /* the met girl with the highest bomb, -1 none */
    GsvEvent events[GSV_MAX_EVENTS];
    int nevents;
    GsvPlan plan[GSV_MAX_PLAN];
    int nplan;
    GsvDate booked[GSV_MAX_DATES];
    int nbooked;
} GsvState;

/* Read the live game.  Returns s->in_game. */
int gsv_read(GsvState *s);

/* The game's own computations, transcribed (exposed for the tests). */
int gsv_yuukou_pct(int girl);
int gsv_tokimeki_pct(int girl);
/* out[0..10] / score[0..10] as get_grp(out, score) leaves them */
void gsv_grp(unsigned char out[GSV_GIRLS], short score[GSV_GIRLS]);

/* key = value text of S (the status log, the tests); returns the length */
int gsv_format(const GsvState *s, char *buf, int cap);

/* Names.  The romanised names are PC-side text (docs: "Status at a glance");
 * the Shift-JIS ones are the game's own tables in RAM. */
const char *gsv_girl_name(int girl);        /* "SHIORI FUJISAKI" */
const char *gsv_girl_short(int girl);       /* "SHIORI" */
const char *gsv_girl_sjis(int girl);        /* girl_full_name_tbl[girl] (Yoshio's screen) */
const char *gsv_place_sjis(int place);      /* machiawase_menu_set_tbl0[place] */
const char *gsv_place_name(int place);      /* romanised */
const char *gsv_spot_name(int spot);        /* date spot ids 1..36, romanised */
const char *gsv_stat_name(int k);           /* "STAMINA" .. "STRESS" */
const char *gsv_stat_short(int k);          /* "STA" .. "STR" */
const char *gsv_cal_event_name(int ev);     /* calendar event 1..20 */
const char *gsv_plan_name(int type);        /* schedule_buf entry type */
const char *gsv_activity_name(int a);       /* weekly activity 0..7 */
const char *gsv_club_name(int c);
const char *gsv_youbi_name(int y);          /* "MON" .. "SUN" */
const char *gsv_ending_name(int now_girl);  /* 0..13 */

#ifdef __cplusplus
}
#endif
#endif /* GAME_STATE_VIEW_H */
