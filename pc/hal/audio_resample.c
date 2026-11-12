/* SPDX-License-Identifier: GPL-3.0-or-later */
/* audio_resample.c -- 44.1 kHz -> 48 kHz polyphase resampler
 * (audio_resample.h). */
#include "audio_resample.h"
#include <math.h>
#include <string.h>

#define L 160                 /* output / input = 160 / 147 */
#define M 147
#define TAPS 64
#define HALF (TAPS / 2)
#define HIST_MAX (AUDIO_RS_MAX_OUT + 2 * TAPS + 16)

static float g_h[L][TAPS];
static int g_ready;
/* input history: g_in[i] is input frame (g_base + i) */
static float g_in[HIST_MAX][2];
static long long g_base;      /* absolute index of g_in[0] */
static int g_count;           /* frames held */
static unsigned long long g_pos;   /* the next output, in 1/L input frames */

static double bessel_i0(double x) {
    double sum = 1.0, term = 1.0;
    int k;
    for (k = 1; k < 50; k++) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
    }
    return sum;
}

static void design(void) {
    const double PI = 3.14159265358979323846, beta = 8.5, fc = 20250.0 / AUDIO_RS_IN_RATE;
    double i0b = bessel_i0(beta);
    int p, k;
    for (p = 0; p < L; p++) {
        double sum = 0.0, h[TAPS];
        for (k = 0; k < TAPS; k++) {
            /* tap k is input frame i0 - (HALF - 1) + k; the output sits at i0 + p / L */
            double d = (double)(HALF - 1 - k) + (double)p / L;
            double r = d / HALF, w, x;
            w = (r <= -1.0 || r >= 1.0) ? 0.0 : bessel_i0(beta * sqrt(1.0 - r * r)) / i0b;
            x = 2.0 * fc * d;
            h[k] = 2.0 * fc * (d == 0.0 ? 1.0 : sin(PI * x) / (PI * x)) * w;
            sum += h[k];
        }
        for (k = 0; k < TAPS; k++) g_h[p][k] = (float)(h[k] / sum);
    }
    g_ready = 1;
}

void audio_rs_reset(void) {
    if (!g_ready) design();
    memset(g_in, 0, sizeof g_in);
    g_base = -(HALF - 1);     /* HALF - 1 frames of silence before the first input */
    g_count = HALF - 1;
    g_pos = 0;
}

/* the input frames up to (and including) absolute index `last` */
static long long last_needed(int out_frames) {
    unsigned long long p = g_pos + (unsigned long long)(out_frames - 1) * M;
    return (long long)(p / L) + HALF;
}

int audio_rs_needed(int out_frames) {
    long long have_to;
    if (!g_ready) audio_rs_reset();
    if (out_frames <= 0) return 0;
    have_to = g_base + g_count - 1;
    return last_needed(out_frames) > have_to ? (int)(last_needed(out_frames) - have_to) : 0;
}

void audio_rs_run(s16 *out, int out_frames, AudioRsPull pull, void *ctx) {
    static s16 tmp[HIST_MAX * 2];
    int need, i, j, drop;
    if (!g_ready) audio_rs_reset();
    if (out_frames > AUDIO_RS_MAX_OUT) out_frames = AUDIO_RS_MAX_OUT;
    need = audio_rs_needed(out_frames);
    if (need > 0) {
        if (g_count + need > HIST_MAX) need = HIST_MAX - g_count;
        pull(ctx, tmp, need);
        for (i = 0; i < need; i++) {
            g_in[g_count + i][0] = tmp[i * 2];
            g_in[g_count + i][1] = tmp[i * 2 + 1];
        }
        g_count += need;
    }
    for (j = 0; j < out_frames; j++) {
        long long i0 = (long long)(g_pos / L);
        int p = (int)(g_pos % L), k, start = (int)(i0 - (HALF - 1) - g_base);
        const float *h = g_h[p];
        float l = 0.0f, r = 0.0f;
        for (k = 0; k < TAPS; k++) {
            int idx = start + k;
            if (idx >= 0 && idx < g_count) {
                l += h[k] * g_in[idx][0];
                r += h[k] * g_in[idx][1];
            }
        }
        l = floorf(l + 0.5f);
        r = floorf(r + 0.5f);
        out[j * 2] = (s16)(l > 32767.0f ? 32767 : l < -32768.0f ? -32768 : (int)l);
        out[j * 2 + 1] = (s16)(r > 32767.0f ? 32767 : r < -32768.0f ? -32768 : (int)r);
        g_pos += M;
    }
    /* keep the frames the next output still reads */
    drop = (int)((long long)(g_pos / L) - (HALF - 1) - g_base);
    if (drop > 0) {
        if (drop > g_count) drop = g_count;
        memmove(g_in, g_in + drop, (size_t)(g_count - drop) * sizeof g_in[0]);
        g_count -= drop;
        g_base += drop;
    }
}
