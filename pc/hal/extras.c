/* SPDX-License-Identifier: GPL-3.0-or-later */
/* extras.c -- persistent extras of the enhanced profile: endings seen,
 * achievements, the gallery's record (pc/hal/extras.h; the pictures
 * themselves are found by pc/hal/extras_gallery.c, the pages drawn by
 * pc/hal/extras_view.c).
 *
 * The ending.  ending_select_init (ENDING overlay 0x801330F4,
 * src/overlay/ENDING/ending_select_init.c) decides it once: `now_girl = 13`,
 * then Rei (12) when score[0] < 500 && denwa_cnt >= 70 && ev_flag4f == 2,
 * else the met girl 0..10 with the highest score above 499, then Miharu
 * (11) on a dice roll when nobody qualified.  now_girl is
 * `screen_w.ending` = 0x800EF6BF (`#define now_girl screen_w.ending`).  It
 * runs as ending_main0's sub_sub step 0 of sub step 0 (ENDING step 99)
 * and ends with sub_sub_step_up(), so the first frame that shows step 99
 * at sub_step 0 with sub_sub_step >= 1 has the decision in now_girl.  The
 * OPTION ending viewer enters step 99 at sub_step 3 (option_load_wait.c)
 * or 7/15 (option_ending_start.c) and never runs the decision: it is not
 * counted.  The 14 endings are now_girl 0..13 (the eleven girls, Miharu,
 * Rei, and Yoshio's "no girl" ending).
 *
 * Achievements: each is a fact the detectors read (the table below says
 * which); unlocked once, persisted with its time.
 *
 * Session statics (the toast queue, the detectors' latches, the record
 * as loaded) are the player's, not the machine's: pc/hal/dbg_state.c
 * KEEP_FILES keeps this file out of a state's restore. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "extras.h"
#include "file_id.h"
#include "pc_present.h"
#include "pc_settings.h"
#include "pc_async_io.h"

/* pc/hal/profile.c, save_paths.c, present.c, extras_gallery.c: weak, the
 * unit tests link this file without them */
extern int hal_switch(const char *name) __attribute__((weak));
extern const char *pc_save_dir(void) __attribute__((weak));
extern void pc_present_osd(const char *text, unsigned ms) __attribute__((weak));
extern void extras_gallery_frame_end(const ExView *v) __attribute__((weak));
extern void extras_gallery_reset(void) __attribute__((weak));

/* ---------------------------------------------------------------- names */

static const char *const k_endings[EX_ENDINGS] = {
    "Shiori Fujisaki", "Mio Kisaragi", "Yuina Himoo", "Ayako Katagiri", "Saki Nijino",
    "Yukari Koshiki", "Nozomi Kiyokawa", "Mira Kagami", "Yuko Asahina", "Megumi Mikihara",
    "Yumi Saotome", "Miharu Tatebayashi", "Rei Ijuin", "Yoshio (no girl)",
};

const char *ex_ending_name(int e) { return e >= 0 && e < EX_ENDINGS ? k_endings[e] : "?"; }

/* ---------------------------------------------------------- achievements
 * Steps are now_step values whose overlay and caller are cited:
 * overlay_load_init.c (the step -> EXN table) and the goto_step callers. */
enum {
    A_FIRST_DATE, A_DOUBLE_DATE, A_WALK_HOME, A_FIRST_MEETING, A_BIRTHDAY,
    A_PHONE, A_CLUB, A_SPORTS_FESTIVAL, A_CULTURE_FESTIVAL, A_SCHOOL_TRIP,
    A_CHRISTMAS, A_VALENTINE, A_MAX_STAT, A_GRADUATE, A_NO_BOMB, A_ALL_ENDINGS,
    A_ENDING0,                  /* + now_girl: 14 */
    A_COUNT = A_ENDING0 + EX_ENDINGS
};

static const ExAchievement k_ach[A_COUNT] = {
    /* machiawase_set.c: goto_step(80), the DATE overlay */
    { "first_date", "First date", "Go on a date" },
    /* schedule_day_main.c case 73: goto_step(88), DATE2 */
    { "double_date", "Double date", "Go on a double date" },
    /* schedule_day_main.c case 68: goto_step(89), GEKO */
    { "walk_home", "Walking home", "A girl walks home with you" },
    /* schedule_day_main.c case 72: goto_step(91), a girl's first appearance */
    { "first_meeting", "Nice to meet you", "Meet a girl for the first time" },
    /* schedule_day_main.c cases 50 / 51: goto_step(87) / (86), GEKO */
    { "birthday", "Happy birthday", "A birthday scene" },
    /* TEL (steps 67 / 69): schedule_day_main.c, holiday_tel_exit.c */
    { "phone_call", "On the line", "A phone call" },
    /* basyo_pack's club (bits 12..15) leaves 11, "no club": a new game
     * starts at 0xB000 (measured, date_save.txt); clubs are 0..10 (culture
     * 0..5, sports 6..10: schedule_day_main.c case 36, the RENSYU recipes);
     * spring_events.txt joins tennis, 0x8000 */
    { "club", "Club member", "Join a club" },
    /* schedule_day_main.c: goto_step(114), TAIIKU */
    { "sports_festival", "Sports festival", "Take part in the sports festival" },
    /* schedule_day_main.c case 36: goto_step(112) / (113), BUNKASAI / BUNKAKEN */
    { "culture_festival", "Culture festival", "The culture festival" },
    /* schedule_day_main.c case 65: goto_step(81), SHUGAKU */
    { "school_trip", "School trip", "Go on the school trip" },
    /* pre_xmas.c: goto_step(82), the Christmas party (SHOUGATU) */
    { "christmas", "Christmas party", "The Christmas party" },
    /* schedule_day_main.c case 49: goto_step(84), VALEN */
    { "valentine", "Valentine's Day", "Valentine's Day" },
    /* parameter_change / param_clamp cap a parameter at 999 */
    { "max_stat", "Perfect 999", "Raise any parameter but stress to 999" },
    /* the ending decision (ending_select_init) */
    { "graduate", "Graduation", "See any ending" },
    { "no_bomb", "No bombs", "Graduate with no girl's bomb at 60 or more" },
    { "all_endings", "Every ending", "See all 14 endings" },
    { "ending_0", "Shiori", "Shiori Fujisaki's ending" },
    { "ending_1", "Mio", "Mio Kisaragi's ending" },
    { "ending_2", "Yuina", "Yuina Himoo's ending" },
    { "ending_3", "Ayako", "Ayako Katagiri's ending" },
    { "ending_4", "Saki", "Saki Nijino's ending" },
    { "ending_5", "Yukari", "Yukari Koshiki's ending" },
    { "ending_6", "Nozomi", "Nozomi Kiyokawa's ending" },
    { "ending_7", "Mira", "Mira Kagami's ending" },
    { "ending_8", "Yuko", "Yuko Asahina's ending" },
    { "ending_9", "Megumi", "Megumi Mikihara's ending" },
    { "ending_10", "Yumi", "Yumi Saotome's ending" },
    { "ending_11", "Miharu", "The hidden girl: Miharu Tatebayashi" },
    { "ending_12", "Rei", "The hidden girl: Rei Ijuin" },
    { "ending_13", "Best friend", "Yoshio's ending: no confession" },
};

/* step -> achievement: the scenes whose entry is the fact */
static const struct { int step, ach; } k_step_ach[] = {
    { 80, A_FIRST_DATE }, { 88, A_DOUBLE_DATE }, { 89, A_WALK_HOME },
    { 91, A_FIRST_MEETING }, { 86, A_BIRTHDAY }, { 87, A_BIRTHDAY },
    { 67, A_PHONE }, { 69, A_PHONE }, { 114, A_SPORTS_FESTIVAL },
    { 112, A_CULTURE_FESTIVAL }, { 113, A_CULTURE_FESTIVAL }, { 81, A_SCHOOL_TRIP },
    { 82, A_CHRISTMAS }, { 84, A_VALENTINE },
};

int ex_achievement_count(void) { return A_COUNT; }
const ExAchievement *ex_achievement(int i) { return i >= 0 && i < A_COUNT ? &k_ach[i] : NULL; }

/* ---------------------------------------------------------------- store */

#define MAX_OTHER 64

/* the ending detector: step 99 entered and not yet decided; the first view */
static int g_end_armed, g_seen_init;

static struct {
    char dir[1100];
    int open;
    ExEndingRun runs[EX_MAX_ENDING_RUNS];
    int nruns;
    long long ach[A_COUNT];
    ExPicture pics[EX_MAX_PICTURES];
    int npics;
    char *other[MAX_OTHER];     /* unknown lines, kept */
    int nother;
} S;

long long ex_now(void) {
    const char *e = getenv("TOKI_EXTRAS_CLOCK");
    if (e && *e) return atoll(e);
    return (long long)time(NULL);
}

const char *ex_dir(void) { return S.dir; }
int ex_ending_runs(void) { return S.nruns; }
const ExEndingRun *ex_ending_run(int i) { return i >= 0 && i < S.nruns ? &S.runs[i] : NULL; }
long long ex_achievement_time(int i) { return i >= 0 && i < A_COUNT ? S.ach[i] : 0; }
int ex_picture_count(void) { return S.npics; }
const ExPicture *ex_picture(int i) { return i >= 0 && i < S.npics ? &S.pics[i] : NULL; }

int ex_ending_seen(int e, const ExEndingRun **last) {
    int i, n = 0;
    if (last) *last = NULL;
    for (i = 0; i < S.nruns; i++)
        if (S.runs[i].id == e) {
            n++;
            if (last) *last = &S.runs[i];
        }
    return n;
}

int ex_endings_distinct(void) {
    int e, n = 0;
    for (e = 0; e < EX_ENDINGS; e++) n += ex_ending_seen(e, NULL) > 0;
    return n;
}

int ex_achievements_unlocked(void) {
    int i, n = 0;
    for (i = 0; i < A_COUNT; i++) n += S.ach[i] != 0;
    return n;
}

int ex_picture_known(const char *source) {
    int i;
    for (i = 0; i < S.npics; i++)
        if (!strcmp(S.pics[i].source, source)) return 1;
    return 0;
}

int ex_pictures_seen(int kind) {
    int i, n = 0;
    for (i = 0; i < S.npics; i++) n += S.pics[i].kind == kind;
    return n;
}

/* The disc's own directory (pc/src/file_lba_table.c, built from the image):
 * stills are every file of BS/ (301), event scenes every EVENT/ file that
 * holds at least one 64x128 sheet after its 8 KiB palette header (the
 * loaders read them to 0x801B0000 and upload the sheets from +0x2000 on,
 * extras_gallery.c), i.e. larger than 0x6000 bytes (77 of the 78). */
int ex_pictures_total(int kind) {
    u32 i;
    int n = 0;
    for (i = 0; i < g_file_lba_table_count; i++) {
        const char *p = g_file_lba_table[i].path;
        if (kind == EX_PIC_STILL && !strncmp(p, "CDROM/BS/", 9)) n++;
        if (kind == EX_PIC_EVENT && !strncmp(p, "CDROM/EVENT/", 12) &&
            g_file_lba_table[i].size > 0x6000u)
            n++;
    }
    return n;
}

static void store_clear(void) {
    int i;
    for (i = 0; i < S.nother; i++) free(S.other[i]);
    memset(&S, 0, sizeof S);
}

static int ach_index(const char *key) {
    int i;
    for (i = 0; i < A_COUNT; i++)
        if (!strcmp(k_ach[i].key, key)) return i;
    return -1;
}

static void parse_line(char *line) {
    char *nl = strpbrk(line, "\r\n");
    if (nl) *nl = 0;
    if (!line[0] || line[0] == '#' || !strncmp(line, "format ", 7)) return;
    if (!strncmp(line, "ending ", 7) && S.nruns < EX_MAX_ENDING_RUNS) {
        ExEndingRun r;
        memset(&r, 0, sizeof r);
        if (sscanf(line + 7, "%lld %d %d-%d-%d %d %d %d %d %d %d %d %d %d %d %d %d %d",
                   &r.unix_time, &r.id, &r.year, &r.month, &r.day, &r.met, &r.tokimeki, &r.yuukou,
                   &r.bomb, &r.params[0], &r.params[1], &r.params[2], &r.params[3], &r.params[4],
                   &r.params[5], &r.params[6], &r.params[7], &r.params[8]) == 18 &&
            r.id >= 0 && r.id < EX_ENDINGS) {
            S.runs[S.nruns++] = r;
            return;
        }
    } else if (!strncmp(line, "achievement ", 12)) {
        char key[48];
        long long t;
        int k;
        if (sscanf(line + 12, "%47s %lld", key, &t) == 2 && (k = ach_index(key)) >= 0) {
            if (!S.ach[k] || t < S.ach[k]) S.ach[k] = t > 0 ? t : 1;
            return;
        }
    } else if (!strncmp(line, "picture ", 8) && S.npics < EX_MAX_PICTURES) {
        ExPicture p;
        memset(&p, 0, sizeof p);
        if (sscanf(line + 8, "%16s %d %d %d %lld %d %d %d-%d-%d %39s", p.hash, &p.kind, &p.w, &p.h,
                   &p.unix_time, &p.step, &p.girl, &p.year, &p.month, &p.day, p.source) == 11 &&
            p.kind >= 0 && p.kind < EX_PIC_KINDS && p.w > 0 && p.w <= 640 && p.h > 0 && p.h <= 512 &&
            strspn(p.hash, "0123456789abcdef") == 16 && !ex_picture_known(p.source)) {
            S.pics[S.npics++] = p;
            return;
        }
    }
    /* unknown or not understood: kept as it is */
    if (S.nother < MAX_OTHER) S.other[S.nother++] = strdup(line);
}

int ex_store_open(const char *dir) {
    char path[1200], line[512];
    FILE *f;
    store_clear();
    g_seen_init = 0;            /* a new record: the detectors start over */
    snprintf(S.dir, sizeof S.dir, "%s", dir ? dir : "");
    S.open = 1;
    if (!S.dir[0]) return 0;
    snprintf(path, sizeof path, "%s/extras.txt", S.dir);
    f = fopen(path, "r");
    if (!f) return errno == ENOENT ? 0 : -1;
    while (fgets(line, sizeof line, f)) parse_line(line);
    fclose(f);
    return 0;
}

/* write PATH atomically: PATH.tmp, flushed and synced, renamed over PATH */
static int write_atomic(const char *path, const char *text, size_t n) {
    char tmp[1300];
    FILE *f;
    int ok;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (pc_settings_mkdirs(path, 0)) return -1;
    f = fopen(tmp, "wb");
    if (!f) return -1;
    ok = fwrite(text, 1, n, f) == n;
    ok &= fflush(f) == 0;
    ok &= fsync(fileno(f)) == 0;
    ok &= fclose(f) == 0;
    if (!ok || rename(tmp, path)) {
        remove(tmp);
        return -1;
    }
    return 0;
}

int ex_store_save(void) {
    char path[1200];
    char *buf;
    size_t cap = 4096 + (size_t)S.nruns * 160 + (size_t)S.npics * 160 + (size_t)A_COUNT * 64 +
                 (size_t)S.nother * 520, n = 0;
    int i, r;
    if (!S.dir[0]) return 0;
    buf = malloc(cap);
    if (!buf) return -1;
#define PUT(...) (n += (size_t)snprintf(buf + n, cap - n, __VA_ARGS__))
    PUT("# Heartthrob Memorial PC Enhanced extras (pc/hal/extras.c): endings seen, achievements,\n"
        "# the gallery. Format: pc/hal/extras.h.\n"
        "format 1\n");
    for (i = 0; i < S.nruns; i++) {
        const ExEndingRun *e = &S.runs[i];
        PUT("ending %lld %d %04d-%02d-%02d %d %d %d %d %d %d %d %d %d %d %d %d %d\n", e->unix_time,
            e->id, e->year, e->month, e->day, e->met, e->tokimeki, e->yuukou, e->bomb, e->params[0],
            e->params[1], e->params[2], e->params[3], e->params[4], e->params[5], e->params[6],
            e->params[7], e->params[8]);
    }
    for (i = 0; i < A_COUNT; i++)
        if (S.ach[i]) PUT("achievement %s %lld\n", k_ach[i].key, S.ach[i]);
    for (i = 0; i < S.npics; i++) {
        const ExPicture *p = &S.pics[i];
        PUT("picture %s %d %d %d %lld %d %d %04d-%02d-%02d %s\n", p->hash, p->kind, p->w, p->h,
            p->unix_time, p->step, p->girl, p->year, p->month, p->day, p->source);
    }
    for (i = 0; i < S.nother; i++) PUT("%s\n", S.other[i]);
#undef PUT
    snprintf(path, sizeof path, "%s/extras.txt", S.dir);
    r = write_atomic(path, buf, n);
    free(buf);
    if (r) fprintf(stderr, "[EXTRAS] cannot write %s\n", path);
    return r;
}

/* ---------------------------------------------------------------- toasts */

#define TOASTS 8
static char g_toast[TOASTS][96];
static int g_toast_head, g_toast_n;
static struct timespec g_toast_next;

static void toast(const char *fmt, const char *a, const char *b) {
    char line[96];
    snprintf(line, sizeof line, fmt, a, b ? b : "");
    printf("[EXTRAS] %s\n", line);
    fflush(stdout);
    if (g_toast_n < TOASTS) {
        snprintf(g_toast[(g_toast_head + g_toast_n) % TOASTS], sizeof g_toast[0], "%s", line);
        g_toast_n++;
    }
}

static void toast_pump(void) {
    struct timespec now;
    if (!g_toast_n || !pc_present_osd) return;
    if (!pc_settings_get()->toasts) { g_toast_n = 0; return; }   /* [system] toasts */
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec < g_toast_next.tv_sec ||
        (now.tv_sec == g_toast_next.tv_sec && now.tv_nsec < g_toast_next.tv_nsec))
        return;
    pc_present_osd(g_toast[g_toast_head], 2600);
    g_toast_head = (g_toast_head + 1) % TOASTS;
    g_toast_n--;
    g_toast_next = now;
    g_toast_next.tv_sec += 3;
}

/* ---------------------------------------------------------------- engine */

/* Achievements are opt-in ([enhanced] achievements, default off;
 * TOKI_ACHIEVEMENTS=0/1 wins, for tests): while off nothing is unlocked,
 * recorded or announced. */
int ex_achievements_on(void) {
    const char *e = getenv("TOKI_ACHIEVEMENTS");
    if (e && *e) return *e == '1';
    return pc_settings_get()->achievements;
}

static int unlock_index(int k, long long when) {
    if (!ex_achievements_on()) return 0;
    if (k < 0 || k >= A_COUNT || S.ach[k]) return 0;
    S.ach[k] = when > 0 ? when : 1;
    toast("Achievement: %s%s", k_ach[k].name, NULL);
    return 1;
}

int ex_unlock(const char *key, long long when) {
    int r = unlock_index(ach_index(key), when);
    if (r) ex_store_save();
    return r;
}

static void record_ending(const ExView *v) {
    ExEndingRun r;
    int i, e = v->now_girl, first, none = 1;
    long long now = ex_now();
    if (e < 0 || e >= EX_ENDINGS) {
        printf("[EXTRAS] ending decision read now_girl %d: not an ending id, ignored\n", e);
        return;
    }
    memset(&r, 0, sizeof r);
    r.unix_time = now;
    r.id = e;
    r.year = v->year;
    r.month = v->month;
    r.day = v->day;
    for (i = 0; i < 11; i++) r.met += v->girl[i].met;
    if (e < EX_GIRL_RECORDS) {
        r.tokimeki = v->girl[e].tokimeki;
        r.yuukou = v->girl[e].yuukou;
        r.bomb = v->girl[e].bomb;
    }
    for (i = 0; i < EX_PARAMS; i++) r.params[i] = v->params[i];
    first = ex_ending_seen(e, NULL) == 0;
    if (S.nruns == EX_MAX_ENDING_RUNS) {
        memmove(&S.runs[0], &S.runs[1], sizeof S.runs[0] * (EX_MAX_ENDING_RUNS - 1));
        S.nruns--;
    }
    S.runs[S.nruns++] = r;
    printf("[EXTRAS] ending decided: now_girl %d (%s) on %04d-%02d-%02d, %d met%s\n", e,
           ex_ending_name(e), r.year, r.month, r.day, r.met, first ? ", first time" : "");
    toast(first ? "New ending: %s%s" : "Ending: %s%s", ex_ending_name(e), NULL);
    unlock_index(A_GRADUATE, now);
    unlock_index(A_ENDING0 + e, now);
    for (i = 0; i < 11; i++)
        if (v->girl[i].met && v->girl[i].bomb >= 60) none = 0;
    if (none) unlock_index(A_NO_BOMB, now);
    if (ex_endings_distinct() == EX_ENDINGS) unlock_index(A_ALL_ENDINGS, now);
    ex_store_save();
}

void ex_observe(const ExView *v) {
    int i, dirty = 0;
    long long now = 0;
    if (!g_seen_init) {
        /* the first frame: a decision already on screen (a state of the
         * ending loaded at start) is not a new one */
        g_seen_init = 1;
        g_end_armed = v->step != EX_STEP_ENDING;
    }
    /* the ending (see the header) */
    if (v->step != EX_STEP_ENDING) {
        g_end_armed = 1;
    } else if (v->sub == 0 && v->subsub == 0) {
        /* entered, the decision still to come */
    } else if (v->sub == 0 && g_end_armed && v->in_game) {
        g_end_armed = 0;
        record_ending(v);
    } else {
        g_end_armed = 0;
    }
    if (!v->in_game) return;
    /* the scene achievements */
    for (i = 0; i < (int)(sizeof k_step_ach / sizeof k_step_ach[0]); i++)
        if (v->step == k_step_ach[i].step && !S.ach[k_step_ach[i].ach]) {
            if (!now) now = ex_now();
            dirty |= unlock_index(k_step_ach[i].ach, now);
        }
    if (!S.ach[A_CLUB] && ((v->basyo_pack >> 12) & 15) <= 10)
        dirty |= unlock_index(A_CLUB, now ? now : (now = ex_now()));
    if (!S.ach[A_MAX_STAT])
        for (i = 0; i < EX_PARAMS - 1; i++)
            if (v->params[i] >= 999) {
                dirty |= unlock_index(A_MAX_STAT, now ? now : (now = ex_now()));
                break;
            }
    if (dirty) ex_store_save();
}

void ex_rearm(const ExView *v) {
    g_seen_init = 1;
    g_end_armed = v->step != EX_STEP_ENDING || (v->sub == 0 && v->subsub == 0);
}

/* A gallery picture written on the writer thread (pc/hal/async_io.c; weak:
 * the extras harness links this file alone and writes inline). */
PC_ASYNC_DECLARE_WEAK
typedef struct { char path[1300], tmp[1310]; unsigned char *rgb; int w, h; } PicJob;
static void pic_job(void *arg) {
    PicJob *j = (PicJob *)arg;
    if (present_png_write(j->tmp, j->rgb, j->w, j->h) || rename(j->tmp, j->path)) {
        remove(j->tmp);
        fprintf(stderr, "[EXTRAS] cannot write %s\n", j->path);
    }
    free(j->rgb);
    free(j);
}

int ex_picture_add(const ExPicture *p, const unsigned char *rgb) {
    char path[1300], tmp[1310];
    if (!p || !rgb || !p->source[0]) return -1;
    if (ex_picture_known(p->source)) return 0;
    if (S.npics >= EX_MAX_PICTURES) return -1;
    if (S.dir[0]) {
        snprintf(path, sizeof path, "%s/gallery/%s.png", S.dir, p->hash);
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        if (pc_async_run && !pc_settings_mkdirs(path, 0)) {
            /* encoded and written on the writer thread (pc/hal/async_io.c):
             * a picture is taken in the middle of a scene, where a slow
             * disk or an on-access scan would stall the game */
            size_t n = (size_t)p->w * p->h * 3u;
            PicJob *j = (PicJob *)malloc(sizeof *j);
            unsigned char *copy = (unsigned char *)malloc(n);
            if (j && copy) {
                memcpy(copy, rgb, n);
                snprintf(j->path, sizeof j->path, "%s", path);
                snprintf(j->tmp, sizeof j->tmp, "%s", tmp);
                j->rgb = copy;
                j->w = p->w;
                j->h = p->h;
                pc_async_run(pic_job, j, n);
                goto added;
            }
            free(j);
            free(copy);
        }
        if (pc_settings_mkdirs(path, 0) || present_png_write(tmp, rgb, p->w, p->h) ||
            rename(tmp, path)) {
            remove(tmp);
            fprintf(stderr, "[EXTRAS] cannot write %s\n", path);
            return -1;
        }
    }
added:
    S.pics[S.npics++] = *p;
    printf("[EXTRAS] gallery: %s %s %dx%d step %d girl %d -> %s.png\n",
           p->kind == EX_PIC_EVENT ? "event scene" : "picture", p->source, p->w, p->h, p->step,
           p->girl, p->hash);
    toast(p->kind == EX_PIC_EVENT ? "Gallery: new event scene%s%s" : "Gallery: new picture%s%s", "",
          NULL);
    ex_store_save();
    return 1;
}

int ex_picture_read(const ExPicture *p, unsigned char *rgb) {
    char path[1300];
    if (!p || !S.dir[0]) return -1;
    snprintf(path, sizeof path, "%s/gallery/%s.png", S.dir, p->hash);
    if (pc_async_flush) pc_async_flush();      /* a picture still being written */
    return present_png_read(path, rgb, p->w, p->h);
}

/* ---------------------------------------------------------------- the port */

static int g_active = -1;

int extras_active(void) {
    if (g_active < 0) {
        const PcSettings *s = pc_settings_get();
        const char *f = getenv("PC_EXTRAS_FORCE");
        int sw = hal_switch ? hal_switch("TOKI_EXTRAS") : 0;
        g_active = sw && (!s->harness || (f && *f == '1'));
        if (sw && !g_active)
            printf("[EXTRAS] off: automated run (%s; PC_EXTRAS_FORCE=1 turns them on)\n",
                   s->harness_reason);
    }
    return g_active;
}

void extras_store_ensure(void) {
    char dir[1100];
    if (S.open) return;
    snprintf(dir, sizeof dir, "%s/extras", pc_save_dir ? pc_save_dir() : "saves");
    if (ex_store_open(dir))
        fprintf(stderr, "[EXTRAS] cannot read %s/extras.txt\n", dir);
}

static void ensure_open(void) {
    static int said;
    extras_store_ensure();
    if (said) return;
    said = 1;
    printf("[EXTRAS] on: %s (%d endings seen, %d achievements, %d pictures)\n", S.dir,
           ex_endings_distinct(), ex_achievements_unlocked(), S.npics);
    fflush(stdout);
}

void extras_frame_end(void) {
    ExView v;
    if (!extras_active()) return;
    ensure_open();
    ex_state_read(&v);
    ex_observe(&v);
    if (extras_gallery_frame_end) extras_gallery_frame_end(&v);
    toast_pump();
}

void extras_after_load(void) {
    ExView v;
    /* gpu.c's hook flags came back with the state: this session decides */
    if (extras_gallery_reset) extras_gallery_reset();
    if (!extras_active()) return;
    ensure_open();
    ex_files_reset();
    ex_state_read(&v);
    ex_rearm(&v);
}
