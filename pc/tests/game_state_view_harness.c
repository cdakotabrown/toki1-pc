/* SPDX-License-Identifier: GPL-3.0-or-later */
/* game_state_view_harness.c -- pc/hal/game_state_view.c against the game's
 * own functions (pc/tools/test_game_state_view.py builds and runs it).
 *
 * Linked like the port: the PS1 globals resolve at their console addresses
 * (-Wl,--just-symbols=pc/ld/psx_globals.syms) in a 2 MB block mapped at
 * 0x80000000 and seeded with the retail executable's data (the tables), and
 * the MATCHED C of get_grp, get_grp_point, grp_table_calc, get_h_yuukou(_table)
 * and get_h_tokimeki(_table) -- the functions Yoshio's evaluation screen and
 * the ending call -- is compiled in.  The oracle is therefore the game's own
 * code over the same RAM, not a second transcription.
 *
 *   harness random N SEED [EXE]   N random states: every girl's yuukou% /
 *                                 tokimeki%, get_grp's icon and score row
 *                                 compared; prints "mismatches K of M"
 *   harness image RAM [EXE]       a 2 MB RAM image (the status log's .ram):
 *                                 the same comparison, then gsv_format
 *   harness names [EXE]           the game's name tables as hex (SJIS)
 *   harness page RAM [EXE]        the status page (pc/hal/status_view.c) on a
 *                                 RAM image: registered (TOKI_STATUS=1),
 *                                 opened, each of the four tabs drawn into a
 *                                 canvas that prints "TAB n" and its text
 *                                 lines; exit 3 when there is no page
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "types.h"
#include "../hal/game_state_view.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define RAM_BASE 0x80000000ul
#define RAM_SIZE 0x200000ul

/* the game's functions (src/main/matched) */
extern void get_grp(u8 *out, s16 *clr);
extern u8 get_h_yuukou(s32 who);
extern s32 get_h_tokimeki(s32 who);
extern void grp_table_calc();
/* get_grp calls the head symbol of grp_table_calc (the port's
 * pc/src/missing_funcs_gen.c forwards it the same way) */
void func_80053364(u8 *out, s16 *clr) { grp_table_calc(out, clr); }

extern u8 girl_tbl[];
extern s16 para_tai;
extern u8 byouki_kind;
extern u8 now_step, now_year, now_month, now_day, now_youbi;

/* the status page (pc/hal/status_view.c, pc/hal/menu.c) */
#include "pc_menu.h"
#include "pc_ui.h"
extern void status_menu_register(void);
extern int status_page(void);
extern int status_hud_text(char *out, int cap);

static void cv_fill(void *ctx, int x, int y, int w, int h, unsigned rgba) {
    (void)ctx; (void)x; (void)y; (void)w; (void)h; (void)rgba;
}
static void cv_text(void *ctx, int x, int y, unsigned rgba, const char *t) {
    (void)ctx;
    printf("%3d %3d %08x %s\n", x, y, rgba, t);
    /* the canvas is 284 x 168 units; latin12 measures the text */
    if (x < 0 || y < 0 || x + ui_text_width(t) > 284 || y + UI_BASE + 1 > 168)
        printf("OUTSIDE the canvas: %d %d %d \"%s\"\n", x, y, ui_text_width(t), t);
}

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

/* compare girls 0..10 with the game's functions; returns mismatches */
static int compare(int verbose) {
    u8 out[16], gout[GSV_GIRLS];
    s16 rank[16], gscore[GSV_GIRLS];
    int i, bad = 0;
    memset(out, 0, sizeof out);
    memset(rank, 0, sizeof rank);
    get_grp(out, rank);
    gsv_grp(gout, gscore);
    for (i = 0; i < GSV_GIRLS; i++) {
        int gy = get_h_yuukou(i), gt = (u8)get_h_tokimeki(i);
        int my = gsv_yuukou_pct(i), mt = gsv_tokimeki_pct(i);
        if (gy != my || gt != mt || out[i] != gout[i] || rank[i] != gscore[i]) {
            bad++;
            if (verbose)
                printf("girl %d: game yuu%%=%d tok%%=%d icon=%d score=%d | view %d %d %d %d\n",
                       i, gy, gt, out[i], rank[i], my, mt, gout[i], gscore[i]);
        }
    }
    return bad;
}

static void randomise(void) {
    int g, k;
    s16 *para = &para_tai;
    for (g = 0; g < 13; g++) {
        u8 *r = girl_tbl + 56 * g;
        int range = rnd(8) == 0 ? 300 : 129;    /* now and then past the clamps */
        *(s16 *)(r + 2) = (s16)((int)rnd(range + 20) - 10);
        *(s16 *)(r + 6) = (s16)((int)rnd(range + 20) - 10);
        *(s16 *)(r + 10) = (s16)((int)rnd(range + 20) - 10);
        *(u32 *)(r + 12) = rnd(0x7FFFFFFF) | (rnd(3) ? 2u : 0u);
        {
            unsigned density = 1u << rnd(5);    /* 1 in 1..16 bytes non-zero */
            for (k = 20; k < 56; k++) r[k] = rnd(density) ? 0 : (u8)rnd(rnd(6) ? 3 : 40);
        }
    }
    for (k = 0; k < 9; k++) {
        int v = (int)rnd(k == 8 ? 200 : 1000);
        if (rnd(10) == 0) v = 125 + (int)rnd(40);           /* around the thresholds */
        if (rnd(40) == 0) v = -(int)rnd(20);
        para[2 * k] = (s16)v;                               /* 16.16: the high half */
    }
    byouki_kind = rnd(3) ? (u8)(rnd(16) << 4) : (u8)rnd(256);
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "";
    if (!strcmp(mode, "random") && argc >= 4) {
        int n = atoi(argv[2]), i, bad = 0, leaders = 0, shiori = 0;
        g_rng = strtoul(argv[3], NULL, 0);
        if (map_ram(argc > 4 ? argv[4] : "pc/assets/raw/PSX.EXE")) return 2;
        for (i = 0; i < n; i++) {
            u8 out[16];
            s16 rank[16];
            int g;
            randomise();
            if (compare(bad < 5)) bad++;
            get_grp(out, rank);
            for (g = 1; g < 11; g++) if (rank[g] >= 500) { leaders++; break; }
            if (rank[0] == 785) shiori++;
        }
        /* the sample reaches the interesting cases */
        printf("states %d with a girl >= 500: %d, Shiori 785: %d\n", n, leaders, shiori);
        printf("mismatches %d of %d\n", bad, n);
        return bad ? 1 : 0;
    }
    if (!strcmp(mode, "image") && argc >= 3) {
        static GsvState s;
        static char text[16384];
        FILE *f;
        int bad;
        if (map_ram(argc > 3 ? argv[3] : "pc/assets/raw/PSX.EXE")) return 2;
        f = fopen(argv[2], "rb");
        if (!f || fread((void *)RAM_BASE, 1, RAM_SIZE, f) != RAM_SIZE) {
            fprintf(stderr, "%s: not a 2 MB RAM image\n", argv[2]);
            return 2;
        }
        fclose(f);
        bad = compare(1);
        gsv_read(&s);
        gsv_format(&s, text, sizeof text);
        fputs(text, stdout);
        printf("mismatches %d\n", bad);
        return bad ? 1 : 0;
    }
    if (!strcmp(mode, "page") && argc >= 3) {
        static PcMenuView v;
        PcMenuCanvas cv;
        FILE *f;
        char hud[64];
        int tab;
        if (map_ram(argc > 3 ? argv[3] : "pc/assets/raw/PSX.EXE")) return 2;
        f = fopen(argv[2], "rb");
        if (!f || fread((void *)RAM_BASE, 1, RAM_SIZE, f) != RAM_SIZE) return 2;
        fclose(f);
        status_menu_register();
        if (status_page() <= 0) {
            printf("no page\n");
            return 3;
        }
        pc_menu_open_page(status_page());
        memset(&cv, 0, sizeof cv);
        cv.w = 284;
        cv.h = 168;
        cv.fill = cv_fill;
        cv.text = cv_text;
        for (tab = 0; tab < 4; tab++) {
            pc_menu_view(&v);
            if (!v.custom) return 4;
            printf("TAB %d %s\n", tab, v.title);
            v.custom->draw(v.custom->ctx, &cv);
            pc_menu_input(PC_MENU_RIGHT);
        }
        if (status_hud_text(hud, sizeof hud)) printf("HUD %s\n", hud);
        return 0;
    }
    if (!strcmp(mode, "names")) {
        int i;
        if (map_ram(argc > 2 ? argv[2] : "pc/assets/raw/PSX.EXE")) return 2;
        for (i = 0; i < 16; i++) {
            const char *p = gsv_girl_sjis(i);
            printf("girl%d ", i);
            while (p && *p) printf("%02x", (u8)*p++);
            printf("\n");
        }
        for (i = 0; i < 21; i++) {
            const char *p = gsv_place_sjis(i);
            printf("place%d ", i);
            while (p && *p) printf("%02x", (u8)*p++);
            printf("\n");
        }
        return 0;
    }
    fprintf(stderr, "usage: %s random N SEED [EXE] | image RAM [EXE] | names [EXE]\n", argv[0]);
    return 2;
}
