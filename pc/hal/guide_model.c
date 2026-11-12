/* SPDX-License-Identifier: GPL-3.0-or-later */
/* guide_model.c -- the text of the GUIDE pages (guide_model.h).
 *
 * The prose and the rule statements are ours; every number is either a
 * table of the player's game read at run time (PS1 RAM, or an overlay
 * file of the player's disc for the overlays' tables) or a constant of
 * the game's code, transcribed in pc/hal/guide_rules.c and checked
 * against the game's functions by pc/tests/guide_harness.c.  Each section
 * ends with a "Source:" line naming the function or table (shown only when
 * guide_show_sources is set: the harness, never the player's game); the full
 * evidence is docs/game-flows/MECHANICS-GUIDE.md (MG below).
 *
 * Read only; SDL-free (text is measured and wrapped with pc/hal/ui_font.c).
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "types.h"
#include "pc_ui.h"
#include "guide_model.h"

/* guide_rules.c */
int guide_debut_rules(int girl, int idx[4]);
void guide_debut_rule(int i, int *act, int *s1, int *lo1, int *hi1, int *s2, int *lo2,
                      int *hi2, int *not_home, int *stage);
int guide_debut_rule_ok(int i, int hi);
int guide_girl_met(int g);
int guide_girl_home(int g);
int guide_yuko_stage(void);
int guide_club_sessions(int club);
int guide_met_1_9(void);

/* tables read from RAM (the executable's data) */
extern u8 get_h_yuukou_str0[];
extern u8 get_h_tokimeki_tbl0[], get_h_tokimeki_tbl1[], get_h_tokimeki_tbl2[];
extern s8 h_tokimeki_mod_tbl[];
extern u8 get_h_tokimeki_tbl3, get_h_tokimeki_tbl4, get_h_tokimeki_tbl5,
    get_h_tokimeki_tbl6, get_h_tokimeki_tbl7, get_h_tokimeki_tbl8, get_h_tokimeki_tbl9;
extern u8 get_grp_point_tbl0[];
extern s8 grp_table_calc_tbl0[];
extern u8 grp_table_calc_tbl1[], grp_table_calc_tbl2[];
extern s8 grp_table_calc_tbl3[], grp_table_calc_tbl4[];
extern u16 basyo_pack;

/* ---- names (ours: translation/localization/CHARACTER-BIBLE.md) ---- */

static const char *const k_girl[GUIDE_NGIRLS] = {
    "Shiori Fujisaki", "Mio Kisaragi", "Yuina Himoo", "Ayako Katagiri", "Saki Nijino",
    "Yukari Koshiki", "Nozomi Kiyokawa", "Mira Kagami", "Yuko Asahina", "Megumi Mikihara",
    "Yumi Saotome", "Miharu Tatebayashi", "Rei Ijuin",
};
static const char *const k_first[GUIDE_NGIRLS] = {
    "Shiori", "Mio", "Yuina", "Ayako", "Saki", "Yukari", "Nozomi", "Mira", "Yuko",
    "Megumi", "Yumi", "Miharu", "Rei",
};
static const char *const k_stat[9] = {
    "Stamina", "Humanities", "Science", "Art", "Sports", "Trivia", "Looks", "Guts", "Stress",
};
static const char *const k_stat3[9] = {
    "Sta", "Hum", "Sci", "Art", "Spo", "Tri", "Loo", "Gut", "Str",
};
static const char *const k_act[8] = {
    "Humanities", "Science", "Art", "Sports", "Club", "Trivia", "Looks", "Rest",
};
/* basyo_pack >> 12 (gsv_club_name's order, MG 2) */
static const char *const k_club[12] = {
    "Literature", "Drama", "Science", "Computer", "Art", "Brass band", "Baseball",
    "Soccer", "Tennis", "Swimming", "Basketball", "None",
};

const char *guide_girl_name(int g) { return g >= 0 && g < GUIDE_NGIRLS ? k_girl[g] : "?"; }

const char *guide_tab_name(int t) {
    static const char *const n[GUIDE_TABS] = { "Activities", "Girls", "Events", "Rules" };
    return t >= 0 && t < GUIDE_TABS ? n[t] : "?";
}

/* ---- the document ---- */

static GuideDoc *D;

static GuideLine *new_line(int kind, unsigned col) {
    GuideLine *l;
    if (D->n >= GUIDE_MAX_LINES) return NULL;
    l = &D->l[D->n++];
    memset(l, 0, sizeof *l);
    l->kind = (unsigned char)kind;
    l->col = col;
    return l;
}

/* a paragraph, wrapped to the page at INDENT */
static void para_v(unsigned col, int kind, int indent, const char *fmt, va_list ap) {
    char buf[1024];
    UiLine w[24];
    int n, i;
    vsnprintf(buf, sizeof buf, fmt, ap);
    n = ui_text_wrap(buf, D->width - indent, w, 24);
    for (i = 0; i < n; i++) {
        GuideLine *l = new_line(kind, col);
        if (!l) return;
        l->indent = (short)indent;
        snprintf(l->text, sizeof l->text, "%.*s", w[i].n, w[i].s);
    }
}

static void P(unsigned col, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void P(unsigned col, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    para_v(col, GL_TEXT, 0, fmt, ap);
    va_end(ap);
}

/* a bullet: "- " then the text indented under it */
static void B(unsigned col, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void B(unsigned col, const char *fmt, ...) {
    va_list ap;
    int first = D->n;
    va_start(ap, fmt);
    para_v(col, GL_TEXT, 8, fmt, ap);
    va_end(ap);
    if (first < D->n) {
        /* the dash on the first line */
        GuideLine *l = &D->l[first];
        char t[GUIDE_TEXT];
        snprintf(t, sizeof t, "%s", l->text);
        snprintf(l->text, sizeof l->text, "- %s", t);
        l->indent = 1;
    }
}

static void H(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void H(const char *fmt, ...) {
    va_list ap;
    if (D->n) new_line(GL_GAP, 0);
    va_start(ap, fmt);
    para_v(GUIDE_C_HEAD, GL_HEAD, 0, fmt, ap);
    va_end(ap);
}

static void SRC(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* The "Source:" lines name the game's functions and tables: provenance for
 * the people who maintain the guide, not for players.  Off in the game; the
 * rules harness (pc/tests/guide_harness.c) turns them on to check that
 * every section still carries one. */
int guide_show_sources;

static void SRC(const char *fmt, ...) {
    char buf[600];
    va_list ap;
    if (!guide_show_sources) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    P(GUIDE_C_DIM, "Source: %s", buf);
    /* retag as source lines */
    {
        int i;
        for (i = D->n - 1; i >= 0 && D->l[i].kind == GL_TEXT && D->l[i].col == GUIDE_C_DIM; i--) {
            D->l[i].kind = GL_SRC;
            if (!strncmp(D->l[i].text, "Source:", 7)) break;
        }
    }
}

/* a table row: label then cells right-aligned at cx[] */
static GuideLine *ROW(unsigned col, const char *label, int n, const short *cx) {
    GuideLine *l = new_line(GL_ROW, col);
    if (!l) return NULL;
    snprintf(l->text, sizeof l->text, "%s", label);
    l->ncell = (unsigned char)(n > GUIDE_CELLS ? GUIDE_CELLS : n);
    memcpy(l->cx, cx, sizeof(short) * l->ncell);
    return l;
}

static void cell(GuideLine *l, int i, unsigned col, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
static void cell(GuideLine *l, int i, unsigned col, const char *fmt, ...) {
    va_list ap;
    if (!l || i >= l->ncell) return;
    va_start(ap, fmt);
    vsnprintf(l->cell[i], sizeof l->cell[i], fmt, ap);
    va_end(ap);
    l->ccol[i] = col;
}

/* 16.16 points as "+0.9" (one decimal, rounded half away from zero) */
static void pts(char *out, int n, int v16) {
    int neg = v16 < 0, a = neg ? -v16 : v16, tenths = (a * 10 + 32768) / 65536;
    if (tenths == 0) snprintf(out, (size_t)n, "0");
    else snprintf(out, (size_t)n, "%c%d.%d", neg ? '-' : '+', tenths / 10, tenths % 10);
}

static int live(const GsvState *s) { return s && s->in_game; }

static unsigned yes_col(int ok) { return ok ? GUIDE_C_GOOD : GUIDE_C_TEXT; }

/* ---- overlay tables, read from the player's disc files ---- */

/* TEL: the explosion penalties (tel_ring_timeout, D_80144448 tokimeki /
 * D_80144454 friendship) and the stood-up cost (tel_girl_adjust,
 * u16 D_80144430) */
typedef struct {
    int ok;
    u8 tok[11], yuu[11];
    u16 stood[11];
} TelTables;

static const TelTables *tel_tables(void) {
    static TelTables t;
    static int done;
    if (!done) {
        u8 raw[22];
        int i;
        done = 1;
        t.ok = guide_exn_read("TEL", 0x80144448u, t.tok, 11) &&
               guide_exn_read("TEL", 0x80144454u, t.yuu, 11) &&
               guide_exn_read("TEL", 0x80144430u, raw, 22);
        for (i = 0; t.ok && i < 11; i++) t.stood[i] = (u16)(raw[2 * i] | raw[2 * i + 1] << 8);
    }
    return &t;
}

/* ---- ACTIVITIES ---- */

/* nine columns 24 units apart; a gain is shown without '+' (the colour
 * says it), so a cell is at most "-2.4" wide */
static const short k_act_cx[9] = { 84, 108, 132, 156, 180, 204, 228, 252, 276 };

static void act_row(const char *label, int act, int club, unsigned col, const GsvState *s) {
    GuideLine *l = ROW(col, label, 9, k_act_cx);
    int k;
    for (k = 0; k < 9; k++) {
        char b[10];
        int v = guide_act_delta(act, club, k, 0, 0, 0);
        pts(b, sizeof b, v);
        if (b[0] == '+') memmove(b, b + 1, strlen(b));
        cell(l, k, v > 0 ? (k == 8 ? GUIDE_C_WARN : GUIDE_C_GOOD) : v < 0 ? (k == 8 ? GUIDE_C_GOOD : GUIDE_C_BAD) : GUIDE_C_DIM, "%s", b);
    }
    (void)s;
}

static void page_activities(const GsvState *s) {
    int a, c, k, cur = live(s) ? s->activity : -1, club = live(s) ? s->club : -1;
    GuideLine *l;
    H("The weekly plan");
    P(GUIDE_C_TEXT, "You choose one activity for the week. Every school day applies the "
      "activity's row once, after school. On a Sunday or holiday you can do it once more, "
      "and then every gain counts four times.");
    H("Change per school day (points)");
    l = ROW(GUIDE_C_DIM, "", 9, k_act_cx);
    for (k = 0; k < 9; k++) cell(l, k, GUIDE_C_DIM, "%s", k_stat3[k]);
    for (a = 0; a < 8; a++) {
        if (a == 4) continue;
        act_row(k_act[a], a, 0, a == cur ? GUIDE_C_HEAD : GUIDE_C_TEXT, s);
    }
    P(GUIDE_C_DIM, "Club (you can only do your own club's row):");
    for (c = 0; c < 11; c++)
        act_row(k_club[c], 4, c, c == club && cur == 4 ? GUIDE_C_HEAD
                : c == club ? GUIDE_C_GOOD : GUIDE_C_TEXT, s);
    if (cur >= 0)
        P(GUIDE_C_HEAD, "Yellow: your plan this week (%s%s%s).", k_act[cur & 7],
          cur == 4 ? ": " : "", cur == 4 ? k_club[club >= 0 && club < 12 ? club : 11] : "");
    SRC("parameter_up_down (0x80074890) with the rows parameter_up_down_tbl0 "
        "(0x800BCDE0) and _tbl1 (0x800BCE28) read from your game; one point = 16 table "
        "units. MG 1.1");

    H("What changes the amounts");
    B(GUIDE_C_TEXT, "Gains are multiplied, losses never are: a negative entry always "
      "costs exactly the amount shown.");
    B(GUIDE_C_TEXT, "Sunday or holiday session: gains x4.");
    B(GUIDE_C_TEXT, "Poor day: gains halved, and stress goes up by one more point "
      "(a stress reduction is halved too, then +1).");
    B(GUIDE_C_TEXT, "Ill (both stamina and stress, state 3): gains halved. A cold or "
      "nerves alone (state 1 or 2): gains halved except for Trivia, Looks and Rest.");
    B(GUIDE_C_TEXT, "Every parameter stays between 0 and 999.");
    B(GUIDE_C_TEXT, "Joining a club gives +1.5 to its subject (Literature and Drama: "
      "Humanities; Science and Computer: Science; Art and Brass band: Art; sports "
      "clubs: Sports and Guts) and counts two sessions.");
    {
        char b1[10], b2[10], b3[10], b4[10];
        pts(b1, sizeof b1, guide_act_delta(1, 0, 2, 0, 0, 0));
        pts(b2, sizeof b2, guide_act_delta(1, 0, 2, 1, 0, 0));
        pts(b3, sizeof b3, guide_act_delta(1, 0, 2, 1, 1, 0));
        pts(b4, sizeof b4, guide_act_delta(1, 0, 2, 0, 1, 3));
        P(GUIDE_C_DIM, "Example, Science from a Science session: school day %s, holiday %s, "
          "poor holiday %s, poor school day while ill %s.", b1, b2, b3, b4);
    }
    SRC("parameter_up_down: holiday a1 = 4, poor a2 = 2, byouki_kind & 15 = 3 xdiv, 1/2 "
        "ydiv; parameter_change (0x80063A88) clamps; kangei_bukatu_para_set (KANGEI "
        "0x80133A6C). MG 1.1-1.2");

    H("Poor days (the biorhythm)");
    P(GUIDE_C_TEXT, "When the week is planned, each school day gets a hidden roll. "
      "Your ability for the day is the activity's parameter / 16 (club: sessions / 16) "
      "plus the biorhythm curve that activity uses (up to +16), plus 5. A random "
      "0..ability+3 is cut by stress (x7/8 at stress 32+, x6/8 at 64+, x5/8 at 96+, "
      "x4/8 at 97+) and quartered when all three curves are low. Under 5 is a poor day.");
    B(GUIDE_C_TEXT, "Curves: body (Sports, sports clubs), mood (Art, Trivia, Looks, "
      "Drama, Art and Brass band clubs), mind (Humanities, Science, Literature, Science "
      "and Computer clubs). They repeat about every 11.5, 14 and 16.5 days (the game "
      "advances them two steps a day). A Rest day is never a poor day.");
    B(GUIDE_C_TEXT, "So a high parameter, low stress and a good curve make poor days rare.");
    if (live(s)) {
        int d;
        char line[200];
        int n = snprintf(line, sizeof line, "Your chance of a poor day (%s), today and the "
                         "next 6 days:", k_act[s->activity & 7]);
        (void)n;
        P(GUIDE_C_HEAD, "%s", line);
        l = ROW(GUIDE_C_TEXT, "", 7, k_act_cx + 2);
        for (d = 0; d < 7; d++) {
            int cnt = guide_poor_count(d), pct = (cnt * 100 + 16384) / 32768;
            cell(l, d, pct >= 50 ? GUIDE_C_BAD : pct >= 20 ? GUIDE_C_WARN : GUIDE_C_GOOD, "%d%%", pct);
        }
    }
    SRC("biorhythm_sf_set (0x8006BE48) via schedule_make (0x8006D890), biorhythm_sf2 "
        "(0x8006C3D4), get_pass_days (0x8006C7A0), rsin_tbl. MG 1.3");
}

/* ---- GIRLS ---- */

static void stat_list(char *out, int n, u32 mask, int shift_base) {
    int k, first = 1, len = 0;
    out[0] = 0;
    for (k = 1; k <= 7; k++)
        if (mask >> (shift_base ? 8 - k - 1 + 1 : k) & 1) {
            len += snprintf(out + len, (size_t)(n - len), "%s%s", first ? "" : ", ", k_stat[k]);
            first = 0;
            if (len >= n) break;
        }
    if (first) snprintf(out, (size_t)n, "none");
}

/* the ladder limits of get_h_tokimeki (0x80052A68): the base thresholds
 * get_h_tokimeki_tbl3..9 plus the mode's offsets (code constants) */
static int ladder_limit(int mode, int k) {
    static const signed char add[7][7] = {
        { 0, 0, 0, 0, 0, 0, 0 },
        { 50, 35, 20, 20, 5, 5, 0 },
        { 0, 0, 0, 0, 0, 0, 0 },
        { 50, 50, 50, 20, 20, 20, 20 },
        { 10, 10, 10, 10, 0, 0, 0 },
        { 20, 20, 20, 20, 0, -5, 0 },
        { 0, 0, 0, 0, -5, -5, -5 },
    };
    u8 thr[7];
    thr[0] = get_h_tokimeki_tbl3; thr[1] = get_h_tokimeki_tbl4;
    thr[2] = get_h_tokimeki_tbl5; thr[3] = get_h_tokimeki_tbl6;
    thr[4] = get_h_tokimeki_tbl7; thr[5] = get_h_tokimeki_tbl8;
    thr[6] = get_h_tokimeki_tbl9;
    if (mode < 1 || mode > 6 || k < 0 || k > 6) return 0;
    return thr[k] + add[mode][k];
}

/* the guide's own tokimeki%: base * weight / 100 plus the ladder, as the
 * page explains it (compared with get_h_tokimeki by the harness) */
int guide_tokimeki_pct(int g, const int stats[9], int base) {
    int v = base * get_h_tokimeki_tbl0[g] / 100, b, k, mode = get_h_tokimeki_tbl2[g];
    for (b = 0; b < 6; b++) {
        if (!(get_h_tokimeki_tbl1[g] >> (b + 1) & 1) || mode < 1 || mode > 6) continue;
        for (k = 0; k < 7; k++)
            if (ladder_limit(mode, k) < stats[b + 1]) break;
        v += h_tokimeki_mod_tbl[g * 8 + k];
    }
    return v > 100 ? 100 : v < 0 ? 0 : v;
}

static const char *act_phrase(int act) {
    static char b[40];
    if (act < 0) return "any week";
    snprintf(b, sizeof b, "%s week", k_act[act]);
    return b;
}

static void debut_section(int g, const GsvState *s) {
    int idx[4], n = guide_debut_rules(g, idx), i, met = live(s) && s->girl[g].met;
    int hi = live(s) && guide_met_1_9() >= 2;   /* touzyou_check2 from then on */
    H("How you meet her");
    if (g == 0) {
        P(GUIDE_C_TEXT, "Your childhood friend: met from the first day. Her start "
          "(friendship, tokimeki, bomb) depends on your blood types: 45/10/10, "
          "50/20/0, 50/25/0 or 55/25/0.");
        SRC("girl_rec_init (0x80041DEC), name_ent_girl_para_set (NAME_ENT 0x8013909C). MG 2.1");
        return;
    }
    if (live(s)) P(met ? GUIDE_C_GOOD : GUIDE_C_WARN, met ? "You have met her." : "Not met yet.");
    if (n) {
        P(GUIDE_C_TEXT, "A school day of a planned week can bring her first meeting if, "
          "when the week is planned:");
        for (i = 0; i < n; i++) {
            int act, s1, lo1, hi1, s2, lo2, hi2, nh, st, ok;
            char req[200];
            guide_debut_rule(idx[i], &act, &s1, &lo1, &hi1, &s2, &lo2, &hi2, &nh, &st);
            if (lo1 == hi1)
                snprintf(req, sizeof req, "%s %d+", k_stat[s1], lo1);
            else
                snprintf(req, sizeof req, "%s %d+ (%d+ once two girls are met)", k_stat[s1],
                         lo1, hi1);
            if (s2) {
                size_t L = strlen(req);
                if (lo2 == hi2) snprintf(req + L, sizeof req - L, " and %s %d+", k_stat[s2], lo2);
                else snprintf(req + L, sizeof req - L, " and %s %d+ (%d+)", k_stat[s2], lo2, hi2);
            }
            ok = live(s) && !met && guide_debut_rule_ok(idx[i], hi);
            if (st >= 0)
                B(yes_col(ok), "Stage %d of 4: %s, %s.%s", st + 1, req,
                  act >= 0 ? "in a Trivia week" : "any week", ok ? " You qualify now." : "");
            else
                B(yes_col(ok), "%s: %s.%s%s", act >= 0 ? act_phrase(act) : "Any week", req,
                  nh > 0 ? (nh == 8 ? " Not if Shiori's home place is the tennis club."
                                    : " Not if Shiori's home place is the swimming club.")
                         : nh < 0 ? " Not while you are in her home club." : "",
                  ok ? " You qualify now." : "");
        }
        P(GUIDE_C_TEXT, "Each school day rolls a die: 1 in 3 while fewer than two other "
          "girls are met, else 1 in 8. At most one first meeting a week, never in April "
          "1995, not in exam weeks and not after the first two years (727 days).");
        if (g == 8)
            P(GUIDE_C_TEXT, "Yuko needs four meetings, one per stage; she counts as met "
              "after the fourth.%s", live(s) ? "" : "");
        if (g >= 1 && g <= 6) {
            P(GUIDE_C_TEXT, "Club route: in a club week, if your club is her home club "
              "and you have 21+ sessions there, the club can introduce her (1 in 3 a "
              "day). Yukari: the tennis club; Nozomi: the swimming club (no session "
              "count).");
            if (live(s) && g <= 4) {
                int h = guide_girl_home(g);
                P(GUIDE_C_DIM, "Her home club in this game: %s; your sessions there: %d.",
                  k_club[(h & 15) > 11 ? 11 : (h & 15)], h < 11 ? guide_club_sessions(h) : 0);
            }
        }
        SRC("schedule_make (0x8006D890), touzyou_check (0x8006EC08), touzyou_check2 "
            "(0x8006EF68), touzyou_basyo_check (0x8006F2BC). MG 2.2");
        return;
    }
    switch (g) {
    case 9:
        P(GUIDE_C_TEXT, "Megumi has no week rule. She is introduced at the Christmas "
          "party (24 Dec) if her tokimeki base is 20 or more (half the time), or on "
          "Valentine's Day with the same 20. Every school event that raises all girls' "
          "tokimeki raises hers, so this comes by itself.");
        SRC("shougatu_girl9_judge (SHOUGATU 0x80140BB0, the Christmas step 82), "
            "valen_list_init (VALEN 0x80132834), valen_result_serifu_set (VALEN "
            "0x80133ABC). MG 2.3");
        break;
    case 10:
        P(GUIDE_C_TEXT, "Yumi (Yoshio's sister) appears at the start of the second year "
          "(the April 1996 new-term event) and starts at friendship 40.");
        SRC("schedule_make, calendar event 2 in 1996. MG 2.3");
        break;
    case 11:
        P(GUIDE_C_TEXT, "Miharu is never 'met'. You bump into her in the corridor: a "
          "4%% chance in each random-event slot from the 30th day on, at most once every "
          "4 weeks, five times in all, not while you are ill.");
        SRC("girls_event_pick (0x8006FBF0), geko_girl11_serifuev_add (GEKO 0x80140F48). MG 2.4");
        break;
    case 12:
        P(GUIDE_C_TEXT, "Rei is the student-council president; the game treats her as "
          "known from the start. Her story runs on phone calls to her.");
        SRC("girl_rec_init; denwa_cnt (tel_dialog40, TEL 0x80138014). MG 2.4");
        break;
    }
}

static void likes_section(int g, const GsvState *s) {
    char b[160];
    int mode, k, cur[9], i;
    H("Her feelings");
    if (g > 10) {
        P(GUIDE_C_TEXT, "Not scored like the others (see her ending).");
        return;
    }
    P(GUIDE_C_TEXT, "Friendship %% = friendship x %d / 100 (100%% at %d). Tokimeki %% = "
      "tokimeki x %d / 100 plus one bonus per parameter she looks at:",
      get_h_yuukou_str0[g], (100 * 100 + get_h_yuukou_str0[g] - 1) / get_h_yuukou_str0[g],
      get_h_tokimeki_tbl0[g]);
    mode = get_h_tokimeki_tbl2[g];
    for (i = 0; i < 9; i++) cur[i] = live(s) ? s->stats[i] : 0;
    /* one ladder per girl (her mode's limits, her values), applied to each
     * parameter of her mask: the names first, then the steps once */
    {
        int j, len = 0, first = 1, any = 0;
        for (k = 1; k <= 6; k++) {
            if (!(get_h_tokimeki_tbl1[g] >> k & 1)) continue;
            len += snprintf(b + len, sizeof b - (size_t)len, "%s%s", first ? "" : ", ", k_stat[k]);
            first = 0;
            any = 1;
        }
        if (any && len < (int)sizeof b) {
            len += snprintf(b + len, sizeof b - (size_t)len, ", each:");
            for (j = 0; j < 7 && len < (int)sizeof b; j++)
                len += snprintf(b + len, sizeof b - (size_t)len, " >%d %+d,",
                                ladder_limit(mode, j), h_tokimeki_mod_tbl[g * 8 + j]);
            if (len < (int)sizeof b)
                snprintf(b + len, sizeof b - (size_t)len, " else %+d", h_tokimeki_mod_tbl[g * 8 + 7]);
            B(GUIDE_C_TEXT, "%s", b);
        }
    }
    if (live(s))
        P(GUIDE_C_HEAD, "Now: friendship %d%% (%d), tokimeki %d%% (%d).", s->girl[g].yuukou_pct,
          s->girl[g].yuukou, s->girl[g].tokimeki_pct, s->girl[g].tokimeki);
    SRC("get_h_yuukou (0x800530A8, get_h_yuukou_str0), get_h_tokimeki (0x80052A68, "
        "_tbl0..9, h_tokimeki_mod_tbl). MG 3.1");
}

static void ending_section(int g, const GsvState *s) {
    char b[160];
    int k;
    H("Her ending");
    if (g == 0) {
        const char *nm[7] = { "Hum", "Sci", "Art", "Spo", "Tri", "Loo", "Gut" };
        int len = 0;
        for (k = 0; k < 7; k++)
            len += snprintf(b + len, sizeof b - (size_t)len, "%s%s %d", k ? ", " : "", nm[k],
                            get_grp_point_tbl0[k]);
        P(GUIDE_C_TEXT, "Shiori confesses (score 785) only if all hold at graduation:");
        B(GUIDE_C_TEXT, "Her icon 85+ (friendship%% and tokimeki%% both 85+).");
        B(GUIDE_C_TEXT, "Parameters at least: %s.", b);
        B(GUIDE_C_TEXT, "8 or more dates with her, bomb under 51, and you are not ill.");
        P(GUIDE_C_TEXT, "785 wins unless another girl scores more.");
        if (live(s))
            P(s->girl[0].score ? GUIDE_C_GOOD : GUIDE_C_WARN, "Now: %s (icon %d, %d dates, "
              "bomb %d).", s->girl[0].score ? "you qualify" : "not yet", s->girl[0].icon,
              s->girl[0].dates, s->girl[0].bomb);
        SRC("get_grp_point (0x80053280, get_grp_point_tbl0). MG 3.2");
        return;
    }
    if (g == 11) {
        P(GUIDE_C_TEXT, "Only when no girl scores 500: a dice roll, better with more "
          "corridor meetings (up to 5) and with her two date interruptions (a date in "
          "March-May and a date at the medal corner in June-August, after two "
          "meetings). Best case 41%%. Lost if her farewell (15 Feb 1998) happened.");
        SRC("ending_select_init (ENDING 0x801330F4), thresholds at ENDING 0x8013C3E0. MG 3.4");
        return;
    }
    if (g == 12) {
        P(GUIDE_C_TEXT, "Phone her 70+ times (one call per holiday, none after 21 Feb "
          "1998), accept the first option when Yoshio says she was kidnapped (offered "
          "after 41 calls), and do not meet Shiori's 785. Then Rei wins over everyone.");
        if (live(s))
            P(GUIDE_C_HEAD, "Now: %d calls, kidnapping answer %s.", s->rei_calls,
              s->rei_answer == 2 ? "accepted" : s->rei_answer == 1 ? "refused (route closed)"
                                                                     : "not asked yet");
        SRC("ending_select_init; denwa_cnt; geko_serifu_branch_step (GEKO 0x8013BF40). MG 3.4");
        return;
    }
    P(GUIDE_C_TEXT, "Score = 4 x icon (the lower of friendship%% and tokimeki%%, in "
      "steps of 5) %+d per bomb point + a bonus for each parameter she likes + 10 per "
      "date. Under 4 dates the score is 0. 500+ can win; the highest score wins.",
      grp_table_calc_tbl0[g]);
    stat_list(b, sizeof b, (u32)grp_table_calc_tbl1[g] | grp_table_calc_tbl2[g], 0);
    {
        int len = 0, first = 1;
        b[0] = 0;
        for (k = 1; k <= 7; k++) {
            int n = (grp_table_calc_tbl1[g] >> k & 1) + (grp_table_calc_tbl2[g] >> k & 1);
            if (!n) continue;
            len += snprintf(b + len, sizeof b - (size_t)len, "%s%s%s", first ? "" : ", ",
                            k_stat[k], n == 2 ? " (counts twice)" : "");
            first = 0;
        }
        B(GUIDE_C_TEXT, "Liked parameters: %s.", first ? "none" : b);
    }
    B(GUIDE_C_TEXT, "Bonus per liked parameter: under 10 %+d, 40-49 %+d, 100-109 %+d, "
      "140+ %+d (steps of 10).", grp_table_calc_tbl3[0], grp_table_calc_tbl3[4],
      grp_table_calc_tbl3[10], grp_table_calc_tbl3[14]);
    if (g == 2)
        B(GUIDE_C_TEXT, "Yuina also scores your stress: under 10 %+d, 70-79 %+d, 140+ %+d.",
          grp_table_calc_tbl4[0], grp_table_calc_tbl4[7], grp_table_calc_tbl4[14]);
    if (live(s))
        P(s->girl[g].score >= 500 ? GUIDE_C_GOOD : GUIDE_C_HEAD, "Now: score %d (icon %d, %d "
          "dates, bomb %d).", s->girl[g].score, s->girl[g].icon, s->girl[g].dates,
          s->girl[g].bomb);
    SRC("get_grp (0x800537F0), grp_table_calc (0x80053364, grp_table_calc_tbl0..4), "
        "ending_select_init. MG 3.3");
}

static void bomb_section(int g, const GsvState *s) {
    const TelTables *t = tel_tables();
    if (g > 10) return;
    H("Her bomb");
    P(GUIDE_C_TEXT, "Hidden anger, 0-128. 60+: Yoshio's screen shows a bomb. 61: a "
      "rumour spreads. 76: she confronts you. 91: Yoshio calls and it explodes "
      "(not in 1998 or in vacations).");
    if (t->ok)
        B(GUIDE_C_TEXT, "Explosion: her bomb to 0, and every met girl loses her own "
          "penalty; %s's is tokimeki -%d, friendship -%d.", k_first[g], t->tok[g], t->yuu[g]);
    if (t->ok)
        B(GUIDE_C_TEXT, "Standing her up: bomb +%d and friendship -%d.", t->stood[g], t->stood[g]);
    if (!t->ok)
        P(GUIDE_C_DIM, "(The penalties are read from your TEL.EXN, which was not found.)");
    if (live(s))
        P(s->girl[g].bomb >= 61 ? GUIDE_C_BAD : s->girl[g].bomb >= 50 ? GUIDE_C_WARN
          : GUIDE_C_HEAD, "Now: bomb %d.", s->girl[g].bomb);
    SRC("schedule_day_main (0x800713EC), tel_ring_timeout (TEL, D_80144448 / D_80144454), "
        "tel_girl_adjust (TEL 0x80139D6C, D_80144430). MG 4");
}

/* the meeting places 0..20 (machiawase_menu_set_tbl0's order) */
static const char *const k_place[21] = {
    "Neighbourhood park", "Central park", "Shopping street", "Aquarium", "Zoo",
    "Botanical garden", "Planetarium", "Art museum", "Library", "Game centre", "Bowling",
    "Karaoke", "Amusement park", "Stadium", "Cinema", "Concert hall", "Pool", "Beach",
    "Shrine", "Skating rink", "Ski slope",
};
const char *guide_place_name(int p) { return p >= 0 && p < 21 ? k_place[p] : "?"; }

static const short k_place_cx[3] = { 150, 214, 262 };

static void dates_section(int g, const GsvState *s) {
    static const char *const grade[5] = { "loves", "likes", "likes", "so-so", "dislikes" };
    int p, ok = guide_tel_like(g, 0) >= 0 && guide_date_cell(g, 0) >= 0;
    GuideLine *l;
    if (g > 10) return;
    H("Dates with her");
    if (!ok) {
        P(GUIDE_C_DIM, "(Read from your TEL.EXN and DATE.EXN, which were not found.)");
        return;
    }
    P(GUIDE_C_TEXT, "Phone her from a Sunday or holiday screen to ask her out. She says "
      "yes with a chance of (the higher of icon / 4 and Trivia / 4, plus her liking of "
      "the place) in 256; liking 0 is always no; Looks under 40 cost the difference. "
      "On the date, the place's grade multiplies what each answer does (Rules).");
    l = ROW(GUIDE_C_DIM, "Place", 3, k_place_cx);
    cell(l, 0, GUIDE_C_DIM, "Liking");
    cell(l, 1, GUIDE_C_DIM, "Date");
    cell(l, 2, GUIDE_C_DIM, "%s", live(s) ? "Yes now" : "");
    for (p = 0; p < 21; p++) {
        int like = guide_tel_like(g, p), c = guide_date_cell(g, p);
        l = ROW(GUIDE_C_TEXT, k_place[p], 3, k_place_cx);
        cell(l, 0, like == 0 ? GUIDE_C_BAD : like >= 179 ? GUIDE_C_GOOD : GUIDE_C_TEXT, "%d",
             like);
        if (like == 0)
            cell(l, 1, GUIDE_C_DIM, "-");       /* never accepted: no date there */
        else
            cell(l, 1, c == 0 ? GUIDE_C_GOOD : c >= 4 ? GUIDE_C_BAD : GUIDE_C_TEXT, "%s",
                 c >= 0 && c < 5 ? grade[c] : "?");
        if (live(s)) {
            int a = guide_accept_a(g, p, s->girl[g].icon), pct;
            if (a < 0) a = 0;
            if (a > 256) a = 256;
            pct = (a * 100 + 128) / 256;
            cell(l, 2, pct >= 75 ? GUIDE_C_GOOD : pct < 40 ? GUIDE_C_WARN : GUIDE_C_TEXT, "%d%%",
                 pct);
        }
    }
    P(GUIDE_C_DIM, "A place must also be open and in season (Rules). Every call that gets "
      "this far takes 12 off her bomb; a no costs you stress +9 and gives Guts +0..7.");
    if (live(s)) {
        int inv = guide_invite_ok(0, g, s->girl[g].icon), walk = guide_invite_ok(1, g, s->girl[g].icon);
        P(inv || walk ? GUIDE_C_GOOD : GUIDE_C_HEAD, "Now: she %s ask you out (icon 33+) and "
          "%s walk home with you (icon 17+).", inv ? "can" : "will not",
          walk ? "can" : "will not");
    } else {
        P(GUIDE_C_TEXT, "She can ask you out herself with icon 33+, and walk home with you "
          "with icon 17+, while her bomb is under 60 and her friendship 10%%+ (Rules).");
    }
    SRC("tel_dialog20 (TEL 0x80136620) with tel_basyo_suki_tbl (TEL 0x801410C4), "
        "date_rank_grade_get (DATE 0x8013C3AC) with D_8015B780, date_sasoi_judge "
        "(0x80070910), geko_judge (0x80070754); tables read from your disc. MG 6.3-6.5");
}

static void special_section(int g) {
    static const char *const notes[GUIDE_NGIRLS] = {
        "Her friendship fades twice a month (the 1st-6th and the 14th-20th), the "
        "others' once. She is never removed on 26 Feb.",
        "Her ending has a variant without glasses if you gave the first answer to her "
        "glasses question on a date. When she asks you out she never suggests the pool, "
        "the beach or the ski slope (nor the stadium in January, March, June or November).",
        "If she is the clear top scorer (500+) on 28 Feb 1998 she challenges you with "
        "her robot (no effect on the ending). She never suggests the ski slope.",
        "When she asks you out she never suggests the pool or the beach.",
        "When she asks you out she never suggests the art museum.",
        "When she asks you out she never suggests the stadium in January, March, June "
        "or November.",
        "Her ending has a second version on later clears.",
        "Dates need Looks 35+ with her (30 with the others), or she is put off.",
        "",
        "Five liked parameters: a well-rounded you suits her, any parameter under 40 "
        "costs her score. She never suggests the ski slope (nor the stadium in January, "
        "March, June or November).",
        "Science counts twice in her score. When she asks you out she never suggests "
        "the art museum or karaoke.",
        "",
        "",
    };
    if (g < GUIDE_NGIRLS && notes[g][0]) {
        H("Good to know");
        P(GUIDE_C_TEXT, "%s", notes[g]);
        SRC("the per-girl notes of MG 3.5; suggestions: geko_girl_pick_init (GEKO). MG 6.4");
    }
}

static void page_girl(int g, const GsvState *s) {
    if (g < 0 || g >= GUIDE_NGIRLS) return;
    new_line(GL_HEAD, GUIDE_C_HEAD);
    snprintf(D->l[D->n - 1].text, GUIDE_TEXT, "%s", k_girl[g]);
    debut_section(g, s);
    likes_section(g, s);
    ending_section(g, s);
    dates_section(g, s);
    bomb_section(g, s);
    special_section(g);
}

void guide_girl_note(int g, const GsvState *s, char *out, int cap) {
    out[0] = 0;
    if (!live(s) || g < 0 || g >= GUIDE_NGIRLS) return;
    if (g <= 10) {
        const GsvGirl *x = &s->girl[g];
        if (x->met) snprintf(out, (size_t)cap, "%d%%/%d%%  score %d", x->tokimeki_pct,
                             x->yuukou_pct, x->score);
        else {
            int idx[4], n = guide_debut_rules(g, idx), i, ok = 0;
            for (i = 0; i < n; i++) ok |= guide_debut_rule_ok(idx[i], guide_met_1_9() >= 2);
            snprintf(out, (size_t)cap, ok ? "not met - you qualify now" : "not met");
        }
    } else if (g == 11) {
        snprintf(out, (size_t)cap, "%d of 5 meetings", s->miharu_count);
    } else {
        snprintf(out, (size_t)cap, "%d calls", s->rei_calls);
    }
}

/* ---- EVENTS and RULES: guide_text.c ---- */
void guide_page_events(const GsvState *s);
void guide_page_rules(const GsvState *s);

/* shared with guide_text.c */
void guide_P(unsigned col, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void guide_P(unsigned col, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    para_v(col, GL_TEXT, 0, fmt, ap);
    va_end(ap);
}
void guide_B(unsigned col, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void guide_B(unsigned col, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    B(col, "%s", buf);
}
void guide_H(const char *t) { H("%s", t); }
void guide_SRC(const char *t) { SRC("%s", t); }
GuideLine *guide_ROW(unsigned col, const char *label, int n, const short *cx) {
    return ROW(col, label, n, cx);
}
void guide_cell(GuideLine *l, int i, unsigned col, const char *fmt, ...) {
    va_list ap;
    if (!l || i >= l->ncell) return;
    va_start(ap, fmt);
    vsnprintf(l->cell[i], sizeof l->cell[i], fmt, ap);
    va_end(ap);
    l->ccol[i] = col;
}
void guide_pts(char *out, int n, int v16) { pts(out, n, v16); }
const char *guide_stat_name(int k) { return k >= 0 && k < 9 ? k_stat[k] : "?"; }
const char *guide_first_name(int g) { return g >= 0 && g < GUIDE_NGIRLS ? k_first[g] : "?"; }
const char *guide_club_name(int c) { return c >= 0 && c < 12 ? k_club[c] : "None"; }

void guide_build(GuideDoc *d, int tab, int girl, const GsvState *s, int width) {
    D = d;
    d->n = 0;
    d->width = width;
    switch (tab) {
    case GUIDE_ACTIVITIES: page_activities(s); break;
    case GUIDE_GIRLS: page_girl(girl, s); break;
    case GUIDE_EVENTS: guide_page_events(s); break;
    default: guide_page_rules(s); break;
    }
    D = NULL;
}

int guide_doc_text(const GuideDoc *d, char *buf, int cap) {
    int i, k, n = 0;
    for (i = 0; i < d->n && n < cap; i++) {
        const GuideLine *l = &d->l[i];
        if (l->kind == GL_GAP) {
            n += snprintf(buf + n, (size_t)(cap - n), "\n");
            continue;
        }
        n += snprintf(buf + n, (size_t)(cap - n), "%s%s", l->kind == GL_HEAD ? "# " : "",
                      l->text);
        for (k = 0; k < l->ncell && n < cap; k++)
            n += snprintf(buf + n, (size_t)(cap - n), " | %s", l->cell[k]);
        if (n < cap) n += snprintf(buf + n, (size_t)(cap - n), "\n");
    }
    return n < cap ? n : cap;
}
