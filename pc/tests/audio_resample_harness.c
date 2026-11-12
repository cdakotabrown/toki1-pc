/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The 44.1 -> 48 kHz device resampler (pc/hal/audio_resample.c;
 * pc/tools/test_audio_quality.py, ResampleTests).
 *
 *   audio_resample_harness HZ OUT [linear]
 * feeds a 44.1 kHz stereo sine of HZ (amplitude 16384; the right channel
 * inverted) through audio_rs_run in odd-sized requests (the device's
 * 441/480/1024/... frames), and writes 2 s of 48 kHz frames to OUT, plus a
 * line "pulled N out M".  `linear` (the negative control) resamples by
 * linear interpolation instead. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "audio_resample.h"

static double g_hz;
static long long g_n;

static void pull(void *ctx, s16 *in, int frames) {
    int i;
    (void)ctx;
    for (i = 0; i < frames; i++, g_n++) {
        double v = 16384.0 * sin(2.0 * 3.14159265358979323846 * g_hz * (double)g_n / 44100.0);
        in[i * 2] = (s16)lrint(v);
        in[i * 2 + 1] = (s16)lrint(-v);
    }
}

int main(int argc, char **argv) {
    static const int sizes[] = { 441, 480, 1024, 960, 512, 7 };
    static s16 out[48000 * 2 * 2 + 16];
    int total = 48000 * 2, done = 0, k = 0, linear;
    FILE *f;
    if (argc < 3) return 90;
    g_hz = atof(argv[1]);
    linear = argc > 3 && !strcmp(argv[3], "linear");
    if (!linear) {
        audio_rs_reset();
        while (done < total) {
            int n = sizes[k++ % 6];
            if (n > total - done) n = total - done;
            audio_rs_run(out + done * 2, n, pull, NULL);
            done += n;
        }
    } else {
        int j;
        for (j = 0; j < total; j++) {
            double t = j * 44100.0 / 48000.0, fr = t - floor(t);
            double a = 16384.0 * sin(2.0 * 3.14159265358979323846 * g_hz * floor(t) / 44100.0);
            double b = 16384.0 * sin(2.0 * 3.14159265358979323846 * g_hz * (floor(t) + 1) / 44100.0);
            double v = a + (b - a) * fr;
            out[j * 2] = (s16)lrint(v);
            out[j * 2 + 1] = (s16)lrint(-v);
        }
        g_n = (long long)(total * 44100.0 / 48000.0);
    }
    f = fopen(argv[2], "wb");
    if (!f) return 91;
    fwrite(out, 4, (size_t)total, f);
    fclose(f);
    printf("pulled %lld out %d\n", g_n, total);
    return 0;
}
