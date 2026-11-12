/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The SPU core's enhanced-quality patch (pc/tools/test_audio_quality.py,
 * CoreTests).
 *
 * Links pc/hal/spu_psx.c and an SPU core archive.  argv[1] = configuration,
 * argv[2] = output file (the output frames, the whole SPU RAM, the readable
 * device registers: ADSR current volumes, ENDX, SPUSTAT).
 *
 *   tone MODE    one voice playing a 4-sample-period ADPCM sine (a quarter
 *                of its rate: where the Gaussian takes the most) at pitch
 *                0x0C00 (8,269 Hz, fractional positions), no reverb;
 *                MODE = gaussian | cubic | sinc
 *   high MODE    a square of the same period at pitch 0x2800 (13,781 Hz,
 *                read 2.5x faster than the output rate: its third harmonic
 *                folds back to 2,756 Hz unless the filter follows the pitch)
 *   reverb HQ    two voices with the game's Studio Medium reverb, keyed off
 *                at a third of the run (the tail is most of it); HQ = 0 | 1
 *   inject WHAT  the split on (voice 1 masked, replace) with host frames
 *                injected: none | dry | wet; or the split off: `nosplit`
 *                (dry frames: ignored), `plain` (no frames)
 *   prepare      mode 2 selected before spu_psx_prepare_quality: cubic
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "spu_psx.h"

#define FRAMES 22050
#define REVERB_BASE 0x7B7C0u

static const u16 studio_medium[32] = {
    0x00B1, 0x007F, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xB4C0,
    0x5280, 0x4EC0, 0x0904, 0x076B, 0x0824, 0x065F, 0x07A2, 0x0616,
    0x076C, 0x05ED, 0x05EC, 0x042E, 0x050F, 0x0305, 0x0462, 0x02B7,
    0x042F, 0x0265, 0x0264, 0x01B2, 0x0100, 0x0080, 0x8000, 0x8000
};

static void w(u32 off, u16 v) { spu_psx_write_reg(0x1F801C00u + off, v); }

/* A looping wave of period 8 samples (filter 0, shift 0: nibble << 12):
 * a sine, or a square (its third harmonic is 3/8 of the source rate). */
static void load_wave(u32 addr, int square) {
    static const int sine[8] = { 0, 7, 0, -7, 0, 7, 0, -7 }, sq[8] = { 7, 7, 7, 7, -7, -7, -7, -7 };
    const int *wave = square ? sq : sine;
    u8 b[16 * 4];
    int blk, i;
    memset(b, 0, sizeof b);
    for (blk = 0; blk < 4; blk++) {
        u8 *p = b + blk * 16;
        p[0] = 0x00;
        p[1] = blk == 0 ? 0x04 : blk == 3 ? 0x03 : 0x00;
        for (i = 0; i < 28; i++) {
            int n = wave[(blk * 28 + i) % 8] & 0xF;
            p[2 + i / 2] |= (u8)(n << ((i & 1) * 4));
        }
    }
    spu_psx_dma_write(b, addr, sizeof b);
}

static void load_samples(void) {
    u8 b[64];
    int i;
    memset(b, 0, sizeof b);
    b[1] = 0x04;
    for (i = 2; i < 16; i++) b[i] = (u8)(0x17 + i * 13);
    b[17] = 0x03;
    for (i = 18; i < 32; i++) b[i] = (u8)(0x71 - i * 7);
    spu_psx_dma_write(b, 0x2000, 32);
    for (i = 2; i < 16; i++) b[i] = (u8)(0x52 + i * 5);
    for (i = 18; i < 32; i++) b[i] = (u8)(0x25 + i * 11);
    spu_psx_dma_write(b, 0x3000, 32);
}

static void voice(int v, u32 addr, u16 pitch, u16 vol) {
    u32 o = (u32)v * 0x10;
    w(o + 0x00, vol);
    w(o + 0x02, vol);
    w(o + 0x04, pitch);
    w(o + 0x06, (u16)(addr >> 3));
    w(o + 0x08, 0x00FF);
    w(o + 0x0A, 0x0000);
}

static int mode_of(const char *s) {
    return !strcmp(s, "cubic") ? 1 : !strcmp(s, "sinc") ? 2 : 0;
}

int main(int argc, char **argv) {
    static s16 out[FRAMES * 2], music[FRAMES * 2];
    static s32 inj[FRAMES * 4];
    static u8 ram[0x80000];
    const char *cfg, *arg;
    int i, split = 0, inject = 0;
    FILE *f;
    if (argc != 4) return 90;
    cfg = argv[1];
    arg = argv[2];
    spu_psx_init();
    w(0x1AA, 0xC080);
    w(0x180, 0x3FFF);
    w(0x182, 0x3FFF);
    if (!strcmp(cfg, "tone") || !strcmp(cfg, "high")) {
        int m = mode_of(arg);
        load_wave(0x4000, !strcmp(cfg, "high"));
        voice(0, 0x4000, !strcmp(cfg, "tone") ? 0x0C00 : 0x2800, 0x3000);
        if (m == 2) spu_psx_prepare_quality(2);
        spu_psx_set_quality(m, 0);
        w(0x188, 1);
    } else if (!strcmp(cfg, "prepare")) {
        load_wave(0x4000, 0);
        voice(0, 0x4000, 0x0C00, 0x3000);
        spu_psx_set_quality(2, 0);               /* no tables yet */
        w(0x188, 1);
    } else if (!strcmp(cfg, "reverb") || !strcmp(cfg, "inject")) {
        load_samples();
        for (i = 0; i < 32; i++) w(0x1C0 + 2 * i, studio_medium[i]);
        w(0x1A2, REVERB_BASE >> 3);
        w(0x184, 0x3C78);
        w(0x186, 0x3C78);
        w(0x198, 3);
        voice(0, 0x2000, 0x0800, 0x0C00);
        voice(1, 0x3000, 0x1300, 0x0900);
        if (!strcmp(cfg, "reverb")) spu_psx_set_quality(0, atoi(arg));
        if (!strcmp(cfg, "inject")) {
            split = strcmp(arg, "nosplit") && strcmp(arg, "plain");
            inject = strcmp(arg, "none") && strcmp(arg, "plain");
            if (split) spu_psx_music_split(1, 2, 1);
            for (i = 0; i < FRAMES; i++) {
                s32 v = (s32)(4000.0 * sin(2.0 * 3.14159265358979 * 441.0 * i / 44100.0));
                int wet = !strcmp(arg, "wet");
                inj[i * 4 + 0] = wet ? 0 : v;
                inj[i * 4 + 1] = wet ? 0 : -v;
                inj[i * 4 + 2] = wet ? v : 0;
                inj[i * 4 + 3] = wet ? -v : 0;
            }
        }
        w(0x188, 3);
    } else {
        return 91;
    }
    for (i = 0; i < FRAMES; i += 735) {
        int n = FRAMES - i < 735 ? FRAMES - i : 735;
        if (!strcmp(cfg, "reverb") && i == 735 * 10) w(0x18C, 3);
        if (inject) spu_psx_inject(inj + (size_t)i * 4);
        spu_psx_mix_split(out + i * 2, music + i * 2, n);
        spu_psx_inject(NULL);
    }
    spu_psx_dma_read(ram, 0, sizeof ram);
    f = fopen(argv[3], "wb");
    if (!f) return 92;
    fwrite(out, sizeof out, 1, f);
    fwrite(ram, sizeof ram, 1, f);
    for (i = 0; i < 24; i++) {
        u16 v = spu_psx_read_reg(0x1F801C0C + 0x10 * i);
        fwrite(&v, 2, 1, f);
    }
    {
        static const u32 regs[] = { 0x1F801D9C, 0x1F801D9E, 0x1F801DAE };
        for (i = 0; i < 3; i++) {
            u16 v = spu_psx_read_reg(regs[i]);
            fwrite(&v, 2, 1, f);
        }
    }
    fclose(f);
    return 0;
}
