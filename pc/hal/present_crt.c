/* SPDX-License-Identifier: GPL-3.0-or-later */
/* present_crt.c -- the CRT filter's presets and math (pc/include/pc_crt.h).
 *
 * SDL-free and stateless: pc/hal/present.c turns these numbers into
 * textures and draw passes, pc/tests/test_crt.c checks them.  Nothing here
 * sees emulated state; the inputs are the finished picture's pixels.
 */
#include "pc_crt.h"

#include <math.h>
#include <stddef.h>

/*                          name        scan  pow  mask                  depth comp glow  vig  gamma */
static const PcCrtParams g_presets[PC_CRT_COUNT] = {
    { "off",      0.0,  2.0, PC_CRT_MASK_NONE,     0.0,  0.0, 0.0,  0.0,  1.0 },
    /* soft scanlines only: the look of a good consumer set */
    { "lite",     0.45, 2.0, PC_CRT_MASK_NONE,     0.0,  0.85, 0.06, 0.0,  1.0 },
    /* Trinitron-style vertical RGB stripes, crisper lines, a little glow */
    { "aperture", 0.65, 2.5, PC_CRT_MASK_APERTURE, 0.30, 1.0, 0.10, 0.12, 0.92 },
    /* slot mask (staggered RGB triads), rounder beam, more glow */
    { "shadow",   0.60, 2.0, PC_CRT_MASK_SHADOW,   0.35, 1.0, 0.14, 0.20, 0.90 },
};

const PcCrtParams *pc_crt_preset(int mode) {
    if (mode <= PC_CRT_OFF || mode >= PC_CRT_COUNT) return NULL;
    return &g_presets[mode];
}

double pc_crt_scan_factor(const PcCrtParams *p, int y, int h, int src_rows) {
    double n, fc, sum = 0.0;
    int k;
    if (!p || h <= 0 || src_rows <= 0 || p->scan <= 0.0) return 1.0;
    n = (double)h / src_rows;           /* output rows per picture row */
    /* the beam centre: the middle output row of a picture row, so an
     * integer scale puts it exactly on one output row (at 2x: the second
     * row lit, the first dark) */
    fc = (floor(n / 2.0) + 0.5) / n;
    if (fc >= 1.0) fc = 0.5;
    for (k = 0; k < 4; k++) {
        double t = (y + (k + 0.5) / 4.0) * src_rows / h;
        double f = t - floor(t), d = fabs(f - fc);
        if (d > 0.5) d = 1.0 - d;       /* the nearest beam (periodic) */
        sum += 1.0 - p->scan * pow(d / 0.5, p->scan_power);
    }
    return sum / 4.0;
}

double pc_crt_mask_factor(const PcCrtParams *p, int x, int y, int c) {
    double m = 1.0, off;
    if (!p || p->mask == PC_CRT_MASK_NONE || p->mask_depth <= 0.0) return 1.0;
    off = 1.0 - p->mask_depth;
    if (x % 3 != c) m = off;            /* each column is one phosphor stripe */
    if (p->mask == PC_CRT_MASK_SHADOW) {
        /* slot mask: every triad has a dark gap every 4 rows, staggered by
         * 2 rows on alternate triads (a brick pattern) */
        int tri = x / 3;
        if ((y + (tri & 1) * 2) % 4 == 3) m *= off;
    }
    return m;
}

static unsigned to8(double v) {
    long i = lround(v * 255.0);
    if (i < 0) i = 0;
    if (i > 255) i = 255;
    return (unsigned)i;
}

double pc_crt_build_mask(const PcCrtParams *p, int w, int h, int src_rows, unsigned int *out) {
    double total = 0.0;
    int x, y;
    if (w <= 0 || h <= 0) return 1.0;
    for (y = 0; y < h; y++) {
        double s = pc_crt_scan_factor(p, y, h, src_rows);
        unsigned period[6], sum6 = 0;
        int j;
        /* the mask repeats every 6 columns (two triads) */
        for (j = 0; j < 6; j++) {
            unsigned r = to8(s * pc_crt_mask_factor(p, j, y, 0));
            unsigned g = to8(s * pc_crt_mask_factor(p, j, y, 1));
            unsigned b = to8(s * pc_crt_mask_factor(p, j, y, 2));
            period[j] = (r << 24) | (g << 16) | (b << 8) | 0xFFu;
        }
        for (x = 0; x < w; x++) {
            unsigned v = period[x % 6];
            if (out) out[(size_t)y * w + x] = v;
            sum6 += (v >> 24) + ((v >> 16) & 0xFF) + ((v >> 8) & 0xFF);
        }
        total += sum6;
    }
    return total / (255.0 * 3.0 * (double)w * h);
}

double pc_crt_boost(const PcCrtParams *p, double mean) {
    double k;
    if (!p) return 0.0;
    /* give back `comp` of what the mask removed; the glow already adds a
     * little light of its own, so half of it is taken off */
    k = p->comp * (1.0 - mean) - p->glow * 0.5;
    if (k < 0.0) k = 0.0;
    if (k > 1.0) k = 1.0;
    return k;
}

void pc_crt_gamma_lut(double gamma, unsigned char lut[256]) {
    int i;
    for (i = 0; i < 256; i++) {
        double v = gamma == 1.0 ? i / 255.0 : pow(i / 255.0, gamma);
        lut[i] = (unsigned char)to8(v);
    }
}

void pc_crt_glow(const unsigned int *src, int w, int h, int stride,
                 unsigned int *out, int gw, int gh) {
    int x, y, c;
    /* box average into out, kept as three planes of sums in a scratch
     * pass: small (the glow is ~80x60), so a fixed buffer is enough */
    static double acc[3][256 * 192];
    if (gw < 1 || gh < 1 || gw > 256 || gh > 192 || w < 1 || h < 1) return;
    for (y = 0; y < gh; y++) {
        int y0 = y * h / gh, y1 = (y + 1) * h / gh;
        if (y1 <= y0) y1 = y0 + 1;
        for (x = 0; x < gw; x++) {
            int x0 = x * w / gw, x1 = (x + 1) * w / gw, sx, sy, n = 0, stx, sty;
            double s[3] = { 0, 0, 0 };
            if (x1 <= x0) x1 = x0 + 1;
            /* at most 4 x 4 samples a cell: an N x picture costs what the
             * 1x one does (the 3x3 blur below hides the difference) */
            stx = (x1 - x0) > 4 ? (x1 - x0) / 4 : 1;
            sty = (y1 - y0) > 4 ? (y1 - y0) / 4 : 1;
            for (sy = y0; sy < y1; sy += sty)
                for (sx = x0; sx < x1; sx += stx) {
                    unsigned int px = src[(size_t)sy * stride + sx];
                    s[0] += (px >> 24) & 0xFF;
                    s[1] += (px >> 16) & 0xFF;
                    s[2] += (px >> 8) & 0xFF;
                    n++;
                }
            for (c = 0; c < 3; c++) acc[c][y * gw + x] = s[c] / n;
        }
    }
    for (y = 0; y < gh; y++)
        for (x = 0; x < gw; x++) {
            double s[3] = { 0, 0, 0 };
            int dx, dy;
            unsigned v[3];
            for (dy = -1; dy <= 1; dy++)
                for (dx = -1; dx <= 1; dx++) {
                    int xx = x + dx, yy = y + dy;
                    if (xx < 0) xx = 0;
                    if (xx >= gw) xx = gw - 1;
                    if (yy < 0) yy = 0;
                    if (yy >= gh) yy = gh - 1;
                    for (c = 0; c < 3; c++) s[c] += acc[c][yy * gw + xx];
                }
            for (c = 0; c < 3; c++) v[c] = to8(s[c] / 9.0 / 255.0);
            out[(size_t)y * gw + x] = (v[0] << 24) | (v[1] << 16) | (v[2] << 8) | 0xFFu;
        }
}

void pc_crt_warp(double u, double v, double curve, double *x, double *y) {
    double cx = u * 2.0 - 1.0, cy = v * 2.0 - 1.0;
    /* the middle of each edge stays put; the further along an edge, the
     * more it is pulled in: convex edges, rounded corners */
    double wx = cx * (1.0 - curve * cy * cy);
    double wy = cy * (1.0 - curve * cx * cx);
    *x = (wx + 1.0) / 2.0;
    *y = (wy + 1.0) / 2.0;
}

double pc_crt_vignette(const PcCrtParams *p, double u, double v) {
    double dx = u - 0.5, dy = v - 0.5, r2;
    if (!p || p->vignette <= 0.0) return 1.0;
    r2 = (dx * dx + dy * dy) / 0.5;     /* 0 centre .. 1 corners */
    return 1.0 - p->vignette * r2 * r2;
}

int pc_crt_reference(int pic, int mask, int glow, double boost, double glow_k, double vig) {
    int b = (int)to8(boost), g = (int)to8(glow_k), v = (int)to8(vig);
    int d = pic;                                    /* 1. picture */
    d = d * mask / 255;                             /* 2. x MASK */
    d += pic * b / 255;                             /* 3. + BOOST */
    if (d > 255) d = 255;
    d += glow * g / 255;                            /* 4. + GLOW */
    if (d > 255) d = 255;
    d = d * v / 255;                                /* 5. x VIGNETTE */
    return d;
}
