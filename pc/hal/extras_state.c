/* SPDX-License-Identifier: GPL-3.0-or-later */
/* extras_state.c -- the extras' read-only view of the game
 * (pc/hal/extras_state.h).  Addresses and their meaning are from the
 * matched C and docs/game-flows/ENDINGS-GUIDE.md section 2 (each one
 * cited below); this file only reads them.
 *
 * The disc-read ring is host bookkeeping about the machine's RAM, so a
 * state load resets it (extras.c ex_after_load); its statics are the
 * session's, kept out of a state's restore (pc/hal/dbg_state.c KEEP_FILES). */
#include <stdio.h>
#include <string.h>
#include "extras_state.h"

#define RAM8(a) (*(volatile const unsigned char *)(unsigned long)(a))
#define RAM16(a) (*(volatile const unsigned short *)(unsigned long)(a))
#define RAM32(a) (*(volatile const unsigned int *)(unsigned long)(a))

/* game_main's scene triple (now_step 0x800EF868, now_sub_step 0x800EF869,
 * now_sub_sub_step 0x800EF86A) and prev_step 0x800EF86E, the step the
 * overlay loader (now_step 2) is loading (step_state_save_and_redirect.c) */
#define A_NOW_STEP 0x800EF868u
#define A_PREV_STEP 0x800EF86Eu
/* the calendar: now_year/now_month/now_day/now_youbi 0x800EE79E..7A1,
 * pass_days 0x800EE7A2 (ENDINGS-GUIDE.md 2.3) */
#define A_YEAR 0x800EE79Eu
#define A_PASS_DAYS 0x800EE7A2u
/* screen_w.ending = now_girl: ending_select_init.c writes the ending here
 * (`#define now_girl screen_w.ending`, screen_w 0x800EE760 + 3935) */
#define A_NOW_GIRL 0x800EF6BFu
/* the nine parameters, s32 16.16 at screen_w+252+4k (parameter_init.c) */
#define A_PARAMS 0x800EE85Cu
/* basyo_pack (u16; the club in bits 12..15, schedule_day_main.c case 36) */
#define A_BASYO_PACK 0x800EE854u
/* girl_tbl: 56-byte records from 0x800EE91C (girl_rec_init.c; the guide's
 * table 2.1): tokimeki +0, yuukou +4, bomb +8 (16.16), flag word +12 */
#define A_GIRL_TBL 0x800EE91Cu
#define GIRL_REC 56
/* the girl a scene is about: GEKO 93's character (schedule_day_main.c
 * case 69: event_girl_no = sched.a), OMIMAI's girl_no 0x800EEEBD */
#define A_GEKO_CHAR 0x8012B1B4u
#define A_GIRL_NO 0x800EEEBDu

static const char *const k_names[16] = {
    "SHIORI FUJISAKI", "MIO KISARAGI", "YUINA HIMOO", "AYAKO KATAGIRI",
    "SAKI NIJINO", "YUKARI KOSHIKI", "NOZOMI KIYOKAWA", "MIRA KAGAMI",
    "YUKO ASAHINA", "MEGUMI MIKIHARA", "YUMI SAOTOME", "MIHARU TATEBAYASHI",
    "REI IJUIN", "YOSHIO SAOTOME", "SOTOI", "YUMI SAOTOME",
};
static const char *const k_short[16] = {
    "SHIORI", "MIO", "YUINA", "AYAKO", "SAKI", "YUKARI", "NOZOMI", "MIRA",
    "YUKO", "MEGUMI", "YUMI", "MIHARU", "REI", "YOSHIO", "SOTOI", "YUMI",
};

const char *ex_girl_name(int g) { return g >= 0 && g < 16 ? k_names[g] : NULL; }
const char *ex_girl_short(int g) { return g >= 0 && g < 16 ? k_short[g] : NULL; }

/* the two-letter girl codes of the per-girl disc files (END_OBJ/END_xx,
 * LASTPHOT/LASTP_xx, EVENT/EV_xx..: ENDINGS-GUIDE.md section 11's table) */
int ex_girl_of_code(const char *c) {
    static const char *const codes[] = {
        "FU", "KI", "HI", "KA", "NI", "KO", "KY", "KG", "AS", "MI", "SA", "TA", "IJ", "YO",
    };
    int i;
    if (!c || !c[0] || !c[1]) return -1;
    for (i = 0; i < (int)(sizeof codes / sizeof codes[0]); i++)
        if (c[0] == codes[i][0] && c[1] == codes[i][1]) return i;
    return -1;
}

/* the girl of the scene now showing, or -1: dates and girl scenes keep her
 * in now_girl (machiawase_set.c / schedule_day_main.c cases 68-72 set it
 * before goto_step), GEKO 93 in event_girl_no, OMIMAI in girl_no */
static int scene_girl(int step) {
    int g;
    switch (step) {
    case 80: case 82: case 83: case 84: case 85: case 86: case 87: case 88:
    case 89: case 90: case 91: case 92: case 94: case 99: case 130:
        g = RAM8(A_NOW_GIRL);
        break;
    case 93:
        g = (int)RAM32(A_GEKO_CHAR);
        break;
    case 97: case 98:
        g = RAM8(A_GIRL_NO);
        break;
    default:
        return -1;
    }
    return g >= 0 && g < 16 ? g : -1;
}

void ex_state_read(ExView *v) {
    int i;
    unsigned y = RAM8(A_YEAR), m = RAM8(A_YEAR + 1), d = RAM8(A_YEAR + 2), wd = RAM8(A_YEAR + 3);
    memset(v, 0, sizeof *v);
    v->step = RAM8(A_NOW_STEP);
    v->sub = RAM8(A_NOW_STEP + 1);
    v->subsub = RAM8(A_NOW_STEP + 2);
    v->prev_step = RAM8(A_PREV_STEP);
    v->year = 1900 + (int)y;
    v->month = (int)m;
    v->day = (int)d;
    v->weekday = (int)wd;
    v->pass_days = RAM16(A_PASS_DAYS);
    /* quicksave.c qs_game_in_progress: past the title / name entry, a sane
     * calendar */
    {
        unsigned step = v->step == EX_STEP_LOADER ? (unsigned)v->prev_step : (unsigned)v->step;
        v->in_game = step > 20 && y >= 95 && y <= 98 && m >= 1 && m <= 12 && d >= 1 && d <= 31 &&
                     wd < 7;
    }
    v->now_girl = RAM8(A_NOW_GIRL);
    v->scene_girl = scene_girl(v->step);
    for (i = 0; i < EX_PARAMS; i++)
        v->params[i] = (short)RAM16(A_PARAMS + 4u * (unsigned)i + 2u);
    v->basyo_pack = RAM16(A_BASYO_PACK);
    for (i = 0; i < EX_GIRL_RECORDS; i++) {
        unsigned r = A_GIRL_TBL + GIRL_REC * (unsigned)i;
        v->girl[i].tokimeki = (short)RAM16(r + 2u);
        v->girl[i].yuukou = (short)RAM16(r + 6u);
        v->girl[i].bomb = (short)RAM16(r + 10u);
        v->girl[i].met = (RAM32(r + 12u) >> 1) & 1u;
    }
}

/* ---- disc reads ---- */

#define RING 24
typedef struct {
    unsigned dest, bytes;
    unsigned long gen;
    char rel[64];
} Load;
static Load g_loads[RING];
static unsigned long g_gen;
extern int extras_active(void) __attribute__((weak));

const char *ex_disc_rel(const char *path) {
    /* the disc path starts after the LAST "raw/CDROM/" (the asset root is
     * any folder now, and may itself contain "CDROM/"); a bare
     * "CDROM/..." path has no raw/ in front */
    const char *p = NULL, *q = path;
    while (q && (q = strstr(q, "raw/CDROM/")) != NULL) { p = q + 4; q++; }
    if (!p && path) p = strstr(path, "CDROM/");
    return p ? p + 6 : (path ? path : "");
}

void extras_note_cd_load(const char *path, unsigned ps1_dest, unsigned bytes) {
    Load *l;
    if (!extras_active || !extras_active() || !path || !bytes) return;
    l = &g_loads[g_gen % RING];
    l->dest = ps1_dest & 0x1FFFFFFFu;
    l->bytes = bytes;
    l->gen = ++g_gen;
    snprintf(l->rel, sizeof l->rel, "%s", ex_disc_rel(path));
}

int ex_file_at(unsigned addr, char *rel, size_t cap, unsigned *offset) {
    const Load *best = NULL;
    int i;
    addr &= 0x1FFFFFFFu;
    for (i = 0; i < RING; i++) {
        const Load *l = &g_loads[i];
        if (!l->gen || addr < l->dest || addr - l->dest >= l->bytes) continue;
        if (!best || l->gen > best->gen) best = l;
    }
    if (!best) return 0;
    if (rel) snprintf(rel, cap, "%s", best->rel);
    if (offset) *offset = addr - best->dest;
    return 1;
}

void ex_files_reset(void) {
    memset(g_loads, 0, sizeof g_loads);
}
