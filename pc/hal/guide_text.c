/* SPDX-License-Identifier: GPL-3.0-or-later */
/* guide_text.c -- the EVENTS and RULES pages of the guide (guide_model.h).
 * Prose and rule statements are ours; the numbers are constants of the
 * game's code (named with their function in each "Source:" line) or
 * tables read at run time from the player's game (the calendar from
 * CALROM.NBN, the exam bonuses from ETC.EXN, the date multipliers from
 * DATE.EXN).  Evidence: docs/game-flows/MECHANICS-GUIDE.md (MG).
 */
#include <stdio.h>
#include <string.h>
#include "types.h"
#include "guide_model.h"

void guide_P(unsigned col, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void guide_B(unsigned col, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void guide_H(const char *t);
void guide_SRC(const char *t);
GuideLine *guide_ROW(unsigned col, const char *label, int n, const short *cx);
void guide_cell(GuideLine *l, int i, unsigned col, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
void guide_pts(char *out, int n, int v16);
const char *guide_stat_name(int k);
const char *guide_first_name(int g);
const char *guide_place_name(int p);

#define P guide_P
#define B guide_B
#define H guide_H
#define SRC guide_SRC
#define T GUIDE_C_TEXT
#define DIM GUIDE_C_DIM
#define HL GUIDE_C_HEAD
#define GOOD GUIDE_C_GOOD

static int live(const GsvState *s) { return s && s->in_game; }

static const char *const k_mon[13] = {
    "", "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
};

/* ---- the calendar ---- */

/* the event numbers of the calendar file (schedule_make's arms; MG 5.1) */
typedef struct {
    int ev;
    const char *name;
    const char *what;
} CalEvent;

static const CalEvent k_events[] = {
    { 2, "New school year", "1996: Yumi joins the school. 1997: the career talk. Club "
      "quitters may rejoin." },
    { 4, "Sports festival", "You run one race; first place raises every girl's tokimeki "
      "(+4 to +6), second +2 to +3." },
    { 5, "Exam week", "Mon-Fri exams, results on Saturday (Rules)." },
    { 6, "Summer vacation", "Every weekday is free; dates and calls are possible." },
    { 8, "Summer camp", "Club members: six days of camp (sessions +2 a day, +1.5 to the "
      "club's subject). Baseball with 8 wins: the national tournament instead." },
    { 10, "Second term", "School starts again." },
    { 11, "School trip", "The whole week; you choose Kyoto/Nara, Okinawa or Hokkaido." },
    { 12, "Culture festival", "Culture clubs (Literature..Brass band): the festival; "
      "sports clubs: a girl may invite you around." },
    { 13, "Christmas party", "At Rei's. Shiori and two girls come (friendship +4, "
      "tokimeki by your Looks); Shiori, Mira or Megumi at 80%/80%: a special evening." },
    { 14, "Winter vacation", "" },
    { 15, "New Year", "The met girl in the best mood visits the shrine with you if her "
      "friendship is 30% and tokimeki 50% or more. Fortune: great luck stress -5, great misfortune +5." },
    { 16, "Third term", "" },
    { 17, "Valentine's Day", "Every met girl with friendship 50%+ gives chocolate." },
    { 18, "Entrance exams", "University or job exam (1998)." },
    { 19, "White Day", "Answer one girl's chocolate: tokimeki +8, friendship +8, bomb "
      "-10; the other givers friendship +1." },
    { 20, "Spring vacation", "" },
    { 21, "Graduation", "The ending." },
};
#define N_EVENTS ((int)(sizeof k_events / sizeof k_events[0]))

static void calendar_section(const GsvState *s) {
    int e, k, d, holidays[4] = { 0, 0, 0, 0 }, nk, first, have = 1;
    unsigned char kind[31], ev[31];
    int next_k = -1, next_d = 0, next_ev = 0, today_k = -1;
    H("The school calendar");
    if (!guide_cal_month(0, &nk, &first, kind, ev)) {
        P(DIM, "(Read from your CALROM.NBN, which was not found.)");
        have = 0;
    }
    if (live(s)) today_k = (s->year - 95) * 12 + s->month - 4;
    for (e = 0; have && e < N_EVENTS; e++) {
        char dates[200];
        int len = 0, count = 0;
        dates[0] = 0;
        for (k = 0; k < 36; k++) {
            if (!guide_cal_month(k, &nk, &first, kind, ev)) break;
            for (d = 0; d < nk; d++) {
                if (ev[d] != k_events[e].ev) continue;
                /* the exam week is marked on its Monday; a vacation on its
                 * first day */
                if (count < 6 && len < (int)sizeof dates - 16)
                    len += snprintf(dates + len, sizeof dates - (size_t)len, "%s%d %s %d",
                                    count ? ", " : "", d + 1, k_mon[(k + 3) % 12 + 1],
                                    95 + (k + 3) / 12);
                count++;
                if (today_k >= 0 && (k > today_k || (k == today_k && d + 1 > s->day)) &&
                    (next_k < 0 || k < next_k || (k == next_k && d < next_d))) {
                    next_k = k;
                    next_d = d;
                    next_ev = e;
                }
            }
        }
        if (count > 6) snprintf(dates + len, sizeof dates - (size_t)len, " and %d more", count - 6);
        B(T, "%s: %s.%s%s", k_events[e].name, count ? dates : "none", k_events[e].what[0] ? " " : "",
          k_events[e].what);
    }
    for (k = 0; have && k < 36; k++) {
        if (!guide_cal_month(k, &nk, &first, kind, ev)) break;
        for (d = 0; d < nk; d++)
            if (kind[d] == 2) holidays[(k + 3) / 12]++;
    }
    if (have)
        P(T, "National holidays (no school, like a Sunday): %d in 1995, %d in 1996, %d in 1997, "
          "%d in 1998.", holidays[0], holidays[1], holidays[2], holidays[3]);
    if (have && next_k >= 0)
        P(HL, "Next: %s on %d %s %d.", k_events[next_ev].name, next_d + 1,
          k_mon[(next_k + 3) % 12 + 1], 95 + (next_k + 3) / 12);
    SRC("CALROM.NBN read from your disc (season_load_step 0x80072A58, cal_font_load "
        "0x800689FC); schedule_make (0x8006D890) turns each event into the day's scene. MG 5.1");
}

void guide_page_events(const GsvState *s) {
    int d;
    calendar_section(s);

    H("Exams");
    P(T, "Each exam day scores one subject from your parameters plus the day's biorhythm "
      "(0 to 16), minus 8:");
    B(T, "Mon Humanities. Tue Science. Wed 70%% Science + 30%% Trivia (at most 38). Thu 70%% "
      "Trivia + 30%% Humanities (at most 38). Fri 80%% Art + 20%% Trivia (at most 25).");
    B(T, "Halved with nerves or sickness. Early exams add a bonus (+14 in July 1995, down to "
      "0 by December 1997; partly random). A library date that month adds 0-9.");
    B(T, "Score = value x 100 / 128 (128+ is 97-100). Under 40 is a red mark; three red "
      "marks mean a remedial week (only study can be planned).");
    B(T, "Every girl whose total is below yours: tokimeki +11 the first time, +2 after; "
      "first place in school: +2 more for all. Parameters do not change.");
    if (live(s)) {
        static const char *const day[5] = { "Mon", "Tue", "Wed", "Thu", "Fri" };
        static const short cx[5] = { 120, 150, 180, 210, 240 };
        GuideLine *l;
        P(HL, "If the exam were today (no bonus):");
        l = guide_ROW(DIM, "", 5, cx);
        for (d = 0; d < 5; d++) guide_cell(l, d, DIM, "%s", day[d]);
        l = guide_ROW(T, "Score", 5, cx);
        for (d = 0; d < 5; d++) {
            int v = guide_exam_raw(d), sc = v < 128 ? v * 100 / 128 : 100;
            guide_cell(l, d, sc < 40 ? GUIDE_C_BAD : sc >= 80 ? GOOD : T, "%d", sc);
        }
    }
    SRC("score_grade_calc (ETC 0x80146D40), get_etc_hosei (ETC 0x801463F8, bonuses read "
        "from ETC.EXN), etc_week_rank_calc (ETC 0x801480BC), etc_week_result_disp (ETC "
        "0x80147D60). MG 5.2");

    H("Clubs and matches");
    B(T, "Sessions count up by 1 a club day (2 a camp day). A club girl can be introduced "
      "after 21 sessions in her club (Girls).");
    B(T, "Sports clubs: a practice match when you choose club on the 3rd Sunday of March, "
      "June, September or December: won with sessions >= 30 x (wins + 1); a win raises "
      "every girl's tokimeki +2, a loss gives Guts +10, stress +10.");
    B(T, "Baseball, national tournament (summer camp): each round won with sessions + "
      "Guts / 16 over 200, 210, 240, 260, 290 (1%% luck); rounds 3-5 raise every girl's "
      "tokimeki +15, +3, +4.");
    B(T, "3 Jan 1998: sports clubs with 10 wins play the final (won with 350 sessions: all "
      "girls +25, lost +18); culture clubs with 380 sessions get an award (+20).");
    SRC("week_day_main0 (0x8007470C), kangei_bukatu_para_set (KANGEI 0x80133A6C), RENSYU, "
        "MASTER, schedule_make. MG 5.3");

    H("Random encounters");
    B(T, "Each term school day has a 1-in-4 after-school slot (at most two a week). A slot "
      "first tries a girl's own event, then an invitation (once a week), then a walk home.");
    B(T, "A girl's own events need her friendship and tokimeki 50%%+ (and, it seems, a "
      "date with her: uncertain); a 5-in-16 roll, then about 3%% per event.");
    B(T, "Saturday: after six good days of study the girl who admires that subject may "
      "come by (Humanities Mio, Science Yuina, Art Ayako, Trivia Yuko, Looks Mira).");
    SRC("schedule_make, schedule_girls_gakko (0x80070B94), girls_event_pick (0x8006FBF0), "
        "get_param_top_girl (0x8006F5C8). MG 5.7");

    H("Bomb events (every day)");
    B(T, "A girl's bomb at 61+: the rumour screen, once until it falls under 50.");
    B(T, "76+: she confronts you on the way home (once until under 50; not on a Sunday, "
      "24 Dec or an event day).");
    B(T, "91+: Yoshio phones about the rumour and the bomb explodes: it goes to 0 and every "
      "met girl loses tokimeki and friendship (her Girls page has the amounts). Never in "
      "1998 or in vacations; one girl a day, the lowest number first.");
    SRC("schedule_day_main (0x800713EC), girls_param_clamp (0x800710F8), get_tel_ring_girl / "
        "tel_ring_timeout (TEL). MG 4");

    H("Rei's story");
    B(T, "21+ calls to her: Rei's first event (in winter). 41+: Yoshio's kidnapping news "
      "(once per game; the first answer keeps her ending open). 65+: her third event.");
    if (live(s)) P(HL, "Now: %d calls.", s->rei_calls);
    SRC("girls_event_pick (0x8006FBF0), geko_serifu_branch_step (GEKO 0x8013BF40). MG 5.4");

    H("Miharu");
    B(T, "Corridor meetings: 4%% per random-event slot (day 30+, 4 weeks apart, up to 5).");
    B(T, "On a date after two meetings she interrupts once in March-May and once at the "
      "medal corner in June-August; each raises her ending chance.");
    if (live(s))
        P(HL, "Now: %d meetings%s.", s->miharu_count,
          s->miharu_left ? ", she has said goodbye (no ending)" : "");
    SRC("girls_event_pick, date_event_sched (DATE 0x80132728). MG 5.5");

    H("The last weeks (1998)");
    B(T, "15 Feb: Miharu's farewell, only if two girls already score 501+ and she has 4+ "
      "meetings and both interruptions (31%%): her ending is then closed.");
    B(T, "22 Feb: Rei no longer takes calls.");
    B(T, "26 Feb: if six or more of Mio to Megumi are met, Yoshio takes the one ranked "
      "sixth by tokimeki + friendship to the tree: she is no longer met. Keep your girl "
      "in the top five, or meet fewer.");
    B(T, "28 Feb: Yuina's robot, if she is the strict top scorer with 500+.");
    B(T, "1 Mar: graduation. The ending is decided from that day's numbers.");
    SRC("final_year_event_check (0x8006EAE4), tel_ring0 (TEL 0x80134CD4), schedule_day_main, "
        "geko_ranking_init (GEKO 0x80142014), girl2_top_check (0x800711D0). MG 5.6");
}

/* ---- RULES ---- */

static void date_table(const GsvState *s) {
    static const short cx[6] = { 102, 132, 162, 202, 232, 262 };
    static const char *const ans[5] = { "Best", "Good", "Middle", "Poor", "Worst" };
    GuideLine *l;
    int imp, w;
    if (guide_date_cell(0, 0) < 0) return;
    P(HL, "With your Trivia (%d), points per answer:", s->stats[5]);
    l = guide_ROW(DIM, "", 6, cx);
    guide_cell(l, 0, DIM, "Tok");
    guide_cell(l, 1, DIM, "Fr");
    guide_cell(l, 2, DIM, "Bomb");
    guide_cell(l, 3, DIM, "Tok");
    guide_cell(l, 4, DIM, "Fr");
    guide_cell(l, 5, DIM, "Bomb");
    l = guide_ROW(DIM, "", 2, cx + 1);
    guide_cell(l, 0, GOOD, "loved place");
    guide_cell(l, 1, GUIDE_C_BAD, "disliked");
    l->cx[0] = 162;
    l->cx[1] = 262;
    for (imp = 0; imp < 5; imp++) {
        l = guide_ROW(T, ans[imp], 6, cx);
        for (w = 0; w < 6; w++) {
            char b[10];
            int v = guide_date_delta_cell(w < 3 ? 0 : 4, imp, w % 3);
            guide_pts(b, sizeof b, v);
            guide_cell(l, w, (w % 3 == 2) == (v > 0) && v ? GUIDE_C_BAD : v ? GOOD : DIM, "%s", b);
        }
    }
}

void guide_page_rules(const GsvState *s) {
    int lim = guide_ill_limit();

    H("The week");
    B(T, "Sunday night you plan the week. School days apply the activity; Sundays, "
      "holidays and vacation days open the holiday screen (a date, the phone, or the "
      "activity at x4).");
    B(T, "Some school days bring a first meeting, a girl's event or a walk home (Girls, "
      "Events).");
    SRC("schedule_make (0x8006D890), schedule_day_main (0x800713EC). MG 1");

    H("Friendship and tokimeki");
    B(T, "Friendship fades by 1 when a week is planned on the 1st-6th of a month (Shiori: "
      "also the 14th-20th). Tokimeki never fades. Only in term time.");
    B(T, "A met girl under 10%% friendship gets +5 bomb at every term planning (neglect).");
    B(T, "Friendship, tokimeki and bomb stay within 0-128 (checked every day).");
    SRC("schdeule_girls_param (0x80070F50), girl_counter_periodic_decrement (0x80070D04), "
        "syoushin_up (0x80070ED0), girls_param_clamp (0x800710F8). MG 3.1");

    H("Her mood");
    P(T, "The game sorts each girl by friendship%% (F) and tokimeki%% (T): F under 10: "
      "hostile. F under 50: cool. T under 50: friendly. T under 80: fond. Else: in love.");
    B(T, "Hostile, or a bomb of 60+: she will not walk home with you or ask you out, and "
      "refuses your call.");
    SRC("get_g_zyotai_h (0x80051F68), get_g_zyotai_s (0x80051E88), geko_judge (0x80070754), "
        "date_sasoi_judge (0x80070910), tel_greet1 (TEL 0x80134F18). MG 6.1");

    H("Conversation answers");
    P(T, "Most choices (walks home, birthdays, trips, festivals) use one scale: best answer "
      "tokimeki +3, friendship +2, bomb -15; middle +2, +1, -10; worst 0, +1, bomb +10; "
      "refusing friendship -1, bomb +5.");
    SRC("date_serifu_param_add (DATE 0x80149F70), geko_serifu_para_set (GEKO 0x80137648) "
        "and siblings. MG 6.2");

    H("Asking her out by phone");
    B(T, "From the holiday screen: one call a day. You can book a Sunday or holiday in the "
      "next 28 days, one date a day, eight at most; never 24 Dec, 1 Jan or after "
      "February 1998.");
    B(T, "Her yes: a chance of (the higher of icon / 4 and Trivia / 4) + her liking of the "
      "place, in 256 (each girl's page lists the places and your chance now).");
    B(T, "Every call that gets that far: bomb -12, friendship +0..3. A no: stress +9, "
      "Guts +0..7.");
    SRC("tel_ring0 (TEL 0x80134CD4), tel_cal_menu_set (TEL 0x80136458), reserved_date_check, "
        "tel_dialog20 (TEL 0x80136620). MG 6.3");

    H("On a date");
    P(T, "Each answer is graded best (0) to worst (4). Good answers gain more with high "
      "Trivia at a place she loves; bad answers cost less with high Trivia at a place "
      "she loves.");
    if (live(s)) date_table(s);
    B(T, "Each ordinary date adds 1 to her date count (the ending needs 4, Shiori 8).");
    B(T, "The same place as your last date with her (within 12 weeks): tokimeki -1, "
      "friendship -2, bomb +5.");
    B(T, "Looks under 30 (Mira: 35): tokimeki -1, friendship -1, bomb +5.");
    B(T, "Ending a booked date at the meeting place: bomb -40. Forgetting one: bomb up "
      "and friendship down by her amount (Girls).");
    SRC("date_score_apply (DATE 0x8013C12C), date_rank_grade_get (DATE 0x8013C3AC, "
        "D_8015B780, D_8015B76C read from DATE.EXN), date_ans_serifu_run (DATE "
        "0x8013B434), date_para_you_check (DATE 0x8013F4D8), date_exit (DATE 0x8013CD8C), "
        "tel_girl_adjust (TEL 0x80139D6C). MG 6.3");

    H("She asks you");
    B(T, "In an after-school slot: a met girl with icon 33+, bomb under 60 and not "
      "hostile asks you out with 8%% (one invitation a week); not with a date booked, not "
      "on the 8th-14th while you are in a club, not on 17 or 24 Dec, not from February 1998.");
    B(T, "Otherwise a girl with icon 17+ walks home with you with 50%%.");
    if (live(s)) {
        int g, n = 0;
        char who[200];
        int len = 0;
        who[0] = 0;
        for (g = 0; g < GSV_GIRLS; g++)
            if (guide_invite_ok(0, g, s->girl[g].icon)) {
                len += snprintf(who + len, sizeof who - (size_t)len, "%s%s", n ? ", " : "",
                                guide_first_name(g));
                n++;
            }
        P(HL, "Now able to ask you out: %s%s.", n ? who : "nobody",
          guide_invite_day_ok() ? "" : " (but not today)");
    }
    SRC("schedule_make, schedule_girls_gakko (0x80070B94), date_sasoi_judge (0x80070910), "
        "geko_judge (0x80070754). MG 6.5");

    H("Places");
    B(T, "Open from the start: neighbourhood park, shopping street, zoo, library, game "
      "centre, amusement park, concert hall.");
    B(T, "Later (earlier if you read the magazine in time; Yoshio tells you): central "
      "park Aug 95, art museum Nov 95, aquarium Feb 96, botanical garden Aug 96, bowling "
      "Nov 96, planetarium Feb 97, karaoke May 97; the stadium only through the "
      "magazine (spring 96); the cinema from June 95.");
    B(T, "Pool and beach only in the summer vacation, skating rink and ski slope only in "
      "the winter vacation, the shrine on 1-7 August.");
    SRC("parameter_init (0x80041BBC), basyo_flag_set (0x8006D1D4), tel_spot_menu_set (TEL "
        "0x8013D758), get_vacation. MG 6.6");

    H("Illness");
    P(T, "Checked every Saturday after school. Stamina under 20: an injury when a random "
      "0-255 is above Guts / 8 + 204 (at most 254); at stamina 0 always. Stress over 80: "
      "nerves with the same odds (drawn from a frame counter). Both: sickness.");
    if (live(s))
        P(HL, "Now: the limit is %d, so each risk is %d in 256.", lim, 255 - lim);
    B(T, "Injury heals with stamina over 30, nerves with stress under 70, sickness with "
      "both; then about two weeks without illness.");
    B(T, "Ill: gains are halved (Activities); sick: only Rest can be planned, no walks "
      "home, Shiori's ending needs you healthy.");
    B(T, "Sick on a school day: a met girl with tokimeki 51%%+ may visit (63 in 256 per "
      "girl, one girl).");
    SRC("byouki_hassei (0x80072E30), byouki_kega_kaifuku, byouki_noiroze_kaifuku, "
        "byouki_main7, parameter_up_down, get_grp_point. MG 1.4");

    H("The ending");
    B(T, "Rei first: 70+ calls, the kidnapping accepted and Shiori not at 785.");
    B(T, "Then the met girl with the highest score of 500 or more (ties: the lower "
      "number, Shiori first).");
    B(T, "Nobody: Miharu's dice roll if you met her and she did not leave; else Yoshio.");
    SRC("ending_select_init (ENDING 0x801330F4). MG 3.3");
}
