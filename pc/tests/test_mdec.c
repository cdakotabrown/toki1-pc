/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc/tests/test_mdec.c — tests for the native MDEC decoder.
 *
 * Decodes TOKIMEKI.BS via the native decoder and checks:
 *   1. The decode succeeds without error.
 *   2. The output buffer has the expected size (320×240 = 76800 u16s).
 *   3. The output is "plausibly the title logo": pixel-density check
 *      (non-transparent pixels concentrated in the middle horizontal
 *      band where the logo glyphs live).
 * Synthetic VLC boundary tests below do not require game assets.
 * Plausibility/determinism are not byte-exact reference-image validation.
 */
#include "../include/game/types.h"
#include "../hal/mdec_native.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BS_PATH "pc/assets/raw/CDROM/FUKUTEST/TOKIMEKI.BS"
#define W 320
#define H 240
#define NPX (W * H)

static u8 *slurp(const char *path, u32 *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *buf = (u8 *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, (size_t)sz, f);
    fclose(f);
    *out_size = (u32)sz;
    return buf;
}

/* BS codes are MSB-first inside little-endian 16-bit words. */
static void put_code(u8 *bs, u32 *position, u32 code, u32 bits) {
    u32 byte;
    while (bits) {
        bits--;
        byte = 8 + (*position / 16) * 2 + 1 - (*position % 16) / 8;
        if ((code >> bits) & 1)
            bs[byte] |= (u8)(1 << (7 - *position % 8));
        (*position)++;
    }
}

static void put_escape(u8 *bs, u32 *position, u32 run) {
    put_code(bs, position, 1, 6);  /* escape: 000001 */
    put_code(bs, position, run, 6);
    put_code(bs, position, 1, 10); /* level +1 */
}

int test_mdec_vlc_eob(void) {
    u8 bs[48];
    u16 pixels[16 * 32 + 2];
    u32 position;
    int target, normal, overflow, block;
    s32 result;

    /* Two macroblocks ensure that every tested block has a successor.
     * Both escape and ordinary AC codes can fill coefficient 63. Neither
     * implicitly terminates the block: EOB must still be consumed. */
    for (target = 0; target < 6; target++) {
        for (normal = 0; normal < 2; normal++) {
            for (overflow = 0; overflow < 2; overflow++) {
                memset(bs, 0, sizeof bs);
                bs[2] = 0x00;
                bs[3] = 0x38;
                bs[4] = 1; /* qscale */
                bs[6] = 2; /* version */
                position = 0;
                for (block = 0; block < 12; block++) {
                    put_code(bs, &position, 0, 10); /* DC */
                    if (block == target) {
                        put_escape(bs, &position, normal ? 61 : 62);
                        if (normal)
                            put_code(bs, &position, 6, 3); /* run 0,+1 */
                        if (overflow) {
                            if (normal)
                                put_code(bs, &position, 6, 3);
                            else
                                put_escape(bs, &position, 0);
                        }
                    }
                    put_code(bs, &position, 2, 2); /* explicit EOB: 10 */
                }
                pixels[0] = pixels[16 * 32 + 1] = 0x55aa;
                result = mdec_decode_bs_native(bs, sizeof bs, 16, 32,
                                               pixels + 1, 0);
                if ((overflow && result >= 0) || (!overflow && result)) {
                    printf("(block=%d normal=%d overflow=%d rc=%d) ",
                           target, normal, overflow, result);
                    return 1;
                }
                if (pixels[0] != 0x55aa ||
                    pixels[16 * 32 + 1] != 0x55aa)
                    return 2;
            }
        }
    }
    return 0;
}

int test_mdec_truncated_codes(void) {
    u8 bs[32] = {0, 0, 0, 0x38, 1, 0, 2, 0};
    u16 pixels[16 * 16];
    u32 position = 0;
    u32 size;
    int block, coefficient;

    /* 72 DC/EOB bits plus 19 three-bit AC codes = 129 bits.
     * Cutting at 128 leaves the final EOB's leading 1 but not its 0.
     * Zero-filled lookahead is NOT a substitute for that missing bit. */
    for (block = 0; block < 6; block++) {
        put_code(bs, &position, 0, 10);
        if (!block) {
            for (coefficient = 0; coefficient < 19; coefficient++)
                put_code(bs, &position, 6, 3);
        }
        put_code(bs, &position, 2, 2);
    }
    if (position != 129) return 1;
    if (mdec_decode_bs_native(bs, 26, 16, 16, pixels, 0)) return 2;
    for (size = 8; size < 26; size++) {
        if (mdec_decode_bs_native(bs, size, 16, 16, pixels, 0) >= 0) {
            printf("(accepted truncated size=%u) ", size);
            return 3;
        }
    }
    return 0;
}

int test_mdec_native(void) {
    u32 bs_size;
    u8 *bs = slurp(BS_PATH, &bs_size);
    if (!bs) {
        printf("(missing %s) ", BS_PATH);
        return 1;
    }

    /* Sanity: TOKIMEKI.BS is 18432 bytes with magic 0x3800 at offset 2. */
    if (bs_size != 18432 || bs[2] != 0x00 || bs[3] != 0x38) {
        printf("(bad input file: size=%u magic=%02X%02X) ", bs_size, bs[2], bs[3]);
        free(bs);
        return 9;
    }

    u16 *pixels = (u16 *)malloc(NPX * 2);
    memset(pixels, 0, NPX * 2);

    s32 rc = mdec_decode_bs_native(bs, bs_size, W, H, pixels, 0);
    if (rc != 0) {
        printf("(decode rc=%d) ", rc);
        free(bs); free(pixels);
        return 2;
    }

    /* Determinism: re-decode and compare to the first run. */
    {
        u16 *p2 = (u16 *)malloc(NPX * 2);
        memset(p2, 0, NPX * 2);
        s32 rc2 = mdec_decode_bs_native(bs, bs_size, W, H, p2, 0);
        if (rc2 != 0 || memcmp(pixels, p2, NPX * 2) != 0) {
            printf("(non-deterministic) ");
            free(bs); free(pixels); free(p2);
            return 10;
        }
        free(p2);
    }

    /* Plausibility check 1: at least 5% of pixels non-zero. */
    u32 nonzero = 0;
    for (u32 i = 0; i < NPX; i++) if (pixels[i] != 0) nonzero++;
    if (nonzero < NPX / 20) {
        printf("(too few non-zero: %u/%u) ", nonzero, NPX);
        free(bs); free(pixels);
        return 3;
    }

    /* Plausibility check 2: non-zero density in the middle vertical band
     * where the logo lives (rows 60..220 from a 240-tall image). */
    u32 mid_nonzero = 0;
    for (u32 y = 60; y < 220; y++)
        for (u32 x = 0; x < W; x++)
            if (pixels[y * W + x] != 0) mid_nonzero++;
    if (mid_nonzero < (160 * W) / 30) {
        printf("(middle band sparse: %u) ", mid_nonzero);
        free(bs); free(pixels);
        return 4;
    }

    free(bs); free(pixels);
    return 0;
}

/* The 24-bit output (the boot movie's DecDCTin mode 3): same frame, each
 * channel rounded the 15-bit way, (c + 4) >> 3 clamped to 31, must give the
 * 15-bit decode exactly; and the bytes are R, G, B in that order. */
int test_mdec_rgb24(void) {
    u32 bs_size, i, mism = 0, wide = 0;
    u8 *bs = slurp(BS_PATH, &bs_size);
    u16 *p555;
    u8 *p24;
    if (!bs) {
        printf("(missing %s) ", BS_PATH);
        return 1;
    }
    p555 = (u16 *)calloc(NPX, 2);
    p24 = (u8 *)calloc(NPX, 3);
    if (mdec_decode_bs_native(bs, bs_size, W, H, p555, 0) != 0 ||
        mdec_decode_bs_native_rgb24(bs, bs_size, W, H, p24) != 0) {
        free(bs); free(p555); free(p24);
        return 2;
    }
    for (i = 0; i < NPX; i++) {
        u32 c, k, v = 0;
        for (k = 0; k < 3; k++) {
            c = ((u32)p24[i * 3 + k] + 4) >> 3;
            v |= (c > 31 ? 31 : c) << (5 * k);
            if (p24[i * 3 + k] & 7) wide++;
        }
        if (v != p555[i]) mism++;
    }
    free(bs); free(p555); free(p24);
    if (mism) { printf("(%u pixels differ from 15-bit) ", mism); return 3; }
    if (!wide) { printf("(no sub-5-bit detail: not 8-bit output) "); return 4; }
    return 0;
}
