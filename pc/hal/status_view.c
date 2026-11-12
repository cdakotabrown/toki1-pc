/* SPDX-License-Identifier: GPL-3.0-or-later */
/* status_view.c -- "status at a glance": an enhanced-only, read-only page of
 * the settings menu (pc/include/pc_menu.h custom page) over the paused game,
 * opened with Ctrl+I ([hotkeys] status) or from the menu, and an optional
 * one-line HUD ([enhanced] status_hud, off by default).
 *
 * Four tabs (LEFT / RIGHT):
 *   GIRLS     every met girl: name in the game's own glyphs (the full-name
 *             table Yoshio's evaluation screen prints, girl_full_name_tbl, drawn
 *             from the game's font in RAM), tokimeki% / friendship%, the
 *             bomb meter (lit at 60 as on Yoshio's screen), dates, ending
 *             score
 *   GIRL      the selected girl: raw values, the bomb thresholds and
 *             latches, last date spot, birthday, what her ending score is
 *             made of (liked parameters, dates, bomb weight; Shiori's
 *             checklist)
 *   PLAYER    the nine parameters, club, illness, this week's activity, and
 *             the ending as the game would pick it today
 *   CALENDAR  today, the calendar's events and birthdays ahead, booked
 *             dates, and the rest of this week as the game scheduled it
 *
 * Every number comes from pc/hal/game_state_view.c (named PS1 globals, the
 * game's rules transcribed and tested against the game's functions).
 * Nothing here writes game memory or runs game code: the menu holds the
 * game thread in the presenter's pause loop while the page is open.
 *
 * TOKI_STATUS_LOG=PATH (tests): each time the page opens, the decoded state
 * is appended to PATH and the 2 MB PS1 RAM is written to PATH.N.ram.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "pc_menu.h"
#include "pc_ui.h"
#include "pc_settings.h"
#include "profile.h"
#include "game_state_view.h"

/* the game's font in RAM (pc/hal/backlog_capture.c; weak: harnesses) */
extern int backlog_font_glyph(u16 code, u8 bits[18]) __attribute__((weak));
extern const char *hal_profile_name(void) __attribute__((weak));

enum { TAB_GIRLS, TAB_GIRL, TAB_PLAYER, TAB_CALENDAR, TAB_COUNT };
static const char *const k_tabs[TAB_COUNT] = { "Girls", "Girl", "Player", "Calendar" };

/* colours: the settings menu's (pc/include/pc_ui.h) */
#define C_TEXT UI_C_TEXT
#define C_DIM UI_C_DIM
#define C_HEAD UI_C_HEAD
#define C_SEL UI_C_SEL
#define C_WARN UI_C_WARN
#define C_BAD UI_C_BAD
#define C_GOOD UI_C_GOOD
#define C_BAR UI_C_BAR
#define C_LIT UI_C_LIT

/* Layout, in canvas units (the page body, 284 x 168 at a game pixel each):
 * the tab bar, then the tab from TOP_Y.  Text is latin12, UI_ROW_TIGHT a
 * line; a girl's row holds her name in the game's 12x12 glyphs. */
#define ROW UI_ROW_TIGHT        /* a text line */
#define GROW 13                 /* a row with the game's 12x12 glyphs */
#define TOP_Y 15
/* the GIRLS table: right edges of the number columns, the meter */
#define COL_TOKI 98
#define COL_LIKE 130
#define COL_METER 136
#define COL_BOMB 184            /* the bomb number, left-aligned after the meter */
#define COL_DATE 222
#define COL_SCORE 268
#define COL_LEAD 272

static GsvState g_s;
static int g_tab, g_sel;        /* g_sel: index into the met list */
static int g_top;               /* GIRLS: the first row shown */
static int g_page = -1;
static int g_met[GSV_GIRLS], g_nmet;

/* ---- the switch ---- */

int status_enabled(void) {
    static int on = -1;
    if (on < 0) on = hal_feature_on("TOKI_STATUS");
    return on && (pc_settings_get()->status);
}

int status_page(void) { return status_enabled() ? g_page : -1; }

/* ---- drawing helpers ---- */

/* printf-style text at x; returns its width */
static int text(const PcMenuCanvas *c, int x, int y, unsigned col, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));
static int text(const PcMenuCanvas *c, int x, int y, unsigned col, const char *fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    c->text(c->ctx, x, y, col, buf);
    return ui_text_width(buf);
}

/* the same, ending at xr (right-aligned) */
static void rtext(const PcMenuCanvas *c, int xr, int y, unsigned col, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));
static void rtext(const PcMenuCanvas *c, int xr, int y, unsigned col, const char *fmt, ...) {
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    c->text(c->ctx, xr - ui_text_width(buf), y, col, buf);
}

/* text cut to w units (an ellipsis where it was cut) */
static void ftext(const PcMenuCanvas *c, int x, int y, unsigned col, int w, const char *s) {
    char buf[160];
    ui_text_fit(s, w, buf, sizeof buf);
    c->text(c->ctx, x, y, col, buf);
}

/* text word-wrapped to the canvas width, at most `max` lines; returns the
 * height used */
static int wtext(const PcMenuCanvas *c, int x, int y, unsigned col, int max, const char *s) {
    UiLine l[4];
    char buf[160];
    int n = ui_text_wrap(s, c->w - x, l, max < 4 ? max : 4), i;
    for (i = 0; i < n; i++) {
        snprintf(buf, sizeof buf, "%.*s", l[i].n, l[i].s);
        c->text(c->ctx, x, y + i * ROW, col, buf);
    }
    return n * ROW;
}

/* A Shift-JIS string in the game's font (12x12, 1 unit a pixel), with the
 * game's own black shadow one pixel down and right (disp_string draws each
 * cell twice); returns its width, or -1 when a glyph is not in the font
 * (nothing drawn). */
static void sjis_cell(const PcMenuCanvas *c, int x, int y, unsigned col, const u8 bits[18]) {
    int gx, gy;
    for (gy = 0; gy < 12; gy++)
        for (gx = 0; gx < 12; gx++) {
            int bit = gy * 12 + gx;
            if (bits[bit >> 3] & (0x80 >> (bit & 7))) {
                /* runs of lit pixels as one fill */
                int run = 1;
                while (gx + run < 12) {
                    int b2 = gy * 12 + gx + run;
                    if (!(bits[b2 >> 3] & (0x80 >> (b2 & 7)))) break;
                    run++;
                }
                c->fill(c->ctx, x + gx, y + gy, run, 1, col);
                gx += run - 1;
            }
        }
}

static int sjis(const PcMenuCanvas *c, int x, int y, unsigned col, const char *s, int max) {
    u8 bits[18];
    int n = 0, i, pass;
    const unsigned char *p = (const unsigned char *)s;
    if (!s || !backlog_font_glyph) return -1;
    /* every glyph first, so a missing one draws nothing */
    for (i = 0; p[i] && p[i + 1] && n < max; i += 2, n++)
        if (!backlog_font_glyph((u16)(p[i] << 8 | p[i + 1]), bits)) return -1;
    for (pass = 0; pass < 2; pass++)
        for (i = 0, n = 0; p[i] && p[i + 1] && n < max; i += 2, n++) {
            backlog_font_glyph((u16)(p[i] << 8 | p[i + 1]), bits);
            if (pass == 0) sjis_cell(c, x + n * 12 + 1, y + 1, UI_C_SHADOW, bits);
            else sjis_cell(c, x + n * 12, y, col, bits);
        }
    return n * 12;
}

/* the girl's name: the game's glyphs, else the romanised short name */
static void girl_name(const PcMenuCanvas *c, int x, int y, unsigned col, int g, int max) {
    if (sjis(c, x, y, col, gsv_girl_sjis(g), max) < 0)
        c->text(c->ctx, x, y, col, gsv_girl_short(g));
}

/* a bar of w units for v in 0..max */
static void bar(const PcMenuCanvas *c, int x, int y, int w, int h, int v, int max, unsigned col) {
    int f = max > 0 ? (v < 0 ? 0 : v > max ? max : v) * w / max : 0;
    c->fill(c->ctx, x - 1, y - 1, w + 2, h + 2, UI_C_RULE);
    c->fill(c->ctx, x, y, w, h, UI_C_TROUGH);
    if (f > 0) c->fill(c->ctx, x, y, f, h, col);
}

/* the bomb meter: ticks at the game's thresholds */
static void bomb_meter(const PcMenuCanvas *c, int x, int y, int w, int h, const GsvGirl *g) {
    static const int ticks[3] = { GSV_BOMB_ICON, GSV_BOMB_SCENE, GSV_BOMB_CALL };
    int i;
    unsigned col = g->bomb >= GSV_BOMB_CALL ? C_LIT : g->lit ? C_WARN : 0xD8C060FFu;
    bar(c, x, y, w, h, g->bomb, 128, col);
    for (i = 0; i < 3; i++) c->fill(c->ctx, x + ticks[i] * w / 128, y - 2, 1, h + 4, C_TEXT);
}

static unsigned score_col(int score) { return score >= GSV_SCORE_MIN ? C_GOOD : C_TEXT; }

/* ---- tabs ---- */

static void tab_bar(const PcMenuCanvas *c) {
    int i, x = 0;
    for (i = 0; i < TAB_COUNT; i++) {
        int w = ui_text_width(k_tabs[i]) + 10;
        if (i == g_tab) {
            c->fill(c->ctx, x, 0, w, UI_ROW, C_SEL);
            c->fill(c->ctx, x, UI_ROW, w, 1, C_HEAD);
        }
        c->text(c->ctx, x + 5, 0, i == g_tab ? C_HEAD : C_DIM, k_tabs[i]);
        x += w + 2;
    }
    rtext(c, c->w, 0, C_DIM, "Read only");
    c->fill(c->ctx, 0, UI_ROW + 1, c->w, 1, UI_C_RULE);
}

static void draw_girls(const PcMenuCanvas *c) {
    int i, y = TOP_Y, rows;
    rtext(c, COL_TOKI, y, C_DIM, "Toki");
    rtext(c, COL_LIKE, y, C_DIM, "Like");
    c->text(c->ctx, COL_METER, y, C_DIM, "Bomb");
    rtext(c, COL_DATE, y, C_DIM, "Date");
    rtext(c, COL_SCORE, y, C_DIM, "Score");
    y += ROW + 1;
    if (!g_nmet) {
        c->text(c->ctx, 2, y + 4, C_DIM, "Nobody met yet");
        return;
    }
    /* rows that fit; the legend only when every girl does */
    rows = (c->h - y) / GROW;
    if (g_nmet <= rows - 1) rows--;
    if (g_sel < g_top) g_top = g_sel;
    if (g_sel >= g_top + rows) g_top = g_sel - rows + 1;
    if (g_top > g_nmet - rows) g_top = g_nmet - rows;
    if (g_top < 0) g_top = 0;
    for (i = g_top; i < g_nmet && i < g_top + rows; i++) {
        const GsvGirl *g = &g_s.girl[g_met[i]];
        int lead = g_met[i] == g_s.leader;
        if (i == g_sel) c->fill(c->ctx, 0, y - 1, c->w, GROW, C_SEL);
        girl_name(c, 2, y, lead ? C_HEAD : C_TEXT, g_met[i], 5);
        rtext(c, COL_TOKI, y, C_TEXT, "%d%%", g->tokimeki_pct);
        rtext(c, COL_LIKE, y, g->neglect ? C_BAD : C_TEXT, "%d%%", g->yuukou_pct);
        bomb_meter(c, COL_METER + 1, y + 4, 42, 4, g);
        text(c, COL_BOMB, y, g->lit ? C_LIT : C_TEXT, "%d%s", g->bomb, g->lit ? "!" : "");
        rtext(c, COL_DATE, y, g->dates < 4 && g->index ? C_WARN : C_TEXT, "%d", g->dates);
        rtext(c, COL_SCORE, y, score_col(g->score), "%d", g->score);
        if (lead) c->text(c->ctx, COL_LEAD, y, C_HEAD, "\xE2\x97\x80");
        y += GROW;
    }
    if (g_nmet <= rows)
        ftext(c, 2, c->h - ROW, C_DIM, c->w - 2,
              "! Bomb lit (60+)   Score 500+ can win   \xE2\x97\x80 Leads");
}

static const char *place_or_spot(int spot) { return gsv_spot_name(spot); }

static void draw_girl(const PcMenuCanvas *c) {
    const GsvGirl *g;
    int y = TOP_Y, k, x;
    char line[128];
    if (!g_nmet) {
        c->text(c->ctx, 2, y + 4, C_DIM, "Nobody met yet");
        return;
    }
    g = &g_s.girl[g_met[g_sel]];
    girl_name(c, 2, y, C_HEAD, g->index, 6);
    c->text(c->ctx, 80, y, C_HEAD, gsv_girl_name(g->index));
    y += GROW + 1;
    text(c, 2, y, C_TEXT, "Tokimeki %d (%d%%)", g->tokimeki, g->tokimeki_pct);
    text(c, 120, y, C_TEXT, "Friendship %d (%d%%)", g->yuukou, g->yuukou_pct);
    y += ROW;
    text(c, 2, y, g->lit ? C_LIT : C_TEXT, "Bomb %d", g->bomb);
    bomb_meter(c, 62, y + 4, 128, 4, g);
    c->text(c->ctx, 198, y, C_DIM, "60 76 91");
    y += ROW;
    if (g->bomb >= GSV_BOMB_CALL)
        snprintf(line, sizeof line, "91+: Yoshio calls on a free term day (not 1998)");
    else if (g->bomb >= GSV_BOMB_SCENE)
        snprintf(line, sizeof line, "76+: she confronts you%s, 91 explodes",
                 g->latch_scene ? " (shown)" : "");
    else if (g->bomb >= GSV_BOMB_RUMOUR)
        snprintf(line, sizeof line, "61+: rumour screen%s, 76 scene, 91 explodes",
                 g->latch_rumour ? " (shown)" : "");
    else if (g->lit)
        snprintf(line, sizeof line, "Lit on Yoshio screen, 61 starts the rumour");
    else
        snprintf(line, sizeof line, "Safe below 60 (no bomb on Yoshio screen)");
    y += wtext(c, 2, y, g->lit ? C_WARN : C_DIM, 2, line);
    if (g->neglect)
        y += wtext(c, 2, y, C_BAD, 2, "Friendship under 10%: bomb +5 every term week");
    if (g->last_spot > 0) {
        text(c, 2, y, C_TEXT, "Last date: %s", place_or_spot(g->last_spot));
        if (g->spot_timer) {
            y += ROW;
            y += wtext(c, 2, y, C_WARN, 2, "Same spot again soon: Toki -1, Like -2, Bomb +5") - ROW;
        }
    } else {
        c->text(c->ctx, 2, y, C_DIM, "No date yet");
    }
    y += ROW;
    text(c, 2, y, C_TEXT, "Birthday %d/%d%s", g->birth_month, g->birth_day,
         g->index == 0 || g->birthday_known ? "" : "  (not told yet)");
    rtext(c, c->w, y, C_TEXT, "Icon %d", g->icon);
    y += ROW + 3;

    if (g->index == 0) {
        /* get_grp_point's checklist */
        static const int need_k[7] = { 1, 2, 3, 4, 5, 6, 7 };
        static const int need_v[7] = { 130, 130, 130, 130, 120, 100, 100 };
        text(c, 2, y, score_col(g->score), "Ending score %d: 785 when all hold", g->score);
        y += ROW;
        text(c, 2, y, g->icon >= 85 ? C_GOOD : C_BAD, "Icon %d/85", g->icon);
        text(c, 73, y, g->dates >= 8 ? C_GOOD : C_BAD, "Dates %d/8", g->dates);
        text(c, 144, y, g->bomb < 51 ? C_GOOD : C_BAD, "Bomb %d<51", g->bomb);
        text(c, 215, y, g_s.ill ? C_BAD : C_GOOD, "%s", g_s.ill ? "Ill" : "Healthy");
        y += ROW;
        for (k = 0, x = 2; k < 7; k++) {
            int v = g_s.stats[need_k[k]];
            text(c, x, y, v >= need_v[k] ? C_GOOD : C_BAD, "%s %d/%d", gsv_stat_short(need_k[k]),
                 v, need_v[k]);
            x += 71;
            if (k == 3) { x = 2; y += ROW; }
        }
        y += ROW;
    } else {
        int bonus = 0;
        static const signed char tbl3[15] = { -100, -75, -50, -25, 0, 10, 20, 30, 40, 50,
                                              60, 70, 80, 90, 100 };
        text(c, 2, y, score_col(g->score), "Ending score %d (500 to qualify)", g->score);
        y += ROW;
        snprintf(line, sizeof line, "Icon %d x 4 + %d dates x 10%s", g->icon, g->dates,
                 g->dates < 4 ? " (under 4: score 0)" : "");
        y += wtext(c, 2, y, g->dates >= 4 ? C_TEXT : C_WARN, 2, line);
        text(c, 2, y, C_TEXT, "Bomb %d x %d = %d", g->bomb, g->bomb_weight, g->bomb * g->bomb_weight);
        y += ROW;
        x = 2 + text(c, 2, y, C_DIM, "Likes:") + 6;
        for (k = 1; k <= 7; k++) {
            int v, b;
            if (!g->liked[k]) continue;
            v = g_s.stats[k] / 10;
            if (v > 14) v = 14;
            if (v < 0) v = 0;
            b = tbl3[v] * g->liked[k];
            bonus += b;
            if (x > c->w - 64) { x = 44; y += ROW; }
            x += text(c, x, y, b >= 0 ? C_GOOD : C_BAD, "%s%s %+d", gsv_stat_short(k),
                      g->liked[k] > 1 ? "x2" : "", b) + 10;
        }
        y += ROW;
        if (g->index == 2) {
            text(c, 2, y, C_TEXT, "Low stress pleases her (stress %d)", g_s.stats[8]);
            y += ROW;
        }
    }
    x = 2 + text(c, 2, y, C_DIM, "Tokimeki grows with:") + 6;
    for (k = 1; k <= 6; k++)
        if (g->ladder[k]) x += text(c, x, y, C_TEXT, "%s", gsv_stat_short(k)) + 6;
}

static void draw_player(const PcMenuCanvas *c) {
    int y = TOP_Y, k;
    /* the nine parameters in two columns: name, value, a bar */
    for (k = 0; k < GSV_STATS; k++) {
        int v = g_s.stats[k], max = k == 8 ? 140 : 250;
        int x = k < 5 ? 2 : 146, ry = y + (k < 5 ? k : k - 5) * ROW;
        c->text(c->ctx, x, ry, C_TEXT, gsv_stat_name(k));
        rtext(c, x + 92, ry, C_TEXT, "%d", v);
        bar(c, x + 97, ry + 4, 38, 4, v, max, k == 8 && v > 80 ? C_WARN : C_BAR);
    }
    y += 5 * ROW + 3;
    text(c, 2, y, C_TEXT, "Club: %s   This week: %s", gsv_club_name(g_s.club),
         gsv_activity_name(g_s.activity));
    y += ROW;
    text(c, 2, y, g_s.ill ? C_BAD : C_TEXT, "Health: %s   Birthday %d/%d",
         g_s.ill ? "Ill" : "Well", g_s.birth_month, g_s.birth_day);
    y += ROW + 3;
    c->text(c->ctx, 2, y, C_HEAD, "If the game ended today");
    y += ROW;
    if (g_s.ending <= 10)
        text(c, 2, y, C_GOOD, "%s (score %d)", gsv_girl_name(g_s.ending), g_s.leader_score);
    else if (g_s.ending == 12)
        c->text(c->ctx, 2, y, C_GOOD, "Rei Ijuin");
    else if (g_s.ending == 11)
        c->text(c->ctx, 2, y, C_WARN, "Maybe Miharu (a dice roll), else Yoshio");
    else
        c->text(c->ctx, 2, y, C_DIM, "Nobody: the Yoshio ending");
    y += ROW;
    {
        char line[128];
        snprintf(line, sizeof line, "Rei: %d/70 calls, kidnap answer %s   Miharu met %d",
                 g_s.rei_calls, g_s.rei_answer == 2 ? "Yes" : g_s.rei_answer == 1 ? "No" : "-",
                 g_s.miharu_count);
        wtext(c, 2, y, C_DIM, 2, line);
    }
}

static void date_text(char *out, int n, int month, int day, int ahead) {
    snprintf(out, (size_t)n, "%d/%d +%d", month, day, ahead);
}

static void draw_calendar(const PcMenuCanvas *c) {
    int y = TOP_Y, i, x2 = 146, y2, w1 = x2 - 8, w2 = c->w - x2;
    char d[24], line[96];
    static const char *const season[4] = { "Term", "Summer vacation", "Winter vacation",
                                           "Spring vacation" };
    snprintf(line, sizeof line, "Today 19%02d/%d/%d %s  Week %d  %s", g_s.year, g_s.month, g_s.day,
             gsv_youbi_name(g_s.youbi), g_s.week, g_s.season < 4 ? season[g_s.season] : "?");
    ftext(c, 2, y, C_HEAD, c->w - 2, line);
    y += ROW + 3;
    for (i = 0; i < g_s.nbooked; i++) {
        const GsvDate *b = &g_s.booked[i];
        snprintf(line, sizeof line, "Booked %d/%d: %s at %s", b->month, b->day,
                 gsv_girl_short(b->girl), gsv_place_name(b->place));
        ftext(c, 2, y, C_HEAD, c->w - 2, line);
        y += ROW;
    }
    c->text(c->ctx, 2, y, C_DIM, "Coming up");
    c->text(c->ctx, x2, y, C_DIM, "This week (game plan)");
    c->fill(c->ctx, x2 - 5, y, 1, c->h - y, UI_C_RULE);
    y += ROW;
    y2 = y;
    for (i = 0; i < g_s.nevents && y <= c->h - ROW; i++) {
        const GsvEvent *e = &g_s.events[i];
        unsigned col = C_TEXT;
        date_text(d, sizeof d, e->month, e->day, e->days_ahead);
        if (e->kind == GSV_EV_BIRTHDAY) {
            snprintf(line, sizeof line, "%s B-day", gsv_girl_short(e->girl));
            col = C_GOOD;
        } else if (e->kind == GSV_EV_DATE) {
            snprintf(line, sizeof line, "Date %s", gsv_girl_short(e->girl));
            col = C_HEAD;
        } else {
            snprintf(line, sizeof line, "%s", e->label);
            if (e->kind == GSV_EV_PLAYER_BIRTHDAY) col = C_GOOD;
        }
        c->text(c->ctx, 2, y, col, d);
        ftext(c, 62, y, col, w1 - 62, line);
        y += ROW;
    }
    if (!g_s.nevents) c->text(c->ctx, 2, y, C_DIM, "Nothing in 60 days");
    y = y2;
    {
        int last = -1;
        for (i = 0; i < g_s.nplan && y <= c->h - ROW; i++) {
            const GsvPlan *p = &g_s.plan[i];
            char what[48];
            if (p->type == 0)
                snprintf(what, sizeof what, "School, %s", gsv_activity_name(p->a));
            else if (p->type == 50 || p->type == 69 || p->type == 72)
                snprintf(what, sizeof what, "%s %s", p->label, gsv_girl_short(p->a));
            else
                snprintf(what, sizeof what, "%s", p->label);
            if (p->day != last)
                c->text(c->ctx, x2, y, p->day ? C_DIM : C_HEAD,
                        gsv_youbi_name((g_s.youbi + p->day) % 7));
            ftext(c, x2 + 26, y, p->day ? C_TEXT : C_HEAD, w2 - 26, what);
            last = p->day;
            y += ROW;
        }
        if (!g_s.nplan) c->text(c->ctx, x2, y, C_DIM, "Nothing scheduled");
    }
}

/* ---- the page ---- */

static void collect(void) {
    int i;
    gsv_read(&g_s);
    g_nmet = 0;
    for (i = 0; i < GSV_GIRLS; i++)
        if (g_s.girl[i].met) g_met[g_nmet++] = i;
    if (g_sel >= g_nmet) g_sel = g_nmet ? g_nmet - 1 : 0;
}

static void log_open(void) {
    static int count;
    const char *path = getenv("TOKI_STATUS_LOG");
    static char textbuf[16384];
    char ram[600];
    FILE *f;
    if (!path || !*path) return;
    gsv_format(&g_s, textbuf, sizeof textbuf);
    f = fopen(path, "a");
    if (f) {
        fprintf(f, "--- open %d\n%s", count, textbuf);
        fclose(f);
    }
    snprintf(ram, sizeof ram, "%s.%d.ram", path, count);
    f = fopen(ram, "wb");
    if (f) {
        fwrite((const void *)(unsigned long)0x80000000ul, 1, 0x200000ul, f);
        fclose(f);
    }
    count++;
}

static void enter(void *ctx) {
    (void)ctx;
    collect();
    printf("[STATUS] open (%s)\n", g_s.in_game ? "in game" : "no game");
    fflush(stdout);
    log_open();
}

static int input(void *ctx, int in) {
    (void)ctx;
    switch (in) {
    case PC_MENU_LEFT: g_tab = (g_tab + TAB_COUNT - 1) % TAB_COUNT; return 1;
    case PC_MENU_RIGHT: g_tab = (g_tab + 1) % TAB_COUNT; return 1;
    case PC_MENU_UP: if (g_nmet) g_sel = (g_sel + g_nmet - 1) % g_nmet; return 1;
    case PC_MENU_DOWN: if (g_nmet) g_sel = (g_sel + 1) % g_nmet; return 1;
    case PC_MENU_OK:
        if (g_tab == TAB_GIRLS && g_nmet) g_tab = TAB_GIRL;
        else if (g_tab == TAB_GIRL) g_tab = TAB_GIRLS;
        return 1;
    default:
        return 0;
    }
}

static void draw(void *ctx, const PcMenuCanvas *c) {
    (void)ctx;
    collect();                  /* read-only; the game is held */
    tab_bar(c);
    if (!g_s.in_game) {
        c->text(c->ctx, 2, TOP_Y + 4, C_DIM, "No game in progress");
        c->text(c->ctx, 2, TOP_Y + 4 + ROW, C_DIM, "Start or load a game first.");
        return;
    }
    switch (g_tab) {
    case TAB_GIRLS: draw_girls(c); break;
    case TAB_GIRL: draw_girl(c); break;
    case TAB_PLAYER: draw_player(c); break;
    default: draw_calendar(c); break;
    }
}

static const char *help(void *ctx, int line) {
    (void)ctx;
    if (line) return "Esc/Ctrl+I Close";
    return g_tab == TAB_GIRLS
        ? "\xE2\x86\x90\xE2\x86\x92 Tab   \xE2\x86\x91\xE2\x86\x93 Girl   Enter Details"
        : "\xE2\x86\x90\xE2\x86\x92 Tab   \xE2\x86\x91\xE2\x86\x93 Girl";
}

/* ---- the HUD line ---- */

/* [enhanced] status_hud, or TOKI_STATUS_HUD=1 (environment, read once:
 * automated runs use default settings) */
static int hud_on(void) {
    static int env = -1;
    if (env < 0) {
        const char *e = getenv("TOKI_STATUS_HUD");
        env = e && *e && e[0] != '0';
    }
    return env || pc_settings_get()->status_hud;
}

/* The line drawn over the game when [enhanced] status_hud is on: stress
 * and the met girl with the highest bomb.  Empty (0) when off or no game. */
int status_hud_text(char *out, int cap) {
    GsvState *s = &g_s;
    if (cap > 0) out[0] = 0;
    if (!status_enabled() || !hud_on()) return 0;
    if (!gsv_read(s)) return 0;
    if (s->most_bomb >= 0)
        snprintf(out, (size_t)cap, "Stress %d  Bomb: %s %d%s", s->stats[8],
                 gsv_girl_short(s->most_bomb), s->girl[s->most_bomb].bomb,
                 s->girl[s->most_bomb].lit ? "!" : "");
    else
        snprintf(out, (size_t)cap, "Stress %d  No bomb", s->stats[8]);
    return 1;
}

/* ---- registration ---- */

static int get_hud(void *p) { (void)p; return pc_settings_get()->status_hud; }
static void set_hud(void *p, int v) { (void)p; pc_settings_mut()->status_hud = v != 0; }
static const char *open_note(void *p) { (void)p; return "Ctrl+I"; }

void status_menu_register(void) {
    static int done;
    PcMenuCustom ops;
    PcMenuItem it;
    if (done || !status_enabled()) return;
    done = 1;
    memset(&ops, 0, sizeof ops);
    ops.enter = enter;
    ops.input = input;
    ops.draw = draw;
    ops.help = help;
    g_page = pc_menu_custom_page("Status at a glance", &ops);
    if (g_page < 0) return;
    memset(&it, 0, sizeof it);
    it.type = PC_MENU_HEADER;
    it.label = "Status";
    pc_menu_add(&it);
    memset(&it, 0, sizeof it);
    it.type = PC_MENU_PAGE;
    it.label = "Status at a glance";
    it.target = g_page;
    it.note = open_note;
    pc_menu_add(&it);
    memset(&it, 0, sizeof it);
    it.type = PC_MENU_TOGGLE;
    it.label = "Status HUD line";
    it.key = "enhanced.status_hud";
    it.get = get_hud;
    it.set = set_hud;
    if (pc_menu_add(&it) < 0) fprintf(stderr, "[STATUS] settings menu full: entries not added\n");
}
