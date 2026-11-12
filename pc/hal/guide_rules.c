/* SPDX-License-Identifier: GPL-3.0-or-later */
/* guide_rules.c -- the game's rules behind the GUIDE pages (guide_model.h),
 * transcribed from the matched C and read from the player's game.
 *
 * Each function names the game function it reproduces.
 * pc/tests/guide_harness.c links those functions (src/main/matched) at the
 * console addresses and compares them with these on the same RAM, so a
 * number the guide prints is the game's.  Tables are never copied here:
 * they are read at run time from PS1 RAM (the executable's data, which the
 * port loads from the player's PSX.EXE) through the named globals of
 * pc/ld/psx_globals.syms.  Evidence: docs/game-flows/MECHANICS-GUIDE.md.
 *
 * Read only: no PS1 write, no game call.
 */
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "guide_model.h"

/* ---- the globals read ---- */
extern s8 parameter_up_down_tbl0[];     /* 8 activities x 9 parameters */
extern s8 parameter_up_down_tbl1[];     /* 12 clubs x 9 */
extern u8 byouki_kind;
extern u8 byouki_state;
extern u16 basyo_pack;
extern u8 sched_icon;                   /* this week's activity, low nibble */
extern u16 pass_days;
extern u32 p_birthday;
extern u16 get_pass_days_tbl0[];
extern u16 get_pass_days_tbl1;
extern s16 rsin_tbl[];
extern s16 para_tai, para_bun, para_ri, para_gei, para_un, para_zat, para_you,
    para_kon, para_str;
extern u8 bukatu_rec[];                 /* 16 x {u16 sessions, u16} */
extern u32 album_work;
extern u8 girl_tbl[];
extern u8 now_day;

#define REC(g) (girl_tbl + 56 * (g))
#define S16(p) (*(const s16 *)(const void *)(p))
#define U16(p) (*(const u16 *)(const void *)(p))
#define U32(p) (*(const u32 *)(const void *)(p))

/* ---- parameter_up_down (0x80074890) ----
 * Positive entries are scaled by (holiday ? 4 : 1) << 12, divided by 2 on a
 * poor day, by 2 when ill in state 3, and by 2 in states 1 / 2 unless the
 * activity is trivia, looks or rest; negative entries are applied as they
 * are (entry << 12).  Stress (entry 8) gets one more point on a poor day. */
int guide_act_row(int act, int club, int stat) {
    const s8 *p;
    if (act < 0 || act > 7 || stat < 0 || stat > 8) return 0;
    if (act == 4) {
        if (club < 0 || club > 15) club = 11;
        p = &parameter_up_down_tbl1[club * 9];
    } else {
        p = &parameter_up_down_tbl0[act * 9];
    }
    return p[stat];
}

int guide_act_delta(int act, int club, int stat, int holiday, int poor, int ill) {
    int v = guide_act_row(act, club, stat), xdiv = 1, ydiv = 1, mul;
    ill &= 0xF;
    if (ill == 3) xdiv = 2;
    else if ((ill == 1 || ill == 2) && act != 5 && act != 7 && act != 6) ydiv = 2;
    mul = ((holiday ? 4 : 1) << 12) / (poor ? 2 : 1) / xdiv / ydiv;
    if (stat < 8) return v > 0 ? v * mul : v * 4096;
    /* stress */
    if (v < 0) return v * mul + (poor ? 2 : 1) * 65536 - 65536;
    return v * 4096 + (poor ? 2 : 1) * 65536 - 65536;
}

/* ---- the biorhythm: biorhythm_sf2 (0x8006C3D4), get_pass_days
 * (0x8006C7A0), rsin (0x800A404C) / sin_1 (0x800A4088) ---- */
static s32 sin1(s32 a) {
    if (a < 2049) return a < 1025 ? rsin_tbl[a] : rsin_tbl[2048 - a];
    if (a < 3073) return -rsin_tbl[a - 2048];
    return -rsin_tbl[4096 - a];
}

static s32 rsin_(s32 a) {
    if (a < 0) return -sin1((-a) & 0xFFF);
    return sin1(a & 0xFFF);
}

static s32 pass_days_of(u32 pd) {
    u32 packed = p_birthday, month = packed & 0xF, field = (packed << 23) >> 27;
    if (month == 4 && field < 4) return -(s32)get_pass_days_tbl1 - (s32)field + 5506;
    return (s32)pd - (s32)get_pass_days_tbl0[month] - (s32)field + 5475;
}

static void curves(u32 pd, s32 out[3]) {
    s32 base = pass_days_of(pd) + (s32)pd;    /* get_pass_days() + pass_days */
    out[0] = rsin_(base * 178);
    out[1] = rsin_(base * 146);
    out[2] = rsin_(base * 124);
}

void guide_bio_curves(int offset, int out[3]) {
    s32 c[3];
    curves((u16)(pass_days + offset), c);
    out[0] = c[0]; out[1] = c[1]; out[2] = c[2];
}

static s32 sf2(const s32 c[3], int u, int v, int w, int k) {
    s32 sum = 0;
    int n = 0, use[3], i;
    use[0] = u; use[1] = v; use[2] = w;
    for (i = 0; i < 3; i++) {
        if (!use[i]) continue;
        n++;
        if (abs(c[i]) < 409) sum -= 4096;
        else if (c[i] > 0) sum += c[i];
        else sum -= c[i] / 2;
    }
    if (n == 0) return 1;
    if (sum / n < 0) return 0;          /* k > 0 only here */
    return sum * k / 4096 / n;
}

int guide_poor_count(int offset) {
    s32 c[3], t0 = 0, t1, m;
    int probes = 0, club, count = 0, r;
    curves((u16)(pass_days + offset), c);
    switch (sched_icon & 0xF) {
    case 0: t0 = sf2(c, 0, 0, 1, 16) + para_bun / 16; break;
    case 1: t0 = sf2(c, 0, 0, 1, 16) + para_ri / 16; break;
    case 2: t0 = sf2(c, 0, 1, 0, 16) + para_gei / 16; break;
    case 3: t0 = sf2(c, 1, 0, 0, 16) + para_un / 16; break;
    case 4:
        club = basyo_pack >> 12;
        if (club == 1 || club == 4 || club == 5) t0 = sf2(c, 0, 1, 0, 16);
        else if (club == 0 || club == 2 || club == 3) t0 = sf2(c, 0, 0, 1, 16);
        else if (club >= 6 && club <= 10) t0 = sf2(c, 1, 0, 0, 16);
        t0 += U16(bukatu_rec + 4 * club) >> 4;
        break;
    case 5: t0 = sf2(c, 0, 1, 0, 16) + para_zat / 16; break;
    case 6: t0 = sf2(c, 0, 1, 0, 16) + para_you / 16; break;
    case 7: t0 = 16; break;
    default: break;
    }
    t0 += 5;
    t1 = para_str < 97 ? para_str / 32 : 4;
    if (sf2(c, 1, 0, 0, 3) == 0) probes++;
    if (sf2(c, 0, 1, 0, 3) == 0) probes++;
    if (sf2(c, 0, 0, 1, 3) == 0) probes++;
    m = t0 + 4;
    if (m <= 0) return 32768;           /* not reached: t0 >= 5 */
    for (r = 0; r < 32768; r++) {
        s32 x = ((8 - t1) * (r % m)) / 8;
        if (probes == 3) x /= 4;
        if (x < 5) count++;
    }
    return count;
}

/* ---- exams: score_grade_calc (ETC 0x80146D40), get_etc_hosei (ETC
 * 0x801463F8) ----
 * The day's subject (now_youbi 0..4): its main parameter plus a capped
 * second one plus the biorhythm (0..16) minus 8, halved with neurosis or
 * sickness (byouki_kind & 2), at least 1; plus the exam's bonus when
 * (rand() & 7) + bonus > 8; plus LIB x (rand() % 10) (LIB: a library date
 * this month); score = v * 100 / 128, or 100 - (rand() & 3) from 128 up.
 * R is the value every rand() returns (the harness's way to enumerate). */
extern u8 now_youbi;
int guide_exam_bonus(int year, int month) {
    static s32 tbl[9];
    static int ok = -1;
    int sel = year == 95 ? 0 : year == 96 ? 3 : 6;
    int guide_exn_read(const char *ov, unsigned addr, void *buf, int n);
    if (ok < 0) ok = guide_exn_read("ETC", 0x80150518u, tbl, sizeof tbl);
    if (!ok) return -1;
    if (month == 12) sel++;
    else if (month == 2) sel--;
    return tbl[sel];
}

int guide_exam_raw(int youbi) {
    s32 c[3], main_ = 0, sub = 0, bio = 0, v;
    curves(pass_days, c);
    switch (youbi) {
    case 0: main_ = para_bun; bio = sf2(c, 0, 0, 1, 16); break;
    case 1: main_ = para_ri; bio = sf2(c, 0, 0, 1, 16); break;
    case 2:
        main_ = para_ri * 7 / 10;
        sub = para_zat * 3 / 10;
        if (sub >= 39) sub = 38;
        bio = sf2(c, 0, 1, 1, 16);
        break;
    case 3:
        sub = para_bun * 3 / 10;
        main_ = para_zat * 7 / 10;
        bio = sf2(c, 0, 0, 1, 16);
        if (sub >= 39) sub = 38;
        break;
    case 4:
        main_ = para_gei * 8 / 10;
        sub = para_zat * 2 / 10;
        bio = sf2(c, 0, 1, 0, 16);
        if (sub >= 26) sub = 25;
        break;
    default:
        return 0;
    }
    v = main_ + sub + bio - 8;
    if (byouki_kind & 2) v /= 2;
    return v < 0 ? 1 : v;
}

int guide_exam_score(int youbi, int year, int month, int lib, int r) {
    int v = guide_exam_raw(youbi), bonus = guide_exam_bonus(year, month);
    if (bonus > 0 && (r & 7) + bonus - 8 > 0) v += bonus;
    v += lib * (r % 10);
    return v < 128 ? v * 100 / 128 : 100 - (r & 3);
}

/* the biorhythm term of a day (biorhythm_sf2 with k = 16) */
int guide_bio16(int offset, int u, int v, int w) {
    s32 c[3];
    curves((u16)(pass_days + offset), c);
    return sf2(c, u, v, w, 16);
}

/* ---- the first meetings: touzyou_check (0x8006EC08), touzyou_check2
 * (0x8006EF68), touzyou_basyo_check (0x8006F2BC) ----
 * One rule per girl, in the order the checks try them.  stat k: 1
 * humanities .. 7 guts (the s16 halves of the 16.16 slots). */
typedef struct {
    int girl;
    int act;            /* the week's activity, -1 any */
    int s1, t1lo, t1hi; /* stat and its threshold (check / check2) */
    int s2, t2lo, t2hi; /* a second stat, 0 none */
    int not_home;       /* 8 / 9: Shiori's home place must differ; -1: hers
                         * must differ from your club */
    int stage;          /* Yuko: debut stage (flag bits 9..11), -1 none */
} DebutRule;

static const DebutRule k_debut[] = {
    { 1, 0, 1, 55, 75, 0, 0, 0, 0, -1 },
    { 2, 1, 2, 60, 85, 0, 0, 0, 0, -1 },
    { 3, 2, 3, 50, 65, 0, 0, 0, 0, -1 },
    { 5, 3, 4, 81, 84, 6, 81, 84, 8, -1 },
    { 6, 3, 4, 31, 71, 7, 31, 71, 9, -1 },
    { 7, 6, 6, 101, 126, 0, 0, 0, 0, -1 },
    { 8, -1, 5, 36, 36, 0, 0, 0, 0, 0 },
    { 8, -1, 5, 56, 56, 0, 0, 0, 0, 1 },
    { 8, -1, 5, 76, 76, 0, 0, 0, 0, 2 },
    { 8, 5, 5, 91, 91, 0, 0, 0, 0, 3 },
    { 4, -1, 7, 36, 96, 0, 0, 0, -1, -1 },
};
#define N_DEBUT ((int)(sizeof k_debut / sizeof k_debut[0]))

static int stat_of(int k) {
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

static int met(int g) { return (U32(REC(g) + 12) >> 1) & 1; }
static int home(int g) { return (U32(REC(g) + 12) >> 20) & 0xF; }
static int yuko_stage(void) { return (U32(REC(8) + 12) >> 9) & 7; }

/* the rule's test for girl state now; hi: check2's thresholds */
static int rule_ok(const DebutRule *r, int hi) {
    if (met(r->girl)) return 0;
    if (r->act >= 0 && (int)(sched_icon & 0xF) != r->act) return 0;
    if (r->stage >= 0 && yuko_stage() != r->stage) return 0;
    if (r->not_home > 0 && home(0) == r->not_home) return 0;
    if (r->not_home < 0 && home(r->girl) == (basyo_pack >> 12)) return 0;
    if (stat_of(r->s1) < (hi ? r->t1hi : r->t1lo)) return 0;
    if (r->s2 && stat_of(r->s2) < (hi ? r->t2hi : r->t2lo)) return 0;
    return 1;
}

int guide_debut_die24(int mode) { return mode == 1 ? 3 : 8; }

int guide_debut_pick(int mode, int *sub) {
    int i, club = basyo_pack >> 12;
    *sub = 0;
    if (mode == 2) {
        /* the club route: girls 1..4 whose home place is your club after
         * 21 sessions there; Yukari at place 8 (21 sessions), Nozomi at 9 */
        static const int sub_at[5][2] = { { 0, 4 }, { 2, 4 }, { 4, 1 }, { 6, 1 }, { 0, 0 } };
        int sessions = U16(bukatu_rec + 4 * (club & 15));
        for (i = 1; i <= 4; i++)
            if (!met(i) && home(i) == club && sessions >= 21) {
                if (i == 1) *sub = club == 0 ? 2 : 4;   /* 2 or 3 by the frame */
                else if (i == 2) *sub = club == 2 ? 4 : 5;
                else *sub = club == sub_at[i - 1][0] ? sub_at[i - 1][1] : 2;
                return i;
            }
        if (!met(5) && club == 8 && home(0) != 8 && sessions >= 21) { *sub = 1; return 5; }
        if (!met(6) && club == 9 && home(0) != 9) { *sub = 1; return 6; }
        return 0;
    }
    for (i = 0; i < N_DEBUT; i++)
        if (rule_ok(&k_debut[i], mode == 1)) {
            *sub = k_debut[i].stage >= 0 ? k_debut[i].stage : 0;
            return k_debut[i].girl;
        }
    return 0;
}

/* the rule text pieces for the guide (guide_model.c) */
int guide_debut_rules(int girl, int idx[4]) {
    int i, n = 0;
    for (i = 0; i < N_DEBUT && n < 4; i++)
        if (k_debut[i].girl == girl) idx[n++] = i;
    return n;
}
void guide_debut_rule(int i, int *act, int *s1, int *lo1, int *hi1, int *s2, int *lo2,
                      int *hi2, int *not_home, int *stage) {
    const DebutRule *r = &k_debut[i];
    *act = r->act; *s1 = r->s1; *lo1 = r->t1lo; *hi1 = r->t1hi;
    *s2 = r->s2; *lo2 = r->t2lo; *hi2 = r->t2hi; *not_home = r->not_home; *stage = r->stage;
}
int guide_debut_rule_ok(int i, int hi) { return rule_ok(&k_debut[i], hi); }
int guide_girl_met(int g) { return met(g); }
int guide_girl_home(int g) { return home(g); }
int guide_yuko_stage(void) { return yuko_stage(); }
int guide_club_sessions(int club) { return U16(bukatu_rec + 4 * (club & 15)); }
/* schedule_make's count: met girls among 1..9 (2+ selects touzyou_check2) */
int guide_met_1_9(void) {
    int i, n = 0;
    for (i = 1; i < 10; i++) n += met(i);
    return n;
}

/* ---- illness: byouki_hassei (0x80072E30), healthy branch ----
 * stamina < 20 and rand() & 0xFF above the limit (or stamina 0): state 1;
 * stress > 80 and album_work's low byte above the limit: + 2.  Only while
 * byouki_state >> 5 is 0. */
int guide_ill_limit(void) {
    int lim = para_kon / 8 + 204;
    return lim > 254 ? 254 : lim;
}

void guide_ill_onset(int out[4]) {
    int lim = guide_ill_limit(), a, b;
    long n[4] = { 0, 0, 0, 0 };
    for (a = 0; a < 256; a++) {
        int s = 0;
        if (byouki_state >> 5) { n[0] += 256; continue; }
        if (para_tai < 20 && lim < a) s = 1;
        else if (para_tai == 0) s = 1;
        for (b = 0; b < 256; b++) n[s + (para_str > 80 && lim < b ? 2 : 0)]++;
    }
    for (a = 0; a < 4; a++) out[a] = (int)n[a];
}

/* ---- girl_counter_periodic_decrement (0x80070D04): term planning nights ---- */
int guide_decay(int girl, int day) {
    if (!met(girl) || S16(REC(girl) + 6) <= 0) return 0;
    if (day / 7 == 0) return 1;
    if (day / 7 == 2 && girl == 0) return 1;
    return 0;
}

/* ---- the moods ---- */
int guide_mood_h(int y, int t) {     /* get_g_zyotai_h (0x80051F68) */
    if ((unsigned)y < 10) return 4;
    if ((unsigned)y < 50) return 3;
    if ((unsigned)t < 50) return 2;
    if ((unsigned)t < 80) return 1;
    return 0;
}
int guide_mood_s(int y, int t) {     /* get_g_zyotai_s (0x80051E88) */
    if ((unsigned)y < 10) return 4;
    if ((unsigned)y < 30) return 3;
    if ((unsigned)y < 50) return (unsigned)t < 50 ? 3 : 1;
    if ((unsigned)y < 80) return (unsigned)t < 50 ? 2 : 1;
    return (unsigned)t < 50 ? 0x82 : 0x80;
}

/* syoushin_up (0x80070ED0), at each term planning night: +5 bomb for a
 * met girl under 10 % friendship */
int guide_neglect(int girl) {
    return met(girl) && gsv_yuukou_pct(girl) < 10 ? 5 : 0;
}

/* byouki_hassei's next state from STATE (byouki_kind & 15) with the day's
 * draw A (rand() & 0xFF) and B (album_work & 0xFF): 0..3, or -1 when the
 * day goes to the visit check (state 3 that stays) */
int guide_ill_next(int state, int a, int b) {
    int lim = guide_ill_limit(), n;
    int cold = (para_tai < 20 && lim < a) || para_tai == 0;
    int nerves = para_str > 80 && lim < b;
    switch (state) {
    case 0:
        if (byouki_state >> 5) return 0;
        return (cold ? 1 : 0) + (nerves ? 2 : 0);
    case 1:
        n = nerves ? 3 : 1;
        return para_tai > 30 ? 0 : n;
    case 2:
        n = cold ? 3 : 2;
        return para_str < 70 ? 0 : n;
    case 3:
        return para_str < 70 && para_tai > 30 ? 0 : 3;
    default:
        return 0;
    }
}
