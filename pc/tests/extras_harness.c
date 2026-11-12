/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The extras (pc/hal/extras.h) without the game: extras.c, extras_state.c,
 * extras_gallery.c and extras_view.c linked with profile.c, settings.c,
 * menu.c, present_layout.c (the PNG writer / reader), gpu_scanout.c and the
 * disc directory (pc/src/file_lba_table.c).  The detectors are fed views
 * built here; the gallery gets the calls pc/hal/cd.c, libpress.c and gpu.c
 * make, against a VRAM owned by this file.  Run by pc/tools/test_extras.py,
 * one mode per process (the record directory is argv[2]):
 *
 *   config        prints whether the extras are on (profile, harness rule)
 *   endings DIR   a real ending decision, the OPTION viewer, a load into a
 *                 decided ending, a state loaded at start: prints each run
 *   achievements DIR  one view per fact, and the views that must not unlock
 *   persist DIR   unlock, reopen, unknown lines kept, atomic writes
 *   gallery DIR   a still loaded and shown, one loaded and never shown, an
 *                 event scene (its brightest frame), known sources skipped
 *   view DIR      the three pages drawn through a recording canvas
 * Exit 0 = every expectation of the mode held; failures say which. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "extras.h"
#include "gpu_display.h"
#include "pc_menu.h"
#include "pc_ui.h"

u16 g_vram[512][1024];
int g_extras_gpu;

extern void extras_note_cd_load(const char *path, unsigned ps1_dest, unsigned bytes);
extern void extras_gallery_still(const void *bs);
extern void extras_gallery_upload(int x, int y, int w, int h, const void *data);
extern void extras_gallery_draw(unsigned tpage, int u0, int v0, int u1, int v1);
extern void extras_gallery_frame_end(const ExView *v);
extern void extras_gallery_reset(void);
extern void extras_view_register(void);
extern int extras_view_page(int which);

static GpuScanoutState g_disp = { 0, 0, 320, 240, 0, 1 };
void gpu_get_scanout_state(GpuScanoutState *s) { *s = g_disp; }

static int g_fail;
#define EXPECT(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); g_fail = 1; } } while (0)

static ExView view(int step, int sub, int subsub, int in_game) {
    ExView v;
    memset(&v, 0, sizeof v);
    v.step = step;
    v.sub = sub;
    v.subsub = subsub;
    v.in_game = in_game;
    v.year = 1998;
    v.month = 3;
    v.day = 1;
    v.basyo_pack = 0xB000;
    v.now_girl = 13;
    v.scene_girl = -1;
    return v;
}

static void frames(ExView v, int n) {
    while (n--) ex_observe(&v);
}

static int lines_starting(const char *dir, const char *prefix) {
    char path[1200], line[512];
    FILE *f;
    int n = 0;
    snprintf(path, sizeof path, "%s/extras.txt", dir);
    f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) n += !strncmp(line, prefix, strlen(prefix));
    fclose(f);
    return n;
}

/* ---- endings ---- */
static int mode_endings(const char *dir) {
    ExView v;
    int k;
    ex_store_open(dir);
    /* a game: the planner, then schedule_exit's goto_step(99) through the
     * loader, the ending overlay's sub step 0 (decision at sub_sub 0) */
    frames(view(52, 1, 0, 1), 5);
    v = view(EX_STEP_LOADER, 0, 0, 1);
    v.prev_step = 99;
    frames(v, 3);
    v = view(99, 0, 0, 1);
    v.now_girl = 13;            /* ending_select_init writes it in this frame */
    frames(v, 1);
    v = view(99, 0, 1, 1);
    v.now_girl = 4;
    v.girl[4].tokimeki = 120;
    v.girl[4].yuukou = 110;
    v.girl[4].bomb = 7;
    v.girl[4].met = 1;
    v.params[3] = 345;
    frames(v, 30);
    v.subsub = 7;
    frames(v, 30);
    v = view(99, 3, 0, 1);
    v.now_girl = 4;
    frames(v, 30);
    frames(view(194, 0, 0, 1), 30);
    EXPECT(ex_ending_runs() == 1, "one decision recorded, got %d", ex_ending_runs());
    EXPECT(ex_ending_seen(4, NULL) == 1, "Saki's ending recorded");
    {
        const ExEndingRun *r = ex_ending_run(0);
        EXPECT(r && r->tokimeki == 120 && r->yuukou == 110 && r->bomb == 7 && r->met == 1 &&
               r->params[3] == 345 && r->year == 1998, "the run's stats");
    }
    k = ex_achievement_count();
    EXPECT(lines_starting(dir, "ending 0") == 0 && lines_starting(dir, "ending ") == 1,
           "extras.txt has the ending line");
    /* the OPTION viewer: title -> OPTION (33) -> loader -> 99 at sub 3,
     * Yoshio's song at 7/15: never a decision -- also with the calendar
     * of a finished game still in RAM (1998-03, a step past 20: the
     * quicksave.c test calls that "in a game") */
    frames(view(18, 2, 1, 0), 5);
    frames(view(33, 1, 0, 1), 5);
    v = view(99, 3, 0, 1);
    v.now_girl = 7;
    frames(v, 20);
    frames(view(33, 1, 0, 1), 5);
    v = view(99, 7, 15, 1);
    frames(v, 20);
    EXPECT(ex_ending_runs() == 1, "the viewer is not an ending (%d runs)", ex_ending_runs());
    /* a state loaded into a decided ending is not a new decision */
    frames(view(52, 1, 0, 1), 5);
    v = view(99, 0, 3, 1);
    v.now_girl = 12;
    ex_rearm(&v);
    frames(v, 20);
    EXPECT(ex_ending_runs() == 1, "a load into the decided ending records nothing");
    /* ... but a state loaded before the decision plays it */
    frames(view(52, 1, 0, 1), 5);
    v = view(99, 0, 0, 1);
    ex_rearm(&v);
    frames(v, 2);
    v.subsub = 1;
    v.now_girl = 12;
    frames(v, 5);
    EXPECT(ex_ending_runs() == 2 && ex_ending_seen(12, NULL) == 1, "Rei after a load at 99/0/0");
    /* a session whose first frame is a decided ending (a state loaded at
     * start): nothing */
    ex_store_open(dir);
    EXPECT(ex_ending_runs() == 2, "the record reloads (%d)", ex_ending_runs());
    v = view(99, 0, 4, 1);
    v.now_girl = 3;
    frames(v, 10);
    EXPECT(ex_ending_runs() == 2, "no decision from the first frame");
    /* an impossible id is ignored */
    frames(view(52, 1, 0, 1), 2);
    v = view(99, 0, 1, 1);
    v.now_girl = 200;
    frames(v, 2);
    EXPECT(ex_ending_runs() == 2, "now_girl 200 is not an ending");
    printf("endings %d distinct %d achievements %d of %d\n", ex_ending_runs(), ex_endings_distinct(),
           ex_achievements_unlocked(), k);
    return g_fail;
}

/* ---- achievements ---- */
static int unlocked(const char *key) {
    int i;
    for (i = 0; i < ex_achievement_count(); i++)
        if (!strcmp(ex_achievement(i)->key, key)) return ex_achievement_time(i) != 0;
    return -1;
}

static int mode_achievements(const char *dir) {
    static const struct { int step; const char *key; } steps[] = {
        { 80, "first_date" }, { 88, "double_date" }, { 89, "walk_home" }, { 91, "first_meeting" },
        { 87, "birthday" }, { 69, "phone_call" }, { 114, "sports_festival" },
        { 113, "culture_festival" }, { 81, "school_trip" }, { 82, "christmas" },
        { 84, "valentine" },
    };
    ExView v;
    int i, before;
    ex_store_open(dir);
    /* nothing outside a game (the title, the viewer, name entry) */
    for (i = 0; i < (int)(sizeof steps / sizeof steps[0]); i++) frames(view(steps[i].step, 0, 0, 0), 3);
    v = view(52, 0, 0, 0);
    v.basyo_pack = 0x8000;
    v.params[1] = 999;
    frames(v, 3);
    EXPECT(ex_achievements_unlocked() == 0, "nothing unlocks outside a game (%d)",
           ex_achievements_unlocked());
    /* a new game: no club (0xB000), stress at 999 is not a perfect stat */
    v = view(52, 0, 0, 1);
    v.params[8] = 999;
    frames(v, 3);
    EXPECT(unlocked("club") == 0, "basyo_pack 0xB000 is no club");
    EXPECT(unlocked("max_stat") == 0, "stress 999 does not count");
    for (i = 0; i < (int)(sizeof steps / sizeof steps[0]); i++) {
        before = ex_achievements_unlocked();
        frames(view(steps[i].step, 1, 0, 1), 2);
        EXPECT(unlocked(steps[i].key) == 1, "step %d unlocks %s", steps[i].step, steps[i].key);
        EXPECT(ex_achievements_unlocked() == before + 1, "exactly one more at step %d", steps[i].step);
    }
    v = view(52, 0, 0, 1);
    v.basyo_pack = 0x8000;      /* tennis (spring_events.txt) */
    frames(v, 2);
    EXPECT(unlocked("club") == 1, "joining a club");
    v.params[4] = 999;
    frames(v, 2);
    EXPECT(unlocked("max_stat") == 1, "a 999");
    /* the endings: GRADUATION, the girl's, NO BOMBS; ALL after 14 */
    for (i = 0; i < EX_ENDINGS; i++) {
        frames(view(52, 1, 0, 1), 2);
        frames(view(99, 0, 0, 1), 1);
        v = view(99, 0, 1, 1);
        v.now_girl = i;
        v.girl[2].met = 1;
        v.girl[2].bomb = i == 0 ? 60 : 59;  /* the first ending has a lit bomb */
        frames(v, 2);
        if (i == 0) EXPECT(unlocked("no_bomb") == 0, "a bomb at 60 is lit");
        if (i == 1) EXPECT(unlocked("no_bomb") == 1, "59 is not");
        EXPECT(unlocked("all_endings") == (i == EX_ENDINGS - 1), "all endings after the 14th only");
    }
    EXPECT(unlocked("graduate") == 1 && unlocked("ending_13") == 1 && unlocked("ending_0") == 1,
           "ending achievements");
    EXPECT(ex_achievements_unlocked() == ex_achievement_count(), "every achievement (%d of %d)",
           ex_achievements_unlocked(), ex_achievement_count());
    EXPECT(lines_starting(dir, "achievement ") == ex_achievement_count(), "all persisted");
    printf("achievements %d of %d\n", ex_achievements_unlocked(), ex_achievement_count());
    return g_fail;
}

/* ---- persistence ---- */
static int mode_persist(const char *dir) {
    char path[1200];
    FILE *f;
    struct stat st;
    long long t0;
    int i, k = -1;
    snprintf(path, sizeof path, "%s/extras.txt", dir);
    mkdir(dir, 0755);
    f = fopen(path, "w");
    fprintf(f, "# a comment\nformat 1\nfuture_record 1 2 3\nachievement no_such_key 5\n"
               "ending garbage\nachievement first_date 1700000000\n");
    fclose(f);
    ex_store_open(dir);
    for (i = 0; i < ex_achievement_count(); i++)
        if (!strcmp(ex_achievement(i)->key, "first_date")) k = i;
    EXPECT(ex_achievement_time(k) == 1700000000, "first_date read with its time");
    EXPECT(ex_unlock("first_date", 1800000000) == 0, "an unlocked one stays as it was");
    {
        struct stat before;
        EXPECT(stat(path, &before) == 0, "the record exists");
        EXPECT(ex_unlock("valentine", 1800000001) == 1, "a new unlock");
        EXPECT(stat(path, &st) == 0, "written");
        /* written beside and renamed over: a new file, never the old one
         * rewritten in place (a crash mid-write leaves the old record) */
        EXPECT(st.st_ino != before.st_ino, "the record was replaced, not rewritten in place");
    }
    snprintf(path, sizeof path, "%s/extras.txt.tmp", dir);
    EXPECT(stat(path, &st) != 0, "no temporary file left");
    t0 = ex_achievement_time(k);
    ex_store_open("");
    EXPECT(ex_achievements_unlocked() == 0, "forgotten");
    ex_store_open(dir);
    EXPECT(ex_achievement_time(k) == t0, "first_date kept its first time");
    EXPECT(ex_achievements_unlocked() == 2, "two unlocked after reopening (%d)",
           ex_achievements_unlocked());
    EXPECT(lines_starting(dir, "future_record 1 2 3") == 1, "an unknown record survives");
    EXPECT(lines_starting(dir, "achievement no_such_key 5") == 1, "an unknown key survives");
    EXPECT(lines_starting(dir, "ending garbage") == 1, "a line not understood survives");
    EXPECT(lines_starting(dir, "format 1") == 1, "one format line");
    printf("persist ok\n");
    return g_fail;
}

/* ---- gallery ---- */
static void paint(int x0, int y0, int w, int h, unsigned seed) {
    int x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            g_vram[y0 + y][x0 + x] = (u16)(((x * 7 + y * 3 + seed) & 0x7FFF) | 0x8000);
}

static void columns(int x0, int y0, int h) {
    int i;
    for (i = 0; i < 20; i++) extras_gallery_upload(x0 + 16 * i, y0, 16, h, (const void *)0x80162000ul);
}

static int check_png(const ExPicture *p, int x0, int y0) {
    static unsigned char rgb[320 * 256 * 3];
    int x, y;
    if (ex_picture_read(p, rgb)) return 0;
    for (y = 0; y < p->h; y++)
        for (x = 0; x < p->w; x++) {
            u16 c = (u16)(((x * 7 + y * 3 + 1234u) & 0x7FFF));
            unsigned r = c & 31;
            if (rgb[(y * p->w + x) * 3] != (unsigned char)((r << 3) | (r >> 2))) return 0;
        }
    (void)x0; (void)y0;
    return 1;
}

static int mode_gallery(const char *dir) {
    ExView v = view(80, 1, 0, 1);
    int i, x, y;
    ex_store_open(dir);
    v.year = 1995;
    v.month = 4;
    v.day = 16;
    v.scene_girl = 0;
    extras_gallery_reset();
    extras_gallery_frame_end(&v);           /* the uploads hook comes on */
    EXPECT(g_extras_gpu == 1, "uploads watched, draws not (%d)", g_extras_gpu);
    /* 1. the park of date_save: read, decoded, 20 columns, drawn */
    extras_note_cd_load("./pc/assets/raw/CDROM/BS/D_09_02.BS", 0x80162000u, 20480);
    extras_gallery_still((const void *)0x80162000ul);
    paint(320, 256, 320, 160, 1234);
    columns(320, 256, 160);
    extras_gallery_upload(800, 256, 4, 12, (const void *)0x8012d640ul);  /* a glyph */
    EXPECT(g_extras_gpu == 3, "draws watched while the still waits");
    paint(320, 256, 4, 4, 99);              /* overwritten later: the copy counts */
    extras_gallery_frame_end(&v);
    EXPECT(ex_picture_count() == 0, "not shown yet");
    /* a 15-bit sprite on page (320,256): tpage = 5 | 1<<4 | 2<<7 */
    extras_gallery_draw(5 | 16 | 256, 0, 0, 255, 159);
    extras_gallery_frame_end(&v);
    EXPECT(ex_picture_count() == 1, "the shown still is in the gallery (%d)", ex_picture_count());
    if (ex_picture_count() == 1) {
        const ExPicture *p = ex_picture(0);
        EXPECT(!strcmp(p->source, "BS/D_09_02.BS") && p->kind == EX_PIC_STILL && p->w == 320 &&
               p->h == 160 && p->girl == 0 && p->step == 80 && p->month == 4,
               "its record (%s %d %dx%d girl %d)", p->source, p->kind, p->w, p->h, p->girl);
        EXPECT(check_png(p, 320, 256), "its PNG holds the uploaded pixels");
    }
    EXPECT(g_extras_gpu == 1, "draws no longer watched");
    /* 2. the same file again: known, nothing new */
    extras_gallery_still((const void *)0x80162000ul);
    columns(320, 256, 160);
    extras_gallery_draw(5 | 16 | 256, 0, 0, 255, 159);
    extras_gallery_frame_end(&v);
    EXPECT(ex_picture_count() == 1, "a known source is not added twice");
    {
        ExPicture again = *ex_picture(0);
        static unsigned char rgb[320 * 160 * 3];
        snprintf(again.hash, sizeof again.hash, "%016x", 7);
        EXPECT(ex_picture_add(&again, rgb) == 0 && ex_picture_count() == 1,
               "ex_picture_add refuses a known source");
    }
    /* 3. loaded, never shown: a 4-bit draw over it is not the picture */
    extras_note_cd_load("./pc/assets/raw/CDROM/BS/I_01_17.BS", 0x80162000u, 20480);
    extras_gallery_still((const void *)0x80162000ul);
    columns(704, 0, 160);
    for (i = 0; i < 700; i++) {
        extras_gallery_draw(11 | 0 << 7, 0, 0, 255, 159);
        extras_gallery_frame_end(&v);
    }
    EXPECT(ex_picture_count() == 1, "a still never drawn is not in the gallery");
    /* 4. a broken run of columns (a gap) is not a picture */
    extras_note_cd_load("./pc/assets/raw/CDROM/BS/D_01_51.BS", 0x80162000u, 20480);
    extras_gallery_still((const void *)0x80162000ul);
    for (i = 0; i < 20; i++) extras_gallery_upload(320 + 16 * i + (i > 9 ? 16 : 0), 256, 16, 160, NULL);
    extras_gallery_draw(5 | 16 | 256, 0, 0, 255, 159);
    extras_gallery_frame_end(&v);
    EXPECT(ex_picture_count() == 1, "columns with a gap are no picture");
    /* 5. a still that is not from BS/ (the name entry's NAME_SAK.BS) */
    extras_note_cd_load("./pc/assets/raw/CDROM/NAME_ENT/NAME_SAK.BS", 0x80162000u, 20480);
    extras_gallery_still((const void *)0x80162000ul);
    columns(320, 256, 160);
    extras_gallery_draw(5 | 16 | 256, 0, 0, 255, 159);
    extras_gallery_frame_end(&v);
    EXPECT(ex_picture_count() == 1, "only BS/ stills count");
    /* 6. an event scene: EV_FU01's sheets, drawn while the screen fades in */
    v = view(93, 1, 0, 1);
    v.scene_girl = 0;
    extras_note_cd_load("./pc/assets/raw/CDROM/EVENT/EV_FU01.BIN", 0x801B0000u, 157696);
    for (i = 0; i < 6; i++)
        extras_gallery_upload(320 + 64 * i, 128, 64, 128, (const void *)(0x801B2000ul + 0x4000ul * i));
    for (i = 0; i < 300; i++) {
        /* the displayed frame at (0,0): brightness ramps to 31 at frame 40,
         * then a darker animation phase; the brightest is frame 40's */
        int lvl = i < 40 ? i * 31 / 40 : i == 40 ? 31 : 20;
        for (y = 0; y < 240; y++)
            for (x = 0; x < 320; x++) g_vram[y][x] = (u16)(lvl | (lvl << 5) | (lvl << 10));
        g_vram[0][0] = (u16)(i == 40 ? 0x1F : 0);   /* frame 40's mark */
        if (i < 260) extras_gallery_draw(5 | 1 << 7, 0, 0, 127, 127);  /* 8-bit, (320,0) page... */
        extras_gallery_draw(5 | 1 << 7 | 0, 0, 128, 127, 255);        /* ... its sheets at y 128 */
        extras_gallery_frame_end(&v);
    }
    EXPECT(ex_picture_count() == 2, "the event scene is in (%d)", ex_picture_count());
    if (ex_picture_count() == 2) {
        static unsigned char rgb[320 * 240 * 3];
        const ExPicture *p = ex_picture(1);
        EXPECT(!strcmp(p->source, "EVENT/EV_FU01.BIN") && p->kind == EX_PIC_EVENT && p->w == 320 &&
               p->h == 240 && p->girl == 0 && p->step == 93, "its record");
        EXPECT(ex_picture_read(p, rgb) == 0 && rgb[0] == 248 && rgb[1] == 0 && rgb[3] == 248,
               "the brightest frame was kept (%d %d %d)", rgb[0], rgb[1], rgb[3]);
    }
    /* the record and its files */
    EXPECT(lines_starting(dir, "picture ") == 2, "two picture lines");
    printf("gallery %d stills %d/%d events %d/%d\n", ex_picture_count(), ex_pictures_seen(0),
           ex_pictures_total(0), ex_pictures_seen(1), ex_pictures_total(1));
    return g_fail;
}

/* ---- the pages ---- */
static int g_texts, g_images;
static char g_all[8192];
static void c_fill(void *ctx, int x, int y, int w, int h, unsigned rgba) {
    (void)ctx; (void)x; (void)y; (void)w; (void)h; (void)rgba;
}
static int g_outside;
static void c_text(void *ctx, int x, int y, unsigned rgba, const char *s) {
    (void)ctx; (void)rgba;
    g_texts++;
    /* the canvas is 284 x 168 units; latin12 measures the text */
    if (x < 0 || y < 0 || x + ui_text_width(s) > 284 || y + UI_BASE + 1 > 168) {
        fprintf(stderr, "outside the canvas: %d %d \"%s\"\n", x, y, s);
        g_outside++;
    }
    if (strlen(g_all) + strlen(s) + 2 < sizeof g_all) { strcat(g_all, s); strcat(g_all, "|"); }
}
static void c_image(void *ctx, int x, int y, int w, int h, const unsigned char *rgb, int iw, int ih,
                    unsigned long key) {
    (void)ctx; (void)x; (void)y; (void)w; (void)h; (void)rgb; (void)iw; (void)ih; (void)key;
    g_images++;
}

static void draw_page(int page) {
    PcMenuCanvas c;
    const PcMenuCustom *ops;
    pc_menu_open_page(page);
    ops = pc_menu_custom();
    memset(&c, 0, sizeof c);
    c.w = 284;
    c.h = 168;
    c.fill = c_fill;
    c.text = c_text;
    c.image = c_image;
    g_all[0] = 0;
    if (ops && ops->draw) ops->draw(ops->ctx, &c);
    if (ops && ops->help) {
        strcat(g_all, ops->help(ops->ctx, 0));
        strcat(g_all, "|");
        strcat(g_all, ops->help(ops->ctx, 1));
    }
}

static int mode_view(const char *dir) {
    ExView v;
    ex_store_open(dir);
    frames(view(52, 1, 0, 1), 2);
    frames(view(99, 0, 0, 1), 1);
    v = view(99, 0, 1, 1);
    v.now_girl = 11;
    frames(v, 2);
    pc_menu_reset();
    extras_view_register();
    EXPECT(extras_view_page(0) > 0 && extras_view_page(1) > 0 && extras_view_page(2) > 0, "pages");
    draw_page(extras_view_page(0));
    EXPECT(strstr(g_all, "Miharu Tatebayashi") && strstr(g_all, "Seen 1 time") == NULL,
           "endings page: Miharu listed (%s)", g_all);
    EXPECT(strstr(g_all, "1 of 14 endings seen") != NULL, "endings count (%s)", g_all);
    pc_menu_input(PC_MENU_UP);    /* the last row: Yoshio; up again: Rei; again: Miharu */
    pc_menu_input(PC_MENU_UP);
    pc_menu_input(PC_MENU_UP);
    draw_page(extras_view_page(0));
    pc_menu_input(PC_MENU_UP);
    pc_menu_input(PC_MENU_UP);
    pc_menu_input(PC_MENU_UP);
    {
        PcMenuCanvas c;
        const PcMenuCustom *ops = pc_menu_custom();
        memset(&c, 0, sizeof c);
        c.w = 284; c.h = 168; c.fill = c_fill; c.text = c_text; c.image = c_image;
        g_all[0] = 0;
        ops->draw(ops->ctx, &c);
        EXPECT(strstr(g_all, "Seen 1 time") != NULL, "Miharu's details (%s)", g_all);
    }
    draw_page(extras_view_page(2));
    EXPECT(strstr(g_all, "of 30 unlocked") != NULL, "achievements count (%s)", g_all);
    draw_page(extras_view_page(1));
    EXPECT(strstr(g_all, "Pictures 0/301") && strstr(g_all, "Event scenes 0/77"),
           "gallery totals from the disc directory (%s)", g_all);
    EXPECT(g_outside == 0, "%d texts ran past the canvas", g_outside);
    printf("view ok\n");
    return g_fail;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "";
    const char *dir = argc > 2 ? argv[2] : "";
    if (!strcmp(mode, "config")) {
        printf("extras=%d\n", extras_active());
        return 0;
    }
    if (!strcmp(mode, "endings")) return mode_endings(dir);
    if (!strcmp(mode, "achievements")) return mode_achievements(dir);
    if (!strcmp(mode, "persist")) return mode_persist(dir);
    if (!strcmp(mode, "gallery")) return mode_gallery(dir);
    if (!strcmp(mode, "view")) return mode_view(dir);
    fprintf(stderr, "unknown mode %s\n", mode);
    return 2;
}
