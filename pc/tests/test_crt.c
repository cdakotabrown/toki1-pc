/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_crt.c -- the presenter's CRT filter math and its settings keys
 * (pc/hal/present_crt.c, pc/include/pc_crt.h, pc/hal/settings.c).
 * Pure functions only: no SDL, no assets.  Each property carries a
 * negative control showing the check can fail. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pc_crt.h"
#include "pc_settings.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "  %s:%d: %s\n", __FILE__, __LINE__, #c); return __LINE__; } } while (0)

typedef double (*ScanFn)(const PcCrtParams *p, int y, int h, int src_rows);

/* Half a picture row too low: the negative control for the alignment. */
static double scan_misphased(const PcCrtParams *p, int y, int h, int src_rows) {
    return pc_crt_scan_factor(p, y + h / src_rows / 2, h, src_rows);
}

/* At an integer scale n >= 3, every picture row looks the same (period n),
 * holds one lit output row near full brightness, and its darkest output
 * row touches the boundary with the next picture row: the gap is BETWEEN
 * lines, never inside one.  Returns 0 when all of that holds. */
static int scan_aligned(ScanFn fn, const PcCrtParams *p, int n) {
    int h = 240 * n, line, k, dark = 0;
    double lo = 2.0, hi = -1.0;
    for (k = 0; k < n; k++) {
        double v = fn(p, k, h, 240);
        if (v < lo) { lo = v; dark = k; }
        if (v > hi) hi = v;
    }
    if (hi < 0.95) return 1;                    /* a lit row */
    if (lo > 1.0 - p->scan * 0.4 || hi - lo < 0.1) return 2;   /* a real gap */
    if (dark != 0 && dark != n - 1) return 3;   /* at the boundary */
    for (line = 1; line < 240; line += 37)
        for (k = 0; k < n; k++)
            if (fabs(fn(p, line * n + k, h, 240) - fn(p, k, h, 240)) > 1e-9) return 4;
    return 0;
}

int test_crt_presets_and_settings(void) {
    PcSettings s;
    char path[] = "/tmp/toki_crt_test_XXXXXX";
    char file[64], *text = NULL;
    const char *keys[2] = { "display.crt", "display.crt_curvature" };
    FILE *f;
    long n;
    int fd, i;
    CHECK(pc_crt_preset(PC_CRT_OFF) == NULL);
    CHECK(pc_crt_preset(PC_CRT_COUNT) == NULL && pc_crt_preset(-1) == NULL);
    for (i = PC_CRT_OFF + 1; i < PC_CRT_COUNT; i++) {
        const PcCrtParams *p = pc_crt_preset(i);
        CHECK(p && p->name && p->scan > 0.0 && p->scan < 1.0);
        CHECK(p->gamma > 0.5 && p->gamma <= 1.0 && p->vignette < 0.5 && p->glow < 0.3);
    }
    CHECK(!strcmp(pc_crt_preset(PC_CRT_LITE)->name, "lite"));
    CHECK(pc_crt_preset(PC_CRT_LITE)->mask == PC_CRT_MASK_NONE);
    CHECK(pc_crt_preset(PC_CRT_APERTURE)->mask == PC_CRT_MASK_APERTURE);
    CHECK(pc_crt_preset(PC_CRT_SHADOW)->mask == PC_CRT_MASK_SHADOW);

    /* default off; the ini values are the preset names */
    pc_settings_defaults(&s);
    CHECK(s.crt == PC_CRT_OFF && s.crt_curvature == 0);
    for (i = PC_CRT_OFF + 1; i < PC_CRT_COUNT; i++) {
        CHECK(pc_settings_set(&s, "display.crt", pc_crt_preset(i)->name) == 0 && s.crt == i);
    }
    CHECK(pc_settings_set(&s, "display.crt", "off") == 0 && s.crt == PC_CRT_OFF);
    /* negative control: an unknown name is refused and changes nothing */
    CHECK(pc_settings_set(&s, "display.crt", "trinitron") != 0 && s.crt == PC_CRT_OFF);
    CHECK(pc_settings_set(&s, "display.crt_curvature", "true") == 0 && s.crt_curvature == 1);

    /* round trip through a settings file, as the menu saves it */
    fd = mkstemp(path);
    CHECK(fd >= 0);
    close(fd);
    unlink(path);
    snprintf(file, sizeof file, "%s", path);
    f = fopen(file, "w");
    CHECK(f != NULL);
    fputs("# mine\n[display]\nscale = integer\n", f);
    fclose(f);
    pc_settings_defaults(&s);
    s.scale_mode = PC_SCALE_INTEGER;
    s.crt = PC_CRT_SHADOW;
    s.crt_curvature = 1;
    CHECK(pc_settings_save_keys(&s, file, keys, 2) == 0);
    f = fopen(file, "rb");
    CHECK(f != NULL);
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    text = (char *)calloc(1, (size_t)n + 1);
    if (fread(text, 1, (size_t)n, f) != (size_t)n) n = -1;
    fclose(f);
    unlink(file);
    CHECK(n > 0);
    CHECK(strstr(text, "# mine") != NULL);            /* the player's text kept */
    CHECK(strstr(text, "crt = shadow") != NULL);
    pc_settings_defaults(&s);
    CHECK(pc_settings_parse(&s, text, "test") == 0);
    free(text);
    CHECK(s.crt == PC_CRT_SHADOW && s.crt_curvature == 1 && s.scale_mode == PC_SCALE_INTEGER);
    return 0;
}

int test_crt_scanlines(void) {
    int i, n, y;
    for (i = PC_CRT_OFF + 1; i < PC_CRT_COUNT; i++) {
        const PcCrtParams *p = pc_crt_preset(i);
        for (n = 3; n <= 6; n++) {
            CHECK(scan_aligned(pc_crt_scan_factor, p, n) == 0);
            /* negative control: the same check fails on a misphased mask */
            CHECK(scan_aligned(scan_misphased, p, n) != 0);
        }
        /* 2x: one lit row, one dark row per line */
        CHECK(pc_crt_scan_factor(p, 1, 480, 240) > 0.9);
        CHECK(pc_crt_scan_factor(p, 0, 480, 240) < 1.0 - p->scan * 0.4);
        /* a non-integer scale (1080p aspect: 4.5x) still has a lit and a
         * dark row inside every line, and stays in range */
        for (y = 0; y < 1080; y++) {
            double v = pc_crt_scan_factor(p, y, 1080, 240);
            CHECK(v >= 1.0 - p->scan - 1e-9 && v <= 1.0 + 1e-9);
        }
        for (y = 0; y < 1080; y += 9) {
            double lo = 2, hi = -1;
            int k;
            for (k = 0; k < 9; k++) {
                double v = pc_crt_scan_factor(p, y + k, 1080, 240);
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
            CHECK(hi - lo > p->scan * 0.5);
        }
    }
    {
        /* negative control: no scanline depth, no lines */
        PcCrtParams flat = *pc_crt_preset(PC_CRT_LITE);
        flat.scan = 0.0;
        CHECK(scan_aligned(pc_crt_scan_factor, &flat, 4) == 1 ||
              scan_aligned(pc_crt_scan_factor, &flat, 4) == 2);
        for (y = 0; y < 16; y++) CHECK(pc_crt_scan_factor(&flat, y, 960, 240) == 1.0);
    }
    return 0;
}

int test_crt_mask_pattern(void) {
    const PcCrtParams *ap = pc_crt_preset(PC_CRT_APERTURE);
    const PcCrtParams *sh = pc_crt_preset(PC_CRT_SHADOW);
    const PcCrtParams *li = pc_crt_preset(PC_CRT_LITE);
    int x, y, c, w = 64, h = 960;
    unsigned int *m = (unsigned int *)malloc((size_t)w * h * 4);
    CHECK(m != NULL);
    /* aperture grille: column x shows phosphor x % 3 at full, the others
     * at 1 - depth, on every row */
    for (y = 0; y < 8; y++)
        for (x = 0; x < 6; x++)
            for (c = 0; c < 3; c++) {
                double v = pc_crt_mask_factor(ap, x, y, c);
                CHECK(fabs(v - (x % 3 == c ? 1.0 : 1.0 - ap->mask_depth)) < 1e-12);
            }
    /* slot mask: the same stripes, plus a gap row every 4 rows, staggered
     * by 2 rows between neighbouring triads */
    CHECK(pc_crt_mask_factor(sh, 0, 3, 0) < pc_crt_mask_factor(sh, 0, 2, 0));
    CHECK(pc_crt_mask_factor(sh, 3, 1, 0) < pc_crt_mask_factor(sh, 3, 3, 0));
    CHECK(pc_crt_mask_factor(sh, 0, 1, 0) == 1.0 && pc_crt_mask_factor(sh, 3, 3, 0) == 1.0);
    /* lite: no mask at all */
    for (x = 0; x < 6; x++)
        for (c = 0; c < 3; c++) CHECK(pc_crt_mask_factor(li, x, 5, c) == 1.0);
    /* the texture is scanline x mask, opaque */
    pc_crt_build_mask(ap, w, h, 240, m);
    for (y = 0; y < h; y += 7)
        for (x = 0; x < w; x += 5) {
            unsigned v = m[y * w + x];
            double s = pc_crt_scan_factor(ap, y, h, 240);
            CHECK((v & 0xFF) == 0xFF);
            for (c = 0; c < 3; c++) {
                double want = s * pc_crt_mask_factor(ap, x, y, c) * 255.0;
                CHECK(fabs((double)((v >> (24 - 8 * c)) & 0xFF) - want) <= 0.51);
            }
        }
    free(m);
    return 0;
}

/* The composite's mean on a flat field of `level`, over a w x h mask. */
static double flat_mean(const unsigned int *m, int w, int h, int level, double boost,
                        const PcCrtParams *p) {
    double sum = 0;
    int i, c;
    for (i = 0; i < w * h; i++)
        for (c = 0; c < 3; c++)
            sum += pc_crt_reference(level, (m[i] >> (24 - 8 * c)) & 0xFF, level, boost, p->glow, 1.0);
    return sum / (3.0 * w * h);
}

int test_crt_brightness_compensation(void) {
    int i, w = 96, h = 1080;            /* 1080p: 4.5 rows a line */
    unsigned int *m = (unsigned int *)malloc((size_t)w * h * 4);
    CHECK(m != NULL);
    for (i = PC_CRT_OFF + 1; i < PC_CRT_COUNT; i++) {
        const PcCrtParams *p = pc_crt_preset(i);
        double mean = pc_crt_build_mask(p, w, h, 240, m);
        double k = pc_crt_boost(p, mean);
        double white = flat_mean(m, w, h, 255, k, p);
        double grey = flat_mean(m, w, h, 128, k, p);
        double dark = flat_mean(m, w, h, 32, k, p);
        CHECK(mean > 0.3 && mean < 1.0);
        CHECK(k > 0.0 && k < 1.0);
        /* the light the mask took is given back: white stays white-ish
         * (the clamp keeps it under), mid and dark tones within 15% */
        CHECK(white >= 0.85 * 255 && white <= 255.0);
        CHECK(fabs(grey - 128) <= 0.15 * 128);
        CHECK(fabs(dark - 32) <= 0.15 * 32 + 1);
        /* negative control: without the boost the mask darkens (aperture /
         * shadow lose a quarter or more) */
        if (p->mask != PC_CRT_MASK_NONE) CHECK(flat_mean(m, w, h, 255, 0.0, p) < 0.80 * 255);
        CHECK(flat_mean(m, w, h, 128, 0.0, p) < grey - 5);
    }
    free(m);
    return 0;
}

int test_crt_reference_and_glow(void) {
    static unsigned int src[320 * 240];
    unsigned int out[80 * 60];
    unsigned char lut[256];
    int x, y, i;
    /* the passes: identity with a white mask, no boost, no glow */
    for (i = 0; i < 256; i += 15) CHECK(pc_crt_reference(i, 255, 200, 0.0, 0.0, 1.0) == i);
    CHECK(pc_crt_reference(200, 255, 0, 1.0, 0.0, 1.0) == 255);        /* clamps */
    CHECK(pc_crt_reference(200, 0, 0, 0.0, 0.0, 1.0) == 0);
    CHECK(pc_crt_reference(255, 255, 255, 0.0, 0.0, 0.5) == 128);      /* vignette */
    /* gamma: 1 is the identity; < 1 lifts mid-tones, ends fixed, monotone */
    pc_crt_gamma_lut(1.0, lut);
    for (i = 0; i < 256; i++) CHECK(lut[i] == i);
    pc_crt_gamma_lut(0.9, lut);
    CHECK(lut[0] == 0 && lut[255] == 255 && lut[128] > 128);
    for (i = 1; i < 256; i++) CHECK(lut[i] >= lut[i - 1]);
    /* glow: a flat picture glows flat */
    for (i = 0; i < 320 * 240; i++) src[i] = 0x408060FFu;
    pc_crt_glow(src, 320, 240, 320, out, 80, 60);
    for (i = 0; i < 80 * 60; i++) CHECK(out[i] == 0x408060FFu);
    /* one bright pixel block spreads into its neighbours, and only there */
    for (i = 0; i < 320 * 240; i++) src[i] = 0x000000FFu;
    for (y = 120; y < 124; y++)
        for (x = 160; x < 164; x++) src[y * 320 + x] = 0xFFFFFFFFu;
    pc_crt_glow(src, 320, 240, 320, out, 80, 60);
    CHECK((out[30 * 80 + 40] >> 24) > 0 && (out[30 * 80 + 39] >> 24) > 0);
    CHECK((out[31 * 80 + 41] >> 24) > 0);
    CHECK((out[30 * 80 + 40] >> 24) < 255);         /* blurred, not copied */
    CHECK((out[0] >> 24) == 0 && (out[30 * 80 + 43] >> 24) == 0);
    /* a stride wider than the picture is honoured (the N x picture) */
    {
        static unsigned int wide[100 * 40];
        for (i = 0; i < 100 * 40; i++) wide[i] = (i % 100) < 80 ? 0x202020FFu : 0xFFFFFFFFu;
        pc_crt_glow(wide, 80, 40, 100, out, 20, 10);
        for (i = 0; i < 20 * 10; i++) CHECK(out[i] == 0x202020FFu);
        /* negative control: reading it as packed picks up the white */
        pc_crt_glow(wide, 80, 40, 80, out, 20, 10);
        for (i = 0, x = 0; i < 20 * 10; i++) x |= out[i] != 0x202020FFu;
        CHECK(x);
    }
    return 0;
}

int test_crt_warp_and_vignette(void) {
    const PcCrtParams *sh = pc_crt_preset(PC_CRT_SHADOW);
    const PcCrtParams *li = pc_crt_preset(PC_CRT_LITE);
    double x, y, px;
    int i, j;
    /* flat: the identity (the mesh is the plain rectangle) */
    for (j = 0; j <= 18; j++)
        for (i = 0; i <= 24; i++) {
            pc_crt_warp(i / 24.0, j / 18.0, 0.0, &x, &y);
            CHECK(fabs(x - i / 24.0) < 1e-12 && fabs(y - j / 18.0) < 1e-12);
        }
    /* curved: centre and edge midpoints stay, corners come in, every row
     * stays monotone (no fold) and inside the rectangle */
    pc_crt_warp(0.5, 0.5, PC_CRT_CURVE, &x, &y);
    CHECK(fabs(x - 0.5) < 1e-12 && fabs(y - 0.5) < 1e-12);
    pc_crt_warp(0.0, 0.5, PC_CRT_CURVE, &x, &y);
    CHECK(fabs(x) < 1e-12 && fabs(y - 0.5) < 1e-12);
    pc_crt_warp(0.0, 0.0, PC_CRT_CURVE, &x, &y);
    CHECK(x > 0.01 && y > 0.01);
    for (j = 0; j <= 18; j++) {
        px = -1;
        for (i = 0; i <= 24; i++) {
            pc_crt_warp(i / 24.0, j / 18.0, PC_CRT_CURVE, &x, &y);
            CHECK(x > px && x >= 0.0 && x <= 1.0 && y >= 0.0 && y <= 1.0);
            px = x;
        }
    }
    /* vignette: 1 in the middle, darkest in the corners, none for lite */
    CHECK(pc_crt_vignette(sh, 0.5, 0.5) == 1.0);
    CHECK(fabs(pc_crt_vignette(sh, 0.0, 0.0) - (1.0 - sh->vignette)) < 1e-12);
    CHECK(pc_crt_vignette(sh, 0.25, 0.5) < 1.0 && pc_crt_vignette(sh, 0.25, 0.5) > pc_crt_vignette(sh, 0.0, 0.5));
    CHECK(pc_crt_vignette(li, 0.0, 0.0) == 1.0);
    return 0;
}
