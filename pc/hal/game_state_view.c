/* SPDX-License-Identifier: GPL-3.0-or-later */
/* game_state_view.c -- read-only decode of the game's state (game_state_view.h).
 *
 * Sources, all named PS1 globals (pc/ld/psx_globals.syms, linked at their
 * console addresses) -- the evidence for each is in docs/pc-port/
 * ENHANCED-MODE.md "Status at a glance" and docs/game-flows/ENDINGS-GUIDE.md:
 *
 *   calendar   now_year/now_month/now_day/now_youbi, season_no, pass_days,
 *              cal_day_kind (this month and the next: {n, kind[31], m,
 *              event[31]}, schedule_make's Cal)
 *   player     para_tai..para_str (s16 integer halves), basyo_pack (club in
 *              bits 12..15), byouki_kind, p_birthday, sched_icon (activity)
 *   girls      girl_tbl: 56-byte records, tokimeki +2, yuukou +6, bomb +10,
 *              flags +12, birthday +16, date-spot counters +21..+55;
 *              last_date_spot / last_date_spot_timer (date_exit)
 *   ending     get_h_yuukou_str0, get_h_tokimeki_tbl0..9, h_tokimeki_mod_tbl (MOD),
 *              get_grp_tbl0, get_grp_point_tbl0, grp_table_calc_tbl0..4,
 *              denwa_cnt, ev_flag4d, ev_flag4f, girl11_flag
 *   plan       schedule_buf (4-byte entries), schedule_cursor
 *   dates      reserved_date (8 x u32)
 *   names      girl_full_name_tbl (full names, Yoshio's evaluation screen),
 *              machiawase_menu_set_tbl0 (meeting places)
 *
 * The derived values are transcriptions of get_h_yuukou (0x800530A8),
 * get_h_tokimeki (0x80052A68), get_grp (0x800537F0), get_grp_point
 * (0x80053280), grp_table_calc (0x80053364), ending_select_init (ENDING
 * 0x801330F4) and tel_girl_status_init (TEL 0x8013CBF0).  Nothing here
 * writes game memory or calls game code; pc/tests/game_state_view_harness.c
 * proves the transcriptions equal the matched functions.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "types.h"
#include "game_state_view.h"

/* ---- the globals ---- */
extern u8 now_step, prev_step;
extern u8 season_no, now_year, now_month, now_day, now_youbi;
extern u16 pass_days;
extern u8 cal_day_kind[128];
extern s16 para_tai, para_bun, para_ri, para_gei, para_un, para_zat, para_you,
    para_kon, para_str;
extern u16 basyo_pack;
extern u8 byouki_kind;
extern u16 p_birthday;
extern u8 sched_icon;
extern u8 girl_tbl[];
extern u8 last_date_spot[16];
extern u8 last_date_spot_timer[16];
extern u8 get_h_yuukou_str0[];
extern u8 get_h_tokimeki_tbl0[], get_h_tokimeki_tbl1[], get_h_tokimeki_tbl2[];
extern s8 h_tokimeki_mod_tbl[];
extern u8 get_h_tokimeki_tbl3, get_h_tokimeki_tbl4, get_h_tokimeki_tbl5,
    get_h_tokimeki_tbl6, get_h_tokimeki_tbl7, get_h_tokimeki_tbl8, get_h_tokimeki_tbl9;
extern u8 get_grp_tbl0[];
extern u8 get_grp_point_tbl0[];
extern s8 grp_table_calc_tbl0[];
extern u8 grp_table_calc_tbl1[], grp_table_calc_tbl2[];
extern s8 grp_table_calc_tbl3[], grp_table_calc_tbl4[];
extern u8 denwa_cnt;
extern u8 ev_flag4d, ev_flag4f;
extern u32 girl11_flag;
extern u8 schedule_buf[];
extern u8 schedule_cursor;
extern u32 reserved_date[8];
extern u32 girl_full_name_tbl[];
extern u32 machiawase_menu_set_tbl0[];

#define REC(g) (girl_tbl + 56 * (g))
#define S16(p) (*(const s16 *)(const void *)(p))
#define U16(p) (*(const u16 *)(const void *)(p))
#define U32(p) (*(const u32 *)(const void *)(p))

static int stat_value(int k) {
    switch (k) {
    case 0: return para_tai;
    case 1: return para_bun;
    case 2: return para_ri;
    case 3: return para_gei;
    case 4: return para_un;
    case 5: return para_zat;
    case 6: return para_you;
    case 7: return para_kon;
    default: return para_str;
    }
}

/* ---- the game's computations ---- */

/* get_h_yuukou: (s32)((u32)yuukou * (u32)str0[g]) / 100, clamped 0..100 */
int gsv_yuukou_pct(int g) {
    s32 r;
    g %= 13;
    r = (s32)((u32)(s32)S16(REC(g) + 6) * (u32)get_h_yuukou_str0[g]) / 100;
    if (r >= 101) return 100;
    if (r < 0) return 0;
    return (u8)r;
}

/* get_h_tokimeki: base tokimeki * tbl0 / 100, plus one ladder value per
 * parameter (bits 1..6 of tbl1: bun ri gei un zat you) with the limits of
 * the girl's mode (tbl2); clamped 0..100 */
int gsv_tokimeki_pct(int g) {
    static const signed char add[7][7] = {
        { 0, 0, 0, 0, 0, 0, 0 },
        { 50, 35, 20, 20, 5, 5, 0 },    /* mode 1 */
        { 0, 0, 0, 0, 0, 0, 0 },        /* mode 2 */
        { 50, 50, 50, 20, 20, 20, 20 }, /* mode 3 */
        { 10, 10, 10, 10, 0, 0, 0 },    /* mode 4 */
        { 20, 20, 20, 20, 0, -5, 0 },   /* mode 5 */
        { 0, 0, 0, 0, -5, -5, -5 },     /* mode 6 */
    };
    u8 thr[7];
    s32 v;
    u8 mask, mode;
    int b, k;
    g %= 13;
    thr[0] = get_h_tokimeki_tbl3; thr[1] = get_h_tokimeki_tbl4;
    thr[2] = get_h_tokimeki_tbl5; thr[3] = get_h_tokimeki_tbl6;
    thr[4] = get_h_tokimeki_tbl7; thr[5] = get_h_tokimeki_tbl8;
    thr[6] = get_h_tokimeki_tbl9;
    v = (S16(REC(g) + 2) * get_h_tokimeki_tbl0[g]) / 100;
    mask = get_h_tokimeki_tbl1[g];
    mode = get_h_tokimeki_tbl2[g];
    for (b = 0; b < 6; b++) {
        u32 stat;
        if (!(mask & (1 << (b + 1)))) continue;
        if (mode < 1 || mode > 6) continue;     /* the switch has no default */
        stat = (u32)(s32)stat_value(b + 1);
        for (k = 0; k < 7; k++)
            if ((u32)(thr[k] + add[mode][k]) < stat) break;
        v += h_tokimeki_mod_tbl[g * 8 + k];
    }
    if (v >= 101) return 100;
    if (v < 0) return 0;
    return (u8)v;
}

/* get_grp: icons from the two percentages; get_grp_point: Shiori's 0 or
 * 785; grp_table_calc: girls 1..10 (s16 arithmetic, as buf[] is s16) */
void gsv_grp(unsigned char out[GSV_GIRLS], short score[GSV_GIRLS]) {
    static const int order[7] = { 7, 6, 5, 4, 3, 2, 1 };    /* i -> para (shift 7 - i) */
    s16 buf[GSV_GIRLS];
    int i, j, k;
    for (i = 0; i < GSV_GIRLS; i++) {
        u32 a = (u8)(s8)gsv_yuukou_pct(i), b = (u8)(s8)gsv_tokimeki_pct(i);
        a = (u8)(a / 5);
        if (a >= 20u) a = 19;
        b = (u8)(b / 5);
        if (b >= 20u) b = 19;
        out[i] = get_grp_tbl0[a + b * 20];
        score[i] = 0;
    }
    /* get_grp_point */
    if (out[0] >= 85 && (byouki_kind & 0xF) == 0) {
        int ok = 1, sum = 0;
        for (k = 0; k < 7; k++)                 /* bun ri gei un zat you kon */
            if ((u32)(s32)stat_value(k + 1) < (u32)get_grp_point_tbl0[k]) ok = 0;
        for (j = 1; j < 36; j++) sum += REC(0)[20 + j];
        if (ok && sum >= 8 && S16(REC(0) + 10) < 51) score[0] = GSV_SHIORI_SCORE;
    }
    /* grp_table_calc */
    for (k = 1; k < GSV_GIRLS; k++) {
        buf[k] = (s16)(out[k] << 2);
        buf[k] = (s16)(buf[k] + S16(REC(k) + 10) * grp_table_calc_tbl0[k]);
    }
    i = para_str;
    if (i > 140) i = 140;
    buf[2] = (s16)(buf[2] + grp_table_calc_tbl4[i / 10]);
    for (j = 1; j < GSV_GIRLS; j++) {
        u32 ba = grp_table_calc_tbl1[j], bb = grp_table_calc_tbl2[j];
        for (i = 0; i < 7; i++) {
            int shift = 7 - i, v = stat_value(order[i]) / 10;
            if (v > 14) v = 14;
            if ((ba >> shift) & 1) buf[j] = (s16)(buf[j] + grp_table_calc_tbl3[v]);
            if ((bb >> shift) & 1) buf[j] = (s16)(buf[j] + grp_table_calc_tbl3[v]);
        }
    }
    for (i = 1; i < GSV_GIRLS; i++) {
        int sum = 0;
        for (j = 1; j < 36; j++) sum += REC(i)[20 + j];
        if (sum >= 4) buf[i] = (s16)(buf[i] + sum * 10);
        else buf[i] = 0;
        score[i] = buf[i];
    }
}

/* tel_girl_status_init's face level */
static int face_level(int score, int icon) {
    if (score >= 500) return 0;
    if (icon < 6) return 6;
    if (icon < 11) return 5;
    if (icon < 21) return 4;
    if (icon < 41) return 3;
    if (icon < 61) return 2;
    return 1;
}

/* ---- the calendar ---- */

static int days_in(int year, int month) {
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month < 1 || month > 12) return 0;
    if (month == 2 && year % 4 == 0) return 29;     /* 1996: 19xx years */
    return d[month - 1];
}

static void add_event(GsvState *s, int kind, int y, int m, int d, int ahead, int code,
                      int girl, const char *label) {
    GsvEvent *e;
    int i;
    if (s->nevents >= GSV_MAX_EVENTS) return;
    /* in date order */
    for (i = s->nevents; i > 0 && s->events[i - 1].days_ahead > ahead; i--)
        s->events[i] = s->events[i - 1];
    e = &s->events[i];
    e->kind = kind;
    e->year = y;
    e->month = m;
    e->day = d;
    e->days_ahead = ahead;
    e->code = code;
    e->girl = girl;
    e->label = label;
    s->nevents++;
}

/* Calendar events of cal_day_kind from tomorrow on (both blocks), the
 * birthdays schedule_make would schedule (Shiori, met girls with flag bit
 * 6, the player), the booked dates; within `horizon` days. */
static void calendar(GsvState *s, int horizon) {
    int blk, y = s->year, m = s->month, ahead_base = 0, d, i;
    if (days_in(1900 + y, m) != cal_day_kind[0]) return;  /* not this month's table */
    for (blk = 0; blk < 2; blk++) {
        const u8 *c = cal_day_kind + 64 * blk;
        int n = c[0];   /* c[32] is not a day count (95/4: 5) */
        if (n < 28 || n > 31) break;
        for (d = 1; d <= n; d++) {
            int ahead = ahead_base + d - s->day, ev = c[32 + d], kind = c[d];
            if (ahead <= 0 || ahead > horizon) continue;
            if (ev && gsv_cal_event_name(ev))
                add_event(s, GSV_EV_CAL, y, m, d, ahead, ev, -1, gsv_cal_event_name(ev));
            else if (kind == 2)
                add_event(s, GSV_EV_CAL, y, m, d, ahead, 0, -1, "Holiday");
        }
        ahead_base += n;
        if (++m > 12) { m = 1; y++; }
    }
    /* birthdays: birth_day_check_days' fields (bits 0..3 month, 4..8 day) */
    for (i = -1; i < GSV_GIRLS; i++) {
        int bm, bd, ahead;
        if (i < 0) {
            bm = s->birth_month;
            bd = s->birth_day;
        } else {
            const GsvGirl *g = &s->girl[i];
            if (i > 0 && !(g->met && g->birthday_known)) continue;
            bm = g->birth_month;
            bd = g->birth_day;
        }
        if (bm < 1 || bm > 12 || bd < 1 || bd > 31) continue;
        /* days from today to the next bm/bd */
        {
            int yy = 1900 + s->year, mm = s->month, dd = s->day;
            for (ahead = 0; ahead <= horizon; ahead++) {
                if (ahead > 0 && mm == bm && dd == bd) break;
                if (++dd > days_in(yy, mm)) { dd = 1; if (++mm > 12) { mm = 1; yy++; } }
            }
            if (ahead > horizon) continue;
            if (i < 0)
                add_event(s, GSV_EV_PLAYER_BIRTHDAY, yy - 1900, bm, bd, ahead, 0, -1, "Your birthday");
            else
                add_event(s, GSV_EV_BIRTHDAY, yy - 1900, bm, bd, ahead, 0, i, "Birthday");
        }
    }
    /* booked dates (reserved_date: bits 0..4 place, 5..11 year, 12..15
     * month, 16..20 day, 24 machiawase, 25..28 girl, 29 active) */
    for (i = 0; i < 8; i++) {
        u32 w = reserved_date[i];
        GsvDate *b;
        int yy, mm, dd, ahead;
        if (!(w >> 29 & 1)) continue;
        if (s->nbooked >= GSV_MAX_DATES) break;
        b = &s->booked[s->nbooked++];
        b->place = (int)(w & 0x1F);
        b->year = (int)(w >> 5 & 0x7F);
        b->month = (int)(w >> 12 & 0xF);
        b->day = (int)(w >> 16 & 0x1F);
        b->machiawase = (int)(w >> 24 & 1);
        b->girl = (int)(w >> 25 & 0xF);
        yy = 1900 + s->year;
        mm = s->month;
        dd = s->day;
        for (ahead = 0; ahead <= 366; ahead++) {
            if (yy == 1900 + b->year && mm == b->month && dd == b->day) break;
            if (++dd > days_in(yy, mm)) { dd = 1; if (++mm > 12) { mm = 1; yy++; } }
        }
        if (ahead <= 366)
            add_event(s, GSV_EV_DATE, b->year, b->month, b->day, ahead, b->place, b->girl, "Date");
    }
}

/* schedule_buf from schedule_cursor: day blocks end with 251 (no 251
 * after the week's last day), the week with 255 (schedule_make).  The
 * cursor's block is today (measured: Wed 95/4/5 shows Wed..Sun, the
 * holiday on Sun 95/4/9 is today's), so day = days from today.  A buffer
 * with no 255 from the cursor on is not a plan (the zeroed buffer before
 * the first week is planned).  Types without a schedule_day_main arm
 * (250) are left out. */
static void plan(GsvState *s) {
    int i, day = 0;
    if (schedule_cursor >= 32) return;
    for (i = schedule_cursor; i < 32 && schedule_buf[4 * i] != 255; i++)
        ;
    if (i >= 32) return;
    for (i = schedule_cursor; i < 32; i++) {
        const u8 *e = &schedule_buf[4 * i];
        GsvPlan *p;
        if (e[0] == 255) break;
        if (e[0] == 251) { day++; continue; }
        if (!strcmp(gsv_plan_name(e[0]), "?")) continue;
        if (s->nplan >= GSV_MAX_PLAN) break;
        p = &s->plan[s->nplan++];
        p->day = day;
        p->type = e[0];
        p->a = e[1];
        p->b = e[2];
        p->label = gsv_plan_name(e[0]);
    }
}

/* qs_game_in_progress's rule (pc/hal/quicksave.c) */
static int in_game(void) {
    unsigned step = now_step == 2 ? prev_step : now_step;
    return step > 20 && now_year >= 95 && now_year <= 98 && now_month >= 1 && now_month <= 12 &&
           now_day >= 1 && now_day <= 31 && now_youbi < 7;
}

int gsv_read(GsvState *s) {
    unsigned char out[GSV_GIRLS];
    short score[GSV_GIRLS];
    int i, k, best;
    memset(s, 0, sizeof *s);
    s->leader = -1;
    s->most_bomb = -1;
    s->ending = 13;
    s->in_game = in_game();
    if (!s->in_game) return 0;
    s->year = now_year;
    s->month = now_month;
    s->day = now_day;
    s->youbi = now_youbi;
    s->season = season_no;
    s->pass_days = pass_days;
    s->week = pass_days / 7 + 1;
    for (k = 0; k < GSV_STATS; k++) s->stats[k] = stat_value(k);
    s->club = basyo_pack >> 12;
    s->ill = byouki_kind & 0xF;
    s->activity = sched_icon & 0xF;
    s->birth_month = p_birthday & 0xF;
    s->birth_day = (p_birthday >> 4) & 0x1F;

    gsv_grp(out, score);
    for (i = 0; i < GSV_GIRLS; i++) {
        GsvGirl *g = &s->girl[i];
        const u8 *r = REC(i);
        int j;
        g->index = i;
        g->tokimeki = S16(r + 2);
        g->yuukou = S16(r + 6);
        g->bomb = S16(r + 10);
        g->flags = U32(r + 12);
        g->met = g->flags >> 1 & 1;
        g->birth_month = U16(r + 16) & 0xF;
        g->birth_day = (U16(r + 16) >> 4) & 0x1F;
        g->birthday_known = g->flags >> 6 & 1;
        for (j = 1; j < 36; j++) g->dates += r[20 + j];
        g->last_spot = last_date_spot[i];
        g->spot_timer = last_date_spot_timer[i];
        g->yuukou_pct = gsv_yuukou_pct(i);
        g->tokimeki_pct = gsv_tokimeki_pct(i);
        g->icon = out[i];
        g->score = score[i];
        g->face = face_level(score[i], out[i]);
        g->latch_rumour = g->flags >> 17 & 1;
        g->latch_scene = g->flags >> 16 & 1;
        g->neglect = g->yuukou_pct < 10;
        g->lit = g->bomb >= GSV_BOMB_ICON;
        g->ladder_mode = get_h_tokimeki_tbl2[i];
        g->bomb_weight = i ? grp_table_calc_tbl0[i] : 0;
        for (k = 1; k <= 7; k++) {
            if (i) g->liked[k] = (grp_table_calc_tbl1[i] >> k & 1) + (grp_table_calc_tbl2[i] >> k & 1);
            if (k <= 6) g->ladder[k] = get_h_tokimeki_tbl1[i] >> k & 1;
        }
        if (g->met) {
            s->nmet++;
            if (s->most_bomb < 0 || g->bomb > s->girl[s->most_bomb].bomb) s->most_bomb = i;
        }
    }
    if (s->most_bomb >= 0 && s->girl[s->most_bomb].bomb <= 0) s->most_bomb = -1;

    /* ending_select_init's order */
    s->rei_calls = denwa_cnt;
    s->rei_answer = ev_flag4f;
    s->miharu_count = (int)(girl11_flag >> 9 & 7);
    s->miharu_left = ev_flag4d;
    best = 499;
    for (i = 0; i < GSV_GIRLS; i++)
        if (s->girl[i].met && best < s->girl[i].score) {
            s->leader = i;
            best = s->girl[i].score;
        }
    if (s->leader >= 0) s->leader_score = best;
    s->rei_wins = score[0] < 500 && denwa_cnt >= 70 && ev_flag4f == 2;
    if (s->rei_wins) s->ending = 12;
    else if (s->leader >= 0) s->ending = s->leader;
    else if (s->miharu_count && !s->miharu_left) s->ending = 11;   /* a dice roll */
    else s->ending = 13;

    calendar(s, 60);
    plan(s);
    return 1;
}

/* ---- names (PC-side text; the game's own tables where it has them) ---- */

static const char *const k_names[14] = {
    "Shiori Fujisaki", "Mio Kisaragi", "Yuina Himoo", "Ayako Katagiri",
    "Saki Nijino", "Yukari Koshiki", "Nozomi Kiyokawa", "Mira Kagami",
    "Yuko Asahina", "Megumi Mikihara", "Yumi Saotome", "Miharu Tatebayashi",
    "Rei Ijuin", "Yoshio Saotome",
};
static const char *const k_short[14] = {
    "Shiori", "Mio", "Yuina", "Ayako", "Saki", "Yukari", "Nozomi", "Mira",
    "Yuko", "Megumi", "Yumi", "Miharu", "Rei", "Yoshio",
};

const char *gsv_girl_name(int g) { return g >= 0 && g < 14 ? k_names[g] : "?"; }
const char *gsv_girl_short(int g) { return g >= 0 && g < 14 ? k_short[g] : "?"; }
const char *gsv_ending_name(int g) {
    if (g == 13) return "Yoshio (no girl)";
    return gsv_girl_name(g);
}

static const char *ps1_string(u32 addr) {
    if (addr < 0x80000000u || addr >= 0x80200000u) return NULL;
    return (const char *)(unsigned long)addr;
}
const char *gsv_girl_sjis(int g) { return g >= 0 && g < 16 ? ps1_string(girl_full_name_tbl[g]) : NULL; }
const char *gsv_place_sjis(int p) {
    return p >= 0 && p < 21 ? ps1_string(machiawase_menu_set_tbl0[p]) : NULL;
}

const char *gsv_place_name(int p) {
    static const char *const n[21] = {
        "Neighbourhood park", "Central Park", "Shopping street", "Aquarium", "Zoo",
        "Botanical garden", "Planetarium", "Art museum", "Library", "Arcade",
        "Bowling alley", "Karaoke", "Amusement park", "Stadium", "Cinema",
        "Concert hall", "Pool", "Beach", "Shrine", "Skating rink", "Ski slope",
    };
    return p >= 0 && p < 21 ? n[p] : "?";
}

const char *gsv_spot_name(int s) {
    static const char *const n[37] = {
        "-", "Central Park", "Boutique", "Cute-goods shop", "Junk shop", "Aquarium",
        "Aquarium", "Zoo", "Zoo", "Botanical garden", "Planetarium", "Art museum",
        "Art exhibition", "Art exhibition", "Art exhibition", "Library", "Arcade",
        "Medal corner", "Bowling", "Karaoke", "Hero show", "Roller coaster",
        "Ghost house", "Ferris wheel", "Scream machine", "Virtual Ship", "-",
        "Stadium", "Stadium", "Pool", "Beach", "Skating rink", "Ski slope",
        "Neighbourhood park", "Summer festival", "Cinema", "Concert hall",
    };
    return s >= 0 && s < 37 ? n[s] : "?";
}

const char *gsv_stat_name(int k) {
    static const char *const n[GSV_STATS] = {
        "Stamina", "Humanities", "Science", "Art", "Sports", "Trivia", "Looks",
        "Guts", "Stress",
    };
    return k >= 0 && k < GSV_STATS ? n[k] : "?";
}
const char *gsv_stat_short(int k) {
    static const char *const n[GSV_STATS] = {
        "STA", "HUM", "SCI", "ART", "SPT", "TRV", "LKS", "GUT", "STR",
    };
    return k >= 0 && k < GSV_STATS ? n[k] : "?";
}

/* docs/pc-port/COVERAGE-INVENTORY.md 1.4 (the calendar trailers); 1, 7 and
 * 9 have no schedule_make arm and are not shown */
const char *gsv_cal_event_name(int ev) {
    switch (ev) {
    case 2: return "New school year";
    case 4: return "Sports festival";
    case 5: return "Exam week";
    case 6: return "Summer vacation";
    case 8: return "Club summer week";
    case 10: return "Second term";
    case 11: return "School trip week";
    case 12: return "Culture festival";
    case 13: return "Christmas party";
    case 14: return "Winter vacation";
    case 15: return "New Year";
    case 16: return "Third term";
    case 17: return "Valentine's Day";
    case 18: return "Entrance exams";
    case 19: return "White Day";
    case 20: return "Spring vacation";
    default: return NULL;
    }
}

/* schedule_day_main's switch (src/main/matched/schedule_day_main.c) */
const char *gsv_plan_name(int t) {
    switch (t) {
    case 0: return "School";
    case 16: return "Holiday";
    case 32: return "Sports festival";
    case 36: return "Culture festival";
    case 37: return "Club tournament";
    case 38: return "Club final";
    case 39: return "Club contest";
    case 40: return "Christmas party";
    case 44: return "New Year";
    case 48: return "White Day";
    case 49: return "Valentine's Day";
    case 50: return "Birthday";
    case 51: return "Your birthday";
    case 64: return "Exams";
    case 65: return "School trip";
    case 66: return "Club camp";
    case 67: return "Career talk";
    case 68: return "After-school chance";
    case 69: return "Event";
    case 70: return "Date-invite chance";
    case 71: return "Visit";
    case 72: return "First meeting";
    case 73: return "Summer outing";
    case 74: return "Entrance exams";
    case 252: return "Health check";
    case 253: return "Season change";
    case 254: return "Scene 56";
    default: return "?";
    }
}

/* the weekly activity (ENDINGS-GUIDE.md 6.6, parameter_up_down) */
const char *gsv_activity_name(int a) {
    static const char *const n[8] = {
        "Humanities", "Science", "Art", "Sports", "Club", "Trivia", "Looks", "Rest",
    };
    return a >= 0 && a < 8 ? n[a] : "?";
}

const char *gsv_club_name(int c) {
    static const char *const n[12] = {
        "Literature", "Drama", "Science", "Computer", "Art", "Brass band", "Baseball",
        "Soccer", "Tennis", "Swimming", "Basketball", "None",
    };
    return c >= 0 && c < 12 ? n[c] : "None";
}

const char *gsv_youbi_name(int y) {
    static const char *const n[7] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
    return y >= 0 && y < 7 ? n[y] : "?";
}

/* ---- a text form (the status log and the tests) ---- */

static int put(char *b, int cap, int n, const char *fmt, ...) {
    va_list ap;
    int r;
    if (n >= cap) return n;
    va_start(ap, fmt);
    r = vsnprintf(b + n, (size_t)(cap - n), fmt, ap);
    va_end(ap);
    return r < 0 ? n : n + r;
}

/* key = value lines, one girl per line; stable for diffing */
int gsv_format(const GsvState *s, char *b, int cap) {
    int n = 0, i, k;
    if (cap > 0) b[0] = 0;
    n = put(b, cap, n, "in_game = %d\n", s->in_game);
    if (!s->in_game) return n;
    n = put(b, cap, n, "date = %d/%d/%d youbi=%d season=%d pass_days=%d week=%d\n", s->year,
            s->month, s->day, s->youbi, s->season, s->pass_days, s->week);
    n = put(b, cap, n, "stats =");
    for (k = 0; k < GSV_STATS; k++) n = put(b, cap, n, " %d", s->stats[k]);
    n = put(b, cap, n, "\nplayer = club=%d ill=%d activity=%d birthday=%d/%d\n", s->club, s->ill,
            s->activity, s->birth_month, s->birth_day);
    for (i = 0; i < GSV_GIRLS; i++) {
        const GsvGirl *g = &s->girl[i];
        n = put(b, cap, n,
                "girl%d = met=%d tok=%d yuu=%d bomb=%d flags=%08x birth=%d/%d known=%d dates=%d "
                "last_spot=%d timer=%d yuu_pct=%d tok_pct=%d icon=%d score=%d face=%d latch61=%d "
                "latch76=%d neglect=%d lit=%d\n",
                i, g->met, g->tokimeki, g->yuukou, g->bomb, g->flags, g->birth_month, g->birth_day,
                g->birthday_known, g->dates, g->last_spot, g->spot_timer, g->yuukou_pct,
                g->tokimeki_pct, g->icon, g->score, g->face, g->latch_rumour, g->latch_scene,
                g->neglect, g->lit);
    }
    n = put(b, cap, n, "ending = %d leader=%d score=%d rei_calls=%d rei_answer=%d miharu=%d/%d "
            "most_bomb=%d nmet=%d\n", s->ending, s->leader, s->leader_score, s->rei_calls,
            s->rei_answer, s->miharu_count, s->miharu_left, s->most_bomb, s->nmet);
    for (i = 0; i < s->nevents; i++) {
        const GsvEvent *e = &s->events[i];
        n = put(b, cap, n, "event = +%d %d/%d/%d kind=%d code=%d girl=%d %s\n", e->days_ahead,
                e->year, e->month, e->day, e->kind, e->code, e->girl, e->label);
    }
    for (i = 0; i < s->nplan; i++) {
        const GsvPlan *p = &s->plan[i];
        n = put(b, cap, n, "plan = day%d type=%d a=%d b=%d %s\n", p->day, p->type, p->a, p->b,
                p->label);
    }
    for (i = 0; i < s->nbooked; i++) {
        const GsvDate *d = &s->booked[i];
        n = put(b, cap, n, "booked = %d/%d/%d place=%d girl=%d machiawase=%d\n", d->year,
                d->month, d->day, d->place, d->girl, d->machiawase);
    }
    return n;
}
