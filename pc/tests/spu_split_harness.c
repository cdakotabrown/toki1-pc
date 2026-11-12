/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The SPU core's music split (pc/tools/test_music_pack.py, SpuSplitTests).
 *
 * Links pc/hal/spu_psx.c and the SPU core archive (SPU_ARCHIVE, default
 * pc/3rd/spu-core/spu_psx.a, pc/3rd/SPU-CORE.md).  Two looping voices with reverb (the
 * game's Studio Medium preset), keyed on, mixed for FRAMES frames in one
 * configuration (argv[1]):
 *   plain     no split
 *   zero      split on, empty mask, replace       (must equal plain)
 *   dump      split on, mask voice 1, no replace  (must equal plain; the
 *             music frames = plain - only0)
 *   replace   split on, mask voice 1, replace     (must equal only0)
 *   only0     no split, voice 1 never keyed
 * and writes argv[2]: the output frames, the music frames, the whole SPU
 * RAM and the device's readable registers (ADSR current volume, ENDX,
 * SPUSTAT).  The test compares the files: the split may change the output
 * and nothing else.
 */
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

/* Two looping ADPCM samples with different content. */
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

static void voice(int v, u32 addr, u16 pitch) {
    u32 o = (u32)v * 0x10;
    w(o + 0x00, 0x0C00);                    /* low enough that no sum clips */
    w(o + 0x02, 0x0900);
    w(o + 0x04, pitch);
    w(o + 0x06, (u16)(addr >> 3));
    w(o + 0x08, 0x00FF);
    w(o + 0x0A, 0x0000);
}

int main(int argc, char **argv) {
    static s16 out[FRAMES * 2], music[FRAMES * 2];
    static u8 ram[0x80000];
    const char *mode;
    int split = 0, replace = 0, key1 = 1, i;
    u32 mask = 0;
    FILE *f;
    if (argc != 3) return 90;
    mode = argv[1];
    if (!strcmp(mode, "plain")) ;
    else if (!strcmp(mode, "zero")) { split = 1; replace = 1; }
    else if (!strcmp(mode, "dump")) { split = 1; mask = 2; }
    else if (!strcmp(mode, "replace")) { split = 1; mask = 2; replace = 1; }
    else if (!strcmp(mode, "only0")) key1 = 0;
    else return 91;

    spu_psx_init();
    w(0x1AA, 0xC080);                       /* enable, unmute, reverb master on */
    w(0x180, 0x3FFF);
    w(0x182, 0x3FFF);
    load_samples();
    for (i = 0; i < 32; i++) w(0x1C0 + 2 * i, studio_medium[i]);
    w(0x1A2, REVERB_BASE >> 3);
    w(0x184, 0x3C78);
    w(0x186, 0x3C78);
    w(0x198, 3);                            /* EON: both voices */
    voice(0, 0x2000, 0x0800);
    voice(1, 0x3000, 0x1300);
    if (split) spu_psx_music_split(1, mask, replace);
    w(0x188, key1 ? 3 : 1);
    for (i = 0; i < FRAMES; i += 735) {
        int n = FRAMES - i < 735 ? FRAMES - i : 735;
        if (i == 735 * 12) w(0x18C, key1 ? 3 : 1);     /* key off: release tails */
        spu_psx_mix_split(out + i * 2, music + i * 2, n);
    }
    spu_psx_dma_read(ram, 0, sizeof ram);
    f = fopen(argv[2], "wb");
    if (!f) return 92;
    fwrite(out, sizeof out, 1, f);
    fwrite(music, sizeof music, 1, f);
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
