/* SPDX-License-Identifier: GPL-3.0-or-later */
/* guide_harness.c -- the guide's rules (pc/hal/guide_rules.c, guide_model.c)
 * against the game's own functions (pc/tools/test_guide.py builds and runs
 * it).
 *
 * Linked like the port: the PS1 globals resolve at their console addresses
 * (-Wl,--just-symbols=pc/ld/psx_globals.syms) in a 2 MB block mapped at
 * 0x80000000 and seeded with the retail executable's data (the tables), and
 * the MATCHED C of the functions the guide describes is compiled in.  rand()
 * is this file's, so a check can enumerate every value the game's rand()
 * can return (0..32767) and count outcomes exactly.
 *
 *   act [EXE]              parameter_up_down: every activity / club x
 *                          holiday x poor x illness, nine deltas each
 *   poor N SEED [EXE]      biorhythm_sf_set over all 32768 rand() values on N
 *                          random states x 7 day offsets: poor-day counts
 *   debut N SEED [EXE]     touzyou_check / _check2 / touzyou_basyo_check on N
 *                          random states: the girl and variant picked, and
 *                          the dice
 *   ill N SEED [EXE]       byouki_hassei over all 256 x 256 draws from each
 *                          state on N random states: the next state, the
 *                          visit check
 *   girls N SEED [EXE]     girl_counter_periodic_decrement, syoushin_up,
 *                          get_g_zyotai_h/_s, get_h_tokimeki on N states
 *   invite N SEED [EXE]    date_sasoi_judge / geko_judge: the girl picked and
 *                          the roll
 *   tel N SEED [EXE]       tel_dialog20's answer (TEL.EXN at 0x80132000) over
 *                          all 256 draws, every girl and place: yes count,
 *                          the bomb and stress it applies
 *   date N SEED [EXE]      date_score_apply (DATE.EXN at 0x80132000): every
 *                          girl, place and answer grade
 *   cal [EXE]              the guide's reading of CALROM.NBN, every month
 *   calram RAM [EXE]       the months the game loaded into cal_day_kind (a
 *                          RAM image) against the guide's reading
 *   exam N SEED [EXE]      score_grade_calc + get_etc_hosei (ETC.EXN): every
 *                          weekday and exam, sampled rand() values
 *   text RAM|- [EXE]       every page of the guide as text (RAM: a 2 MB
 *                          image; -: no game in progress)
 *   page RAM [EXE]         the page (guide_view.c) registered, opened and
 *                          drawn tab by tab into a recording canvas
 * Each check prints "<name> mismatches K of M" and exits 1 on any.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "types.h"
#include "../hal/game_state_view.h"
#include "../hal/guide_model.h"
#include "pc_menu.h"
#include "pc_ui.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#define RAM_BASE 0x80000000ul
#define RAM_SIZE 0x200000ul

/* the game's functions (src/main/matched) */
extern void parameter_up_down(s32 a0, s32 a1, s32 a2);
extern s32 biorhythm_sf_set(s32 arg0);
extern s32 touzyou_check(u8 *sel, u8 *sub);
extern s32 touzyou_check2(s8 *sel, s8 *sub);
extern s32 touzyou_basyo_check(u8 *sel, u8 *sub);
extern void byouki_hassei(void);
extern void girl_counter_periodic_decrement(void);
extern void syoushin_up(void);
extern s32 get_g_zyotai_h(s32 g);
extern s32 get_g_zyotai_s(s32 g);
extern s32 get_h_tokimeki(s32 who);
extern u8 get_h_yuukou(s32 who);

/* guide_rules.c internals used for the checks */
int guide_met_1_9(void);

extern u8 screen_w[];
extern u8 girl_tbl[];
extern s16 para_tai;
extern u8 byouki_kind, byouki_state;
extern u16 basyo_pack;
extern u8 sched_icon;
extern u16 pass_days;
extern u32 p_birthday;
extern u8 bukatu_rec[];
extern u32 album_work;
extern u32 frame_counter;
extern u8 now_day;
extern u8 cal_day_kind[];

/* the guide page (guide_view.c) */
extern void guide_menu_register(void);
extern int guide_page(void);
extern void guide_test_enter(int tab, int girl);

/* ---- the game's environment ---- */

static int g_rand_mode, g_rand_next;        /* 0: the sequence below, 1: a value */
static int g_rand_value;
int rand(void) {
    if (g_rand_mode) return g_rand_value;
    return g_rand_next++ & 0x7FFF;
}
static int g_goto_ssub = -1, g_goto_sub = -1, g_goto_step = -1;
void goto_sub_sub_step(s32 n) { g_goto_ssub = n; }
void goto_sub_step(s32 n) { g_goto_sub = n; }
void goto_step(s32 n) { g_goto_step = n; }

static unsigned long g_rng;
static unsigned rnd(unsigned n) {
    g_rng = g_rng * 6364136223846793005ul + 1442695040888963407ul;
    return n ? (unsigned)(g_rng >> 33) % n : 0;
}

static int map_ram(const char *exe) {
    unsigned char hdr[0x800];
    FILE *f;
    void *p = mmap((void *)RAM_BASE, RAM_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (void *)RAM_BASE) {
        fprintf(stderr, "map 0x80000000: %s\n", strerror(errno));
        return -1;
    }
    f = fopen(exe, "rb");
    if (!f || fread(hdr, 1, sizeof hdr, f) != sizeof hdr || memcmp(hdr, "PS-X EXE", 8)) {
        fprintf(stderr, "%s: not a PS-X EXE\n", exe);
        if (f) fclose(f);
        return -1;
    }
    {
        u32 load = hdr[0x18] | hdr[0x19] << 8 | hdr[0x1A] << 16 | (u32)hdr[0x1B] << 24;
        u32 size = hdr[0x1C] | hdr[0x1D] << 8 | hdr[0x1E] << 16 | (u32)hdr[0x1F] << 24;
        if (load < RAM_BASE || load + size > RAM_BASE + RAM_SIZE ||
            fread((void *)(unsigned long)load, 1, size, f) != size) {
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    return 0;
}

static int load_image(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f || fread((void *)RAM_BASE, 1, RAM_SIZE, f) != RAM_SIZE) {
        fprintf(stderr, "%s: not a 2 MB RAM image\n", path);
        if (f) fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

#define PARA(k) (*(s32 *)(screen_w + 252 + 4 * (k)))
#define REC(g) (girl_tbl + 56 * (g))

static void set_stat(int k, int v) { PARA(k) = v << 16 | (int)rnd(65536); }

/* a random player and girls: parameters around the guide's thresholds */
static void randomise(void) {
    int g, k;
    static const int near[] = { 31, 36, 50, 55, 56, 60, 65, 71, 75, 76, 81, 84, 85, 91, 96, 101, 126 };
    static const int stress_near[] = { 31, 32, 63, 64, 95, 96, 97, 80, 81, 69, 70 };
    for (k = 0; k < 9; k++) {
        int v = (int)rnd(k == 8 ? 140 : 400);
        if (rnd(3) == 0) v = near[rnd(sizeof near / sizeof near[0])] - 1 + (int)rnd(2);
        if (k == 8 && rnd(3) == 0) v = stress_near[rnd(sizeof stress_near / sizeof stress_near[0])];
        if (rnd(50) == 0) v = 0;
        set_stat(k, v);
    }
    for (g = 0; g < 15; g++) {
        u8 *r = REC(g);
        *(s16 *)(r + 2) = (s16)rnd(129);
        *(s16 *)(r + 6) = (s16)((int)rnd(140) - 5);
        *(s16 *)(r + 10) = (s16)rnd(129);
        *(u32 *)(r + 12) = (rnd(0x7FFFFFFF) & ~0x00F00002u) | (rnd(3) ? 0u : 2u) |
                           (u32)rnd(12) << 20;
    }
    basyo_pack = (u16)(rnd(12) << 12 | rnd(4096));
    sched_icon = (u8)(rnd(10) << 4 | rnd(rnd(8) ? 8 : 16));
    for (k = 0; k < 16; k++) *(u16 *)(bukatu_rec + 4 * k) = (u16)rnd(rnd(2) ? 30 : 300);
    pass_days = (u16)rnd(1100);
    p_birthday = (u32)(rnd(13) | rnd(32) << 4 | rnd(4) << 9);
    byouki_kind &= 0xF0;        /* the club nibble (basyo_pack >> 12); healthy */
    byouki_state = (u8)(rnd(4) ? 0 : rnd(256));
    frame_counter = 0;
}

/* ---- checks ---- */

static int check_act(void) {
    int act, club, hol, poor, ill, k, bad = 0, n = 0;
    for (act = 0; act < 8; act++)
        for (club = 0; club < (act == 4 ? 12 : 1); club++)
            for (hol = 0; hol < 2; hol++)
                for (poor = 0; poor < 2; poor++)
                    for (ill = 0; ill < 5; ill++) {
                        s32 before[9];
                        /* basyo_pack's high byte IS byouki_kind: club in
                         * bits 4..7, illness in 0..3 */
                        basyo_pack = (u16)(club << 12);
                        byouki_kind = (u8)(club << 4 | ill);
                        for (k = 0; k < 9; k++) {
                            PARA(k) = 500 << 16 | 0x1234;
                            before[k] = PARA(k);
                        }
                        parameter_up_down(act, hol, poor);
                        for (k = 0; k < 9; k++) {
                            int game = PARA(k) - before[k];
                            int mine = guide_act_delta(act, club, k, hol, poor, ill);
                            n++;
                            if (game != mine) {
                                if (bad++ < 5)
                                    printf("act %d club %d hol %d poor %d ill %d stat %d: game %d guide %d\n",
                                           act, club, hol, poor, ill, k, game, mine);
                            }
                        }
                    }
    printf("act mismatches %d of %d\n", bad, n);
    return bad;
}

static int check_poor(int states) {
    int i, d, bad = 0, n = 0, poorest = 32768, richest = 0;
    for (i = 0; i < states; i++) {
        randomise();
        for (d = 0; d < 7; d++) {
            int game = 0, r, mine;
            u16 pd = pass_days;
            g_rand_mode = 0;
            g_rand_next = 0;
            for (r = 0; r < 32768; r++) game += biorhythm_sf_set(d) == 1;
            if (pass_days != pd) { printf("pass_days not restored\n"); bad++; }
            mine = guide_poor_count(d);
            n++;
            if (game < poorest) poorest = game;
            if (game > richest) richest = game;
            if (game != mine && bad++ < 5)
                printf("state %d day +%d act %d: game %d guide %d\n", i, d, sched_icon & 15, game, mine);
        }
    }
    printf("poor counts from %d to %d of 32768\n", poorest, richest);
    printf("poor mismatches %d of %d\n", bad, n);
    return bad;
}

static int check_debut(int states) {
    int i, mode, bad = 0, n = 0, picks[11] = { 0 }, g;
    for (i = 0; i < states; i++) {
        randomise();
        for (mode = 0; mode < 3; mode++) {
            u8 sel = 0, sub = 0;
            int r, mine, msub, pass = 0;
            g_rand_mode = 1;
            g_rand_value = 0;           /* the die passes */
            if (mode == 0) r = touzyou_check(&sel, &sub);
            else if (mode == 1) r = touzyou_check2((s8 *)&sel, (s8 *)&sub);
            else r = touzyou_basyo_check(&sel, &sub);
            mine = guide_debut_pick(mode, &msub);
            n++;
            if ((r ? sel : 0) != mine || (r && sub != msub)) {
                if (bad++ < 8)
                    printf("state %d mode %d: game %d/%d guide %d/%d (act %d)\n", i, mode,
                           r ? sel : 0, sub, mine, msub, sched_icon & 15);
            }
            if (mine) picks[mine]++;
            /* the die: how many rand() values let the check through */
            if (mine && i < 40) {
                for (g_rand_value = 0; g_rand_value < 32768; g_rand_value++) {
                    sel = 0;
                    if (mode == 0) pass += touzyou_check(&sel, &sub) != 0;
                    else if (mode == 1) pass += touzyou_check2((s8 *)&sel, (s8 *)&sub) != 0;
                    else pass += touzyou_basyo_check(&sel, &sub) != 0;
                }
                n++;
                /* guide: die24 / 24 of the values, to 0.1 % */
                if (abs(pass * 24 - guide_debut_die24(mode) * 32768) > 32768 * 24 / 1000 &&
                    bad++ < 8)
                    printf("mode %d die: game %d of 32768, guide %d/24\n", mode, pass,
                           guide_debut_die24(mode));
            }
        }
    }
    printf("debut picks:");
    for (g = 1; g < 11; g++) printf(" %d", picks[g]);
    printf("\ndebut mismatches %d of %d\n", bad, n);
    return bad;
}

static int check_ill(int states) {
    int i, st, a, b, bad = 0, n = 0, seen[5] = { 0 };
    for (i = 0; i < states; i++) {
        randomise();
        /* around the limits: stamina 0..40, stress 60..100 */
        {
            static const int sta[] = { 0, 19, 20, 29, 30, 31 }, str[] = { 69, 70, 80, 81 };
            set_stat(0, rnd(2) ? (int)rnd(41) : sta[rnd(6)]);
            set_stat(8, rnd(2) ? 60 + (int)rnd(41) : str[rnd(4)]);
        }
        for (st = 0; st < 4; st++)
            for (a = 0; a < 256; a += 3)
                for (b = 0; b < 256; b += 3) {
                    int game, mine;
                    byouki_kind = (u8)(0x30 | st);
                    g_rand_mode = 1;
                    g_rand_value = a | (int)rnd(128) << 8;
                    album_work = (album_work & ~0xFFu) | (u32)b;
                    g_goto_ssub = g_goto_sub = g_goto_step = -1;
                    now_day = 1 + (u8)rnd(28);
                    cal_day_kind[now_day] = 1;  /* not a school day: no visit */
                    byouki_hassei();
                    /* the game's verdict: sub_sub step 1/2/3 onset, 5/6/7
                     * recovery, none = stays */
                    switch (g_goto_ssub) {
                    case 1: game = 1; break;
                    case 2: game = 2; break;
                    case 3: game = 3; break;
                    case 5: case 6: case 7: game = 0; break;
                    default: game = st == 0 ? 0 : st; break;
                    }
                    if (st == 1 && g_goto_ssub == 3) game = 3;
                    mine = guide_ill_next(st, a, b);
                    n++;
                    seen[game]++;
                    if (game != mine && bad++ < 8)
                        printf("state %d tai %d str %d kon %d a %d b %d: game %d guide %d\n", st,
                               para_tai, *(s16 *)(screen_w + 254 + 32), *(s16 *)(screen_w + 254 + 28),
                               a, b, game, mine);
                }
    }
    /* the visit: state 3 staying, a school day: a met girl with tokimeki
     * 51 %+ and (rand() & 0xFF) < 63 */
    for (i = 0; i < states; i++) {
        int game_visits = 0;
        randomise();
        set_stat(0, 10);
        set_stat(8, 90);
        for (a = 0; a < 256; a++) {
            byouki_kind = 0x33;
            g_rand_mode = 1;
            g_rand_value = a;           /* k = a % 11 and the visit roll alike */
            now_day = 3;
            cal_day_kind[3] = 0;
            g_goto_step = -1;
            byouki_hassei();
            game_visits += g_goto_step == 98;
        }
        {
            int g, any = 0, mine = 0;
            for (g = 0; g < 11; g++)
                if ((*(u32 *)(REC(g) + 12) >> 1 & 1) && (u32)get_h_tokimeki(g) >= 51) any = 1;
            /* a == the roll: < 63 passes for any eligible girl */
            mine = any ? 63 : 0;
            n++;
            if (game_visits != mine && bad++ < 8)
                printf("visit: game %d of 256, guide %d\n", game_visits, mine);
        }
    }
    printf("ill states seen: %d %d %d %d\n", seen[0], seen[1], seen[2], seen[3]);
    printf("ill mismatches %d of %d\n", bad, n);
    return bad;
}

static int check_girls(int states) {
    int i, g, bad = 0, n = 0;
    for (i = 0; i < states; i++) {
        s16 before[11], bomb[11];
        int day;
        randomise();
        day = 1 + (int)rnd(31);
        screen_w[64] = (u8)day;
        for (g = 0; g < 11; g++) before[g] = *(s16 *)(REC(g) + 6);
        {
            int mine[11];
            for (g = 0; g < 11; g++) mine[g] = guide_decay(g, day);
            girl_counter_periodic_decrement();
            for (g = 0; g < 11; g++) {
                n++;
                if (before[g] - *(s16 *)(REC(g) + 6) != mine[g] && bad++ < 8)
                    printf("decay girl %d day %d: game %d guide %d\n", g, day,
                           before[g] - *(s16 *)(REC(g) + 6), mine[g]);
            }
        }
        {
            int mine[11];
            for (g = 0; g < 11; g++) { mine[g] = guide_neglect(g); bomb[g] = *(s16 *)(REC(g) + 10); }
            syoushin_up();
            for (g = 0; g < 11; g++) {
                n++;
                if (*(s16 *)(REC(g) + 10) - bomb[g] != mine[g] && bad++ < 8)
                    printf("neglect girl %d: game %d guide %d\n", g,
                           *(s16 *)(REC(g) + 10) - bomb[g], mine[g]);
            }
        }
        for (g = 0; g < 11; g++) {
            int y = gsv_yuukou_pct(g), t = gsv_tokimeki_pct(g), stats[9], k;
            for (k = 0; k < 9; k++) stats[k] = *(s16 *)(screen_w + 254 + 4 * k);
            n += 3;
            if (get_g_zyotai_h(g) != guide_mood_h(y, t) && bad++ < 8)
                printf("mood_h girl %d: game %d guide %d\n", g, get_g_zyotai_h(g), guide_mood_h(y, t));
            if (get_g_zyotai_s(g) != guide_mood_s(y, t) && bad++ < 8)
                printf("mood_s girl %d: game %d guide %d\n", g, get_g_zyotai_s(g), guide_mood_s(y, t));
            if ((u8)get_h_tokimeki(g) != guide_tokimeki_pct(g, stats, *(s16 *)(REC(g) + 2)) &&
                bad++ < 8)
                printf("tokimeki girl %d: game %d guide %d\n", g, (u8)get_h_tokimeki(g),
                       guide_tokimeki_pct(g, stats, *(s16 *)(REC(g) + 2)));
        }
    }
    printf("girls mismatches %d of %d\n", bad, n);
    return bad;
}

/* ---- phone, invitations, dates ---- */

extern s32 date_sasoi_judge(u8 *sel);
extern s32 geko_judge(s8 *sel);
extern void tel_dialog20(void);
extern void date_score_apply(void);
extern s8 date_sasoi_judge_tbl0;
extern u8 now_year, now_month;
extern u32 reserved_date[8];
extern u8 step_mode;
extern u8 now_girl;
extern s32 tel_sasoi_basyo;
extern u8 now_basyo;
extern s32 serifu_no;
extern s16 imp[];
extern s16 para_zat, para_you;

/* the overlay's file at 0x80132000, as the CD drive delivers it */
static int map_overlay(const char *name) {
    char path[300];
    FILE *f;
    long n;
    snprintf(path, sizeof path, "pc/assets/raw/CDROM/EXEDIR/%s.EXN", name);
    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "%s: missing\n", path);
        return -1;
    }
    n = (long)fread((void *)0x80132000ul, 1, 0x80200000ul - 0x80132000ul, f);
    fclose(f);
    return n > 0 ? 0 : -1;
}

int guide_quiet_printf(const char *fmt, ...) { (void)fmt; return 0; }

/* get_grp calls the head symbol of grp_table_calc (the port's
 * pc/src/missing_funcs_gen.c forwards it the same way) */
extern void grp_table_calc();
void func_80053364(u8 *out, s16 *clr) { grp_table_calc(out, clr); }

static int check_invite(int states) {
    int i, kind, bad = 0, n = 0, picked[2] = { 0, 0 };
    for (i = 0; i < states; i++) {
        unsigned char icon[11];
        short score[11];
        randomise();
        album_work = (u32)rnd(0x7FFFFFFF);
        date_sasoi_judge_tbl0 = (s8)((int)rnd(12) - 1);
        now_year = (u8)(95 + rnd(4));
        now_month = (u8)(rnd(4) ? 1 + rnd(12) : 12);         /* the December gates */
        now_day = (u8)(rnd(4) ? 1 + rnd(31) : rnd(2) ? 17 : 24);
        reserved_date[0] = rnd(4) ? 0 : 1u << 29;
        gsv_grp(icon, score);
        for (kind = 0; kind < 2; kind++) {
            s8 last = date_sasoi_judge_tbl0;
            int r, mine, pass;
            u8 sel = 0;
            /* every roll passes: the scan's first eligible girl */
            g_rand_mode = 1;
            g_rand_value = 0;
            mine = guide_invite_pick(kind, icon);
            r = kind ? geko_judge((s8 *)&sel) : date_sasoi_judge(&sel);
            date_sasoi_judge_tbl0 = last;
            n++;
            if ((r ? sel : -1) != mine && bad++ < 8)
                printf("state %d kind %d: game %d guide %d\n", i, kind, r ? sel : -1, mine);
            if (mine >= 0) picked[kind]++;
            /* the roll: rand() % 100 below 8 / 50 lets a girl through */
            if (mine >= 0) {
                for (pass = 0, g_rand_value = 0; g_rand_value < 100; g_rand_value++) {
                    pass += (kind ? geko_judge((s8 *)&sel) : date_sasoi_judge(&sel)) != 0;
                    date_sasoi_judge_tbl0 = last;
                }
                n++;
                if (pass != guide_invite_pct(kind) && bad++ < 8)
                    printf("kind %d roll: game %d of 100, guide %d\n", kind, pass,
                           guide_invite_pct(kind));
            }
        }
    }
    printf("invitations picked %d, walks %d\n", picked[0], picked[1]);
    printf("invite mismatches %d of %d\n", bad, n);
    return bad;
}

#define SAVE_LO 0x800EE760ul
#define SAVE_N 0x1800ul

static int check_tel(int states) {
    static u8 save[SAVE_N];
    int i, g, p, r, bad = 0, n = 0, yes_total = 0;
    if (map_overlay("TEL")) return 1;
    for (i = 0; i < states; i++) {
        unsigned char icon[11];
        short score[11];
        randomise();
        set_stat(5, rnd(2) ? (int)rnd(500) : (int)rnd(80));   /* trivia; low: the icon counts */
        set_stat(6, rnd(3) ? (int)rnd(200) : (int)rnd(40));   /* looks, often < 40 */
        gsv_grp(icon, score);
        memcpy(save, (void *)SAVE_LO, SAVE_N);
        for (g = 0; g < 11; g++)
            for (p = 0; p < 21; p++) {
                int a = guide_accept_a(g, p, icon[g]), yes = 0, want;
                for (r = 0; r < 256; r++) {
                    s16 bomb0 = *(s16 *)(REC(g) + 10), str0 = *(s16 *)(screen_w + 254 + 32);
                    step_mode = 6;
                    now_girl = (u8)g;
                    tel_sasoi_basyo = p;
                    g_rand_mode = 1;
                    g_rand_value = r | (int)rnd(128) << 8;
                    tel_dialog20();
                    if (step_mode == 32) {
                        yes++;
                    } else {
                        /* a no: stress + 9 */
                        n++;
                        if (*(s16 *)(screen_w + 254 + 32) - str0 != 9 && bad++ < 8)
                            printf("girl %d place %d no: stress %+d\n", g, p,
                                   *(s16 *)(screen_w + 254 + 32) - str0);
                    }
                    /* either way: bomb - 12, not below 0 */
                    n++;
                    if (*(s16 *)(REC(g) + 10) != (bomb0 > 12 ? bomb0 - 12 : 0) && bad++ < 8)
                        printf("girl %d place %d: bomb %d -> %d\n", g, p, bomb0,
                               *(s16 *)(REC(g) + 10));
                    memcpy((void *)SAVE_LO, save, SAVE_N);
                }
                want = a < 0 ? 0 : a > 256 ? 256 : a;
                n++;
                yes_total += yes;
                if (yes != want && bad++ < 8)
                    printf("state %d girl %d place %d: game yes %d of 256, guide %d\n", i, g, p,
                           yes, want);
            }
    }
    printf("tel yes answers %d\n", yes_total);
    printf("tel mismatches %d of %d\n", bad, n);
    return bad;
}

static int check_date(int states) {
    int i, g, p, k, bad = 0, n = 0;
    if (map_overlay("DATE")) return 1;
    for (i = 0; i < states; i++) {
        randomise();
        {
            static const int z[] = { 0, 1, 24, 25, 49, 50, 74, 75, 99, 100, 101, 300 };
            set_stat(5, rnd(2) ? z[rnd(12)] : (int)rnd(400));
        }
        for (g = 0; g < 11; g++)
            for (p = 0; p < 21; p++)
                for (k = 0; k < 5; k++) {
                    s32 *rec = (s32 *)(REC(g));
                    s32 before[3];
                    int w;
                    rec[0] = (40 + (int)rnd(40)) << 16;
                    rec[1] = (40 + (int)rnd(40)) << 16;
                    rec[2] = (70 + (int)rnd(40)) << 16;
                    before[0] = rec[0]; before[1] = rec[1]; before[2] = rec[2];
                    now_girl = (u8)g;
                    now_basyo = (u8)p;
                    serifu_no = 0;
                    imp[0] = (s16)k;
                    date_score_apply();
                    for (w = 0; w < 3; w++) {
                        int game = rec[w] - before[w], mine = guide_date_delta(g, p, k, w);
                        n++;
                        if (game != mine && bad++ < 8)
                            printf("girl %d place %d imp %d which %d zat %d: game %d guide %d\n",
                                   g, p, k, w, para_zat, game, mine);
                    }
                }
    }
    printf("date mismatches %d of %d\n", bad, n);
    return bad;
}

/* ---- the calendar file and the exams ---- */

extern s32 score_grade_calc(void);
extern s32 get_etc_hosei(void);
extern s32 work_fuku[];
extern s32 siken_total;
extern u8 now_youbi;

s32 func_801463F0(void) { return get_etc_hosei(); }    /* the head symbol */
static int g_lib;
s32 etc_month_record_check(s32 spot) { (void)spot; return g_lib; }
void game_reset(void) { printf("game_reset\n"); }

static int map_file(const char *rel, unsigned long addr) {
    char path[300];
    FILE *f;
    long n;
    snprintf(path, sizeof path, "pc/assets/raw/CDROM/%s", rel);
    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "%s: missing\n", path);
        return -1;
    }
    n = (long)fread((void *)addr, 1, 0x80200000ul - addr, f);
    fclose(f);
    return n > 0 ? 0 : -1;
}

/* cal: the guide's reading of CALROM.NBN, every month (the test compares
 * it with an independent parse).  calram RAM: the two months the game has
 * loaded into cal_day_kind ({n, kind[31]}, {first weekday, event[31]} for
 * this month at +0 and the next at +64; cal_font_load) against the guide's
 * reading of the same months.  (cal_font_load itself cannot run here: it
 * reads a PS1 pointer table through a host pointer.) */
static int print_cal(void) {
    int k, i, n, f;
    unsigned char kind[31], ev[31];
    for (k = 0; k < 36; k++) {
        if (!guide_cal_month(k, &n, &f, kind, ev)) {
            printf("no calendar\n");
            return 1;
        }
        printf("month %d n %d first %d kind ", k, n, f);
        for (i = 0; i < n; i++) printf("%d", kind[i]);
        printf(" ev");
        for (i = 0; i < n; i++) printf(" %d", ev[i]);
        printf("\n");
    }
    return 0;
}

static int check_calram(const char *ram) {
    int blk, i, bad = 0, n = 0, y, m;
    if (load_image(ram)) return 2;
    y = now_year;
    m = now_month;
    for (blk = 0; blk < 2; blk++) {
        int k = (y - 95) * 12 + m - 4, gn, gf;
        unsigned char kind[31], ev[31];
        const u8 *c = cal_day_kind + 64 * blk;
        if (!guide_cal_month(k, &gn, &gf, kind, ev)) return 1;
        n++;
        if (c[0] != gn || c[32] != (u8)gf) {
            bad++;
            printf("%d/%d: RAM n %d first %d, guide %d %d\n", y, m, c[0], c[32], gn, gf);
        }
        for (i = 0; i < gn; i++) {
            n += 2;
            if ((c[1 + i] != kind[i] || c[33 + i] != ev[i]) && bad++ < 8)
                printf("%d/%d/%d: RAM %d/%d guide %d/%d\n", y, m, i + 1, c[1 + i], c[33 + i],
                       kind[i], ev[i]);
        }
        if (++m > 12) { m = 1; y++; }
    }
    printf("calram %d/%d checked\n", now_year, now_month);
    printf("calram mismatches %d of %d\n", bad, n);
    return bad;
}

static int check_exam(int states) {
    static const int when[][2] = { { 95, 7 }, { 95, 12 }, { 96, 3 }, { 96, 7 }, { 96, 12 },
                                   { 97, 3 }, { 97, 7 }, { 97, 12 }, { 98, 2 } };
    int i, d, w, r, bad = 0, n = 0, top = 0;
    if (map_overlay("ETC")) return 1;
    for (i = 0; i < states; i++) {
        randomise();
        set_stat(1, (int)rnd(200));
        set_stat(2, (int)rnd(200));
        set_stat(3, (int)rnd(200));
        set_stat(5, (int)rnd(200));
        byouki_kind = (u8)((byouki_kind & 0xF0) | rnd(4));
        for (d = 0; d < 5; d++)
            for (w = 0; w < 9; w++)
                for (r = 0; r < 32768; r += 1 + (int)rnd(700)) {
                    int mine;
                    now_youbi = (u8)d;
                    now_year = (u8)when[w][0];
                    now_month = (u8)when[w][1];
                    g_lib = (int)rnd(2);
                    g_rand_mode = 1;
                    g_rand_value = r;
                    siken_total = 0;
                    score_grade_calc();
                    mine = guide_exam_score(d, when[w][0], when[w][1], g_lib, r);
                    n++;
                    top += work_fuku[d + 10] >= 95;
                    if (work_fuku[d + 10] != mine && bad++ < 8)
                        printf("day %d %d/%d r %d lib %d: game %d guide %d\n", d, when[w][0],
                               when[w][1], r, g_lib, work_fuku[d + 10], mine);
                }
    }
    printf("exam scores of 95+: %d\n", top);
    printf("exam mismatches %d of %d\n", bad, n);
    return bad;
}

/* ---- the pages ---- */

static GuideDoc g_doc;
static char g_text[200000];

static void print_doc(const char *title) {
    guide_doc_text(&g_doc, g_text, sizeof g_text);
    printf("=== %s\n%s", title, g_text);
}

static int do_text(const char *ram) {
    static GsvState s;
    int t, g;
    memset(&s, 0, sizeof s);
    if (strcmp(ram, "-")) {
        if (load_image(ram)) return 2;
        gsv_read(&s);
    }
    for (t = 0; t < GUIDE_TABS; t++) {
        if (t == GUIDE_GIRLS) {
            for (g = 0; g < GUIDE_NGIRLS; g++) {
                char note[96], title[160];
                guide_build(&g_doc, t, g, &s, 280);
                guide_girl_note(g, &s, note, sizeof note);
                snprintf(title, sizeof title, "Girls %d %s [%s]", g, guide_girl_name(g), note);
                print_doc(title);
            }
            continue;
        }
        guide_build(&g_doc, t, -1, &s, 280);
        print_doc(guide_tab_name(t));
    }
    return 0;
}

static void cv_fill(void *ctx, int x, int y, int w, int h, unsigned rgba) {
    (void)ctx; (void)x; (void)y; (void)w; (void)h; (void)rgba;
}
static void cv_text(void *ctx, int x, int y, unsigned rgba, const char *t) {
    (void)ctx;
    printf("%3d %3d %08x %s\n", x, y, rgba, t);
    if (x < 0 || y < 0 || x + ui_text_width(t) > 284 || y + UI_BASE + 1 > 168)
        printf("OUTSIDE the canvas: %d %d %d \"%s\"\n", x, y, ui_text_width(t), t);
}

static int do_page(const char *ram) {
    static PcMenuView v;
    PcMenuCanvas cv;
    int tab, i;
    if (strcmp(ram, "-") && load_image(ram)) return 2;
    guide_menu_register();
    if (guide_page() <= 0) {
        printf("no page\n");
        return 3;
    }
    pc_menu_open_page(guide_page());
    memset(&cv, 0, sizeof cv);
    cv.w = 284;
    cv.h = 168;
    cv.fill = cv_fill;
    cv.text = cv_text;
    for (tab = 0; tab < GUIDE_TABS; tab++) {
        pc_menu_view(&v);
        if (!v.custom) return 4;
        printf("TAB %d %s\n", tab, v.title);
        v.custom->draw(v.custom->ctx, &cv);
        if (tab == GUIDE_GIRLS) {
            /* open the third girl, scroll to the end, step to the next */
            pc_menu_input(PC_MENU_DOWN);
            pc_menu_input(PC_MENU_DOWN);
            pc_menu_input(PC_MENU_OK);
            printf("GIRL\n");
            v.custom->draw(v.custom->ctx, &cv);
            for (i = 0; i < 40; i++) pc_menu_input(PC_MENU_DOWN);
            printf("GIRL END\n");
            v.custom->draw(v.custom->ctx, &cv);
            pc_menu_input(PC_MENU_RIGHT);
            printf("GIRL NEXT\n");
            v.custom->draw(v.custom->ctx, &cv);
            pc_menu_input(PC_MENU_BACK);
            pc_menu_view(&v);
            printf("BACK %s\n", v.custom ? "list" : "closed");
        } else {
            for (i = 0; i < 80; i++) pc_menu_input(PC_MENU_DOWN);
            printf("END\n");
            v.custom->draw(v.custom->ctx, &cv);
        }
        pc_menu_input(PC_MENU_RIGHT);
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "";
    const char *exe = "pc/assets/raw/PSX.EXE";
    int n = argc > 2 ? atoi(argv[2]) : 0;
    if (argc > 3) g_rng = strtoul(argv[3], NULL, 0);
    if (!strcmp(mode, "cal")) {
        if (map_ram(argc > 2 ? argv[2] : exe)) return 2;
        return print_cal();
    }
    if (!strcmp(mode, "calram")) {
        if (argc < 3 || map_ram(argc > 3 ? argv[3] : exe)) return 2;
        return check_calram(argv[2]) ? 1 : 0;
    }
    if (!strcmp(mode, "act")) {
        if (map_ram(argc > 2 ? argv[2] : exe)) return 2;
        return check_act() ? 1 : 0;
    }
    if (!strcmp(mode, "text") || !strcmp(mode, "playertext") || !strcmp(mode, "page")) {
        if (argc < 3 || map_ram(argc > 3 ? argv[3] : exe)) return 2;
        /* "text": with the provenance lines the game hides from players;
         * "playertext": the pages as the game shows them */
        guide_show_sources = !strcmp(mode, "text");
        return strcmp(mode, "page") ? do_text(argv[2]) : do_page(argv[2]);
    }
    if (argc < 4 || map_ram(argc > 4 ? argv[4] : exe)) {
        fprintf(stderr, "usage: %s act|poor|debut|ill|girls N SEED [EXE] | text|page RAM|- [EXE]\n",
                argv[0]);
        return 2;
    }
    if (!strcmp(mode, "poor")) return check_poor(n) ? 1 : 0;
    if (!strcmp(mode, "debut")) return check_debut(n) ? 1 : 0;
    if (!strcmp(mode, "ill")) return check_ill(n) ? 1 : 0;
    if (!strcmp(mode, "girls")) return check_girls(n) ? 1 : 0;
    if (!strcmp(mode, "invite")) return check_invite(n) ? 1 : 0;
    if (!strcmp(mode, "tel")) return check_tel(n) ? 1 : 0;
    if (!strcmp(mode, "date")) return check_date(n) ? 1 : 0;
    if (!strcmp(mode, "exam")) return check_exam(n) ? 1 : 0;
    return 2;
}
