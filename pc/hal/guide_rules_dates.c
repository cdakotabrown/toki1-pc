/* SPDX-License-Identifier: GPL-3.0-or-later */
/* guide_rules_dates.c -- the guide's rules for phone calls, invitations and
 * dates (guide_model.h), transcribed from the matched C and read from the
 * player's game; checked against the game's functions by
 * pc/tests/guide_harness.c.  Evidence: docs/game-flows/MECHANICS-GUIDE.md.
 *
 * Overlay tables: an EXN is loaded at 0x80132000 with no header, so a
 * console address A of that overlay is byte A - 0x80132000 of
 * <assets>/raw/CDROM/EXEDIR/OV.EXN, the file the port's CD drive reads.
 * Read once and kept; never shipped.  Read only: no PS1 write.
 */
#include <stdio.h>
#include <stdlib.h>
#include "types.h"
#include "pc_data_root.h"
#include "guide_model.h"

extern s16 para_zat, para_you;
extern u8 girl_tbl[];
extern u16 basyo_pack;
extern u32 album_work;
extern s8 date_sasoi_judge_tbl0;
extern u8 now_year, now_month, now_day;
extern u32 reserved_date[8];

#define REC(g) (girl_tbl + 56 * (g))
#define S16(p) (*(const s16 *)(const void *)(p))
#define U32(p) (*(const u32 *)(const void *)(p))

int guide_exn_read(const char *ov, unsigned addr, void *buf, int n) {
    char path[600], rel[64];
    FILE *f;
    int got;
    snprintf(rel, sizeof rel, "raw/CDROM/EXEDIR/%s.EXN", ov);
    pc_asset_path(path, sizeof path, rel);
    f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, (long)(addr - 0x80132000u), SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    got = (int)fread(buf, 1, (size_t)n, f);
    fclose(f);
    return got == n;
}

static u8 g_tel_like[11 * 21], g_date_cell[11 * 21];
static s32 g_date_w[5];
static int g_tel_ok = -1, g_date_ok = -1;

/* tel_basyo_suki_tbl (TEL 0x801410C4): her liking of place P (0..20) when
 * you phone; -1 when TEL.EXN is not there */
int guide_tel_like(int g, int p) {
    if (g_tel_ok < 0) g_tel_ok = guide_exn_read("TEL", 0x801410C4u, g_tel_like, sizeof g_tel_like);
    if (!g_tel_ok || g < 0 || g > 10 || p < 0 || p > 20) return -1;
    return g_tel_like[g * 21 + p];
}

/* D_8015B780 (DATE 0x8015B780): the date grade of place P for her (0 the
 * best), and W = D_8015B76C (5 x s32) */
static int date_tables(void) {
    if (g_date_ok < 0)
        g_date_ok = guide_exn_read("DATE", 0x8015B780u, g_date_cell, sizeof g_date_cell) &&
                    guide_exn_read("DATE", 0x8015B76Cu, g_date_w, sizeof g_date_w);
    return g_date_ok;
}

int guide_date_cell(int g, int p) {
    if (!date_tables() || g < 0 || g > 10 || p < 0 || p > 20) return -1;
    return g_date_cell[g * 21 + p];
}

/* tel_dialog20 (TEL 0x80136620), step 6: she says yes when rand() & 0xFF
 * is below a = max(icon >> 2, trivia / 4) + liking (0 when the liking is
 * 0), less 40 - looks when looks < 40.  The chance is a / 256 (clamped). */
int guide_accept_a(int g, int p, int icon) {
    int a = icon >> 2, z = para_zat / 4, like = guide_tel_like(g, p);
    if (like < 0) return -1;
    if (z >= a) a = z;
    a = like ? a + like : 0;
    if (para_you < 40) a += para_you - 40;
    return a;
}

/* date_rank_grade_get (DATE 0x8013C3AC): A from trivia, B from the place's
 * grade; MODE > 0 for a gain, <= 0 for a loss */
void guide_date_ab_cell(int cell, int mode, int *A, int *B) {
    int i, z = para_zat;
    for (i = 0; i < 5; i++)
        if (z >= 100 - i * 25) {
            *A = mode > 0 ? 5 - i : i + 3;
            break;
        }
    if (z == 0) *A = mode > 0 ? 0 : 8;
    if (mode > 0) *B = cell == 0 ? 5 : cell == 1 || cell == 2 ? 4 : cell == 3 ? 3 : 2;
    else *B = cell == 0 ? 3 : cell == 1 || cell == 2 ? 4 : cell == 3 ? 5 : 6;
}

void guide_date_ab(int g, int p, int mode, int *A, int *B) {
    guide_date_ab_cell(guide_date_cell(g, p), mode, A, B);
}

/* date_score_apply (DATE 0x8013C12C): the 16.16 change of tokimeki
 * (WHICH 0), friendship (1) or bomb (2) for an answer of grade IMP
 * (0 best .. 4 worst) at a place of grade CELL */
int guide_date_delta_cell(int cell, int imp, int which) {
    int A = 0, B = 0, k;
    if (!date_tables() || imp < 0 || imp > 4) return 0;
    if (which == 0) {
        guide_date_ab_cell(cell, 3 - imp, &A, &B);
        return (((3 - imp) * A * B) << 16) / 16;
    }
    guide_date_ab_cell(cell, 2 - imp, &A, &B);
    k = which == 1 ? 2 - imp : g_date_w[imp];
    return ((k * A * B) << 16) / 16;
}

int guide_date_delta(int g, int p, int imp, int which) {
    return guide_date_delta_cell(guide_date_cell(g, p), imp, which);
}

/* date_sasoi_judge (0x80070910; KIND 0: she asks you out) and geko_judge
 * (0x80070754; KIND 1: she walks home with you) */
int guide_invite_day_ok(void) {
    if (reserved_date[0] >> 29 & 1) return 0;
    if ((now_day + 6) / 7 == 2 && (basyo_pack >> 12) < 11) return 0;
    if (now_month == 12 && (now_day == 17 || now_day == 24)) return 0;
    if (now_year == 98 && now_month >= 2) return 0;
    return 1;
}

/* the fixed tests: met, bomb under 60, mood not 4, icon 33+ / 17+ */
int guide_invite_ok(int kind, int g, int icon) {
    if (!(U32(REC(g) + 12) >> 1 & 1) || S16(REC(g) + 10) >= 60) return 0;
    if (guide_mood_s(gsv_yuukou_pct(g), gsv_tokimeki_pct(g)) == 4) return 0;
    return icon >= (kind ? 17 : 33);
}

int guide_invite_pct(int kind) { return kind ? 50 : 8; }

/* the girl the scan (from album_work % 11, the last picked skipped) takes
 * when every roll passes, or -1 */
int guide_invite_pick(int kind, const unsigned char icon[11]) {
    int i, g;
    if (!kind && !guide_invite_day_ok()) return -1;
    for (i = 0; i < 11; i++) {
        g = (int)((album_work % 11 + (u32)i) % 11);
        if (guide_invite_ok(kind, g, icon[g]) && date_sasoi_judge_tbl0 != g) return g;
    }
    return -1;
}

/* ---- the school calendar: CDROM/FUKUTEST/CALROM.NBN ----
 * season_load_step reads it to 0x801A0000; cal_font_load_tbl0 points at
 * its 36 month pointers (index 0 = April 1995 .. 35 = March 1998); each
 * month is an 8192-byte picture and then s32 {n, first weekday, kind[n],
 * event[n]} (cal_font_load copies the next month's into cal_day_kind).
 * Read once from the player's disc. */
static s32 g_cal_n[36], g_cal_first[36];
static u8 g_cal_kind[36][31], g_cal_ev[36][31];
static int g_cal_ok = -1;

static u32 le32(const unsigned char *q) {
    return (u32)q[0] | (u32)q[1] << 8 | (u32)q[2] << 16 | (u32)q[3] << 24;
}

static int cal_load(void) {
    char path[600];
    FILE *f;
    long size;
    unsigned char *buf = NULL;
    int k, i, ok = 1;
    if (g_cal_ok >= 0) return g_cal_ok;
    g_cal_ok = 0;
    pc_asset_path(path, sizeof path, "raw/CDROM/FUKUTEST/CALROM.NBN");
    f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 0 && size < (1L << 22)) buf = (unsigned char *)malloc((size_t)size);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) ok = 0;
    fclose(f);
    for (k = 0; ok && k < 36; k++) {
        u32 ptr = le32(buf + 4 * k);
        long o = (long)(ptr - 0x801A0000u) + 8192;
        const unsigned char *q;
        s32 n;
        if (ptr < 0x801A0000u || o < 0 || o + 8 > size) { ok = 0; break; }
        q = buf + o;
        n = (s32)le32(q);
        if (n < 28 || n > 31 || o + 8 + 8 * n > size) { ok = 0; break; }
        g_cal_n[k] = n;
        g_cal_first[k] = (s32)le32(q + 4);
        for (i = 0; i < n; i++) {
            g_cal_kind[k][i] = (u8)le32(q + 8 + 4 * i);
            g_cal_ev[k][i] = (u8)le32(q + 8 + 4 * (n + i));
        }
    }
    free(buf);
    g_cal_ok = ok;
    return ok;
}

/* month K (0 = April 1995): days, first weekday, kind and event of each
 * day; 0 when the file is not there */
int guide_cal_month(int k, int *n, int *first, unsigned char kind[31], unsigned char ev[31]) {
    int i;
    if (k < 0 || k > 35 || !cal_load()) return 0;
    *n = g_cal_n[k];
    *first = g_cal_first[k];
    for (i = 0; i < 31; i++) {
        kind[i] = i < g_cal_n[k] ? g_cal_kind[k][i] : 0;
        ev[i] = i < g_cal_n[k] ? g_cal_ev[k][i] : 0;
    }
    return 1;
}
