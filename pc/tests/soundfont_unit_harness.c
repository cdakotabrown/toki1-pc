/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SoundFont mode's mapping (pc/hal/soundfont.c; pc/tools/test_soundfont.py,
 * MapTests / HeuristicTests).  Includes soundfont.c itself (its statics are
 * what is checked), the SPU core for SPU RAM.
 *
 *   map FILE           parse FILE as <name>.map; print what a few lookups
 *                      give ("seq/vab prog key -> kind bank preset value")
 *   classify CASE      build a synthetic VAB program and sample, print the
 *                      heuristic's choice "bank:preset drum why"
 *   drum CASE          a percussion sample: "gm-key why"
 *   probe SF2          load an SF2 the way the port does: "presets N"
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "../hal/soundfont.c"

/* the libsnd state soundfont.c reads (unused here) */
u8 _svm_voice[24 * 0x18];
u32 _svm_vab_vh[16], _svm_vab_tn[16];
u8 _svm_vab_used[16];
void spu_hal_audio_lock(void) {}
void spu_hal_audio_unlock(void) {}
int pc_menu_add(const PcMenuItem *item) { (void)item; return 0; }

static void print_entry(const char *seq, const char *vab, int prog, int kind, int key) {
    const MapEntry *e = map_find(seq, vab, prog, kind, key);
    if (!e) { printf("%s %s %d %d -> none\n", seq ? seq : "-", vab, prog, key); return; }
    printf("%s %s %d %d -> %d %d %d %.3f %s\n", seq ? seq : "-", vab, prog, key, e->kind, e->bank, e->preset,
           e->value, e->scope);
}

/* ---- synthetic samples (ADPCM, filter 0) at SPU address `addr` -------- */
static unsigned g_rng = 12345;
static int rnd(void) { g_rng = g_rng * 1103515245u + 12345u; return (int)((g_rng >> 16) & 0xF); }

/* `kind`: 0 sine (period 32 source samples), 1 square, 2 noise, 3 pulse;
 * loop: repeat flag on the last block */
static void put_sample(u32 addr, int kind, int blocks, int loop, int decay) {
    static u8 buf[16 * 4096];
    int b, i, n = 0;
    memset(buf, 0, sizeof buf);
    for (b = 0; b < blocks; b++) {
        u8 *p = buf + b * 16;
        int shift = decay ? (b * 12 / blocks) : 0;
        p[0] = (u8)shift;
        p[1] = b == 0 && loop ? 0x04 : b == blocks - 1 ? (loop ? 0x03 : 0x01) : 0x00;
        for (i = 0; i < 28; i++, n++) {
            int v;
            if (kind == 0) v = (int)lrint(7.0 * sin(2 * 3.14159265358979 * n / 32.0));
            else if (kind == 1) v = (n / 16) % 2 ? 6 : -6;
            else if (kind == 3) v = n % 32 < 3 ? 7 : -1;          /* a narrow pulse: bright */
            else v = rnd() - 8;
            p[2 + i / 2] |= (u8)((v & 0xF) << ((i & 1) * 4));
        }
    }
    spu_psx_dma_write(buf, addr, (u32)blocks * 16);
}

static void tone(u8 *t, int mn, int mx, int center, u16 adsr1, u16 adsr2, int vag) {
    memset(t, 0, 32);
    t[4] = (u8)center;
    t[6] = (u8)mn;
    t[7] = (u8)mx;
    t[16] = (u8)adsr1; t[17] = (u8)(adsr1 >> 8);
    t[18] = (u8)adsr2; t[19] = (u8)(adsr2 >> 8);
    t[22] = (u8)vag; t[23] = 0;
}

int main(int argc, char **argv) {
    if (argc < 3) return 90;
    spu_psx_init();
    if (!strcmp(argv[1], "map")) {
        map_load(argv[2]);
        printf("entries %d gain %.3f velocity %d\n", g_map_n, g_gain_global, g_velocity);
        print_entry(NULL, "vab-00000001", 3, M_PRESET, -1);
        print_entry("seq-0000000a", "vab-00000001", 3, M_PRESET, -1);
        print_entry("seq-0000000b", "vab-00000001", 3, M_PRESET, -1);
        print_entry(NULL, "vab-00000001", 9, M_PRESET, -1);
        print_entry(NULL, "vab-00000001", 9, M_KEY, 60);
        print_entry(NULL, "vab-00000001", 9, M_KEY, 61);
        print_entry(NULL, "vab-00000001", 4, M_PRESET, -1);
        print_entry(NULL, "vab-00000001", 3, M_TRANSPOSE, -1);
        print_entry(NULL, "vab-00000001", 3, M_GAIN, -1);
        print_entry(NULL, "vab-00000001", 3, M_VELOCITY, -1);
        print_entry(NULL, "vab-00000002", 3, M_PRESET, -1);
        return 0;
    }
    if (!strcmp(argv[1], "probe")) {
        tsf *f = tsf_load_filename(argv[2]);
        printf("presets %d\n", f ? tsf_get_presetcount(f) : -1);
        return f ? 0 : 1;
    }
    {
        static u8 tones[16 * 32];
        u8 prog[16];
        const char *c = argv[2];
        int n = 1, i;
        u16 pitch = 0x1000;
        SampleFeat sf;
        memset(prog, 0, sizeof prog);
        /* the keyed sample at 0x10000 */
        if (!strcmp(c, "kit")) {
            for (i = 0; i < 8; i++) tone(tones + i * 32, 36 + i, 36 + i, 60, 0x00FF, 0x5FC0, i + 1);
            n = 8;
            put_sample(0x10000, 2, 20, 0, 1);
        } else if (!strcmp(c, "multisample")) {
            for (i = 0; i < 11; i++) tone(tones + i * 32, 60 + 2 * i, 61 + 2 * i, 72, 0x80FF, 0xDFEF, 1 + i / 4);
            n = 11;
            put_sample(0x10000, 0, 1600, 0, 1);
        } else if (!strcmp(c, "snare")) {
            put_sample(0x10000, 2, 8, 0, 1);
            tone(tones, 0, 127, 60, 0x00FF, 0x5FC0, 1);
        } else if (!strcmp(c, "strings")) {
            put_sample(0x10000, 0, 8, 1, 0);
            tone(tones, 36, 96, 60, (u16)(0x3C00 | 0x0F), 0x1FC0, 1);    /* slow attack, sustain */
        } else if (!strcmp(c, "flute")) {
            put_sample(0x10000, 0, 8, 1, 0);
            tone(tones, 60, 96, 72, 0x00FF, 0x1FC0, 1);
        } else if (!strcmp(c, "reed")) {
            put_sample(0x10000, 1, 8, 1, 0);
            tone(tones, 48, 84, 60, 0x00FF, 0x1FC0, 1);
        } else if (!strcmp(c, "brass")) {
            put_sample(0x10000, 3, 8, 1, 0);
            tone(tones, 48, 84, 60, 0x00FF, 0x1FC0, 1);
        } else if (!strcmp(c, "hihat")) {
            put_sample(0x10000, 2, 8, 0, 1);
            tone(tones, 0, 127, 60, 0x00FF, 0x5FC0, 1);
        } else if (!strcmp(c, "kick")) {
            put_sample(0x10000, 0, 40, 0, 1);
            tone(tones, 0, 127, 60, 0x00FF, 0x5FC0, 1);
            pitch = 0x0100;
        } else if (!strcmp(c, "bass")) {
            put_sample(0x10000, 1, 8, 1, 0);
            tone(tones, 24, 55, 36, 0x00FF, 0x1FC0, 1);
        } else if (!strcmp(c, "piano")) {
            put_sample(0x10000, 0, 1600, 0, 1);
            tone(tones, 21, 108, 60, 0x00FF, 0xDFEF, 1);
            pitch = 0x0800;
        } else {
            return 91;
        }
        prog[0] = (u8)n;
        analyze(0x10000 >> 3, pitch, &sf);
        if (!strcmp(argv[1], "drum")) {
            char why[96];
            printf("%d %s\n", classify_drum(&sf, why, sizeof why), why);
        } else {
            ProgMap m = classify_program(prog, tones, n, &sf, tones);
            printf("%d:%d drum %d (%s) looped %d periodic %d dur %.2f zcr %.0f harm %.2f\n", m.bank, m.preset,
                   m.drum, m.why, sf.looped, sf.periodic, sf.dur, sf.zcr_hz, sf.harm);
        }
    }
    return 0;
}
