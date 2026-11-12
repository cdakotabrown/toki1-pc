/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * libpress_still_harness.c -- the libpress MDEC contract for a still BS in
 * RAM (decdct: dec_env_set + DecDCTvlc/DecDCTin/DecDCTout slices), driven
 * through the real pc/hal/libpress.c and pc/hal/mdec_native.c.
 *
 * The bitstream is synthetic: six macroblocks with distinct flat levels,
 * so a slice that takes the wrong macroblock, or an exhausted/zeroed
 * decoder, is visible.  Expected pixels come from the whole-image decoder
 * (column-major macroblocks, 32x48 = two 16-pixel columns of three).
 *
 *   still   mode 2, decoding into the buffer that holds the bitstream (as
 *           decdct does: image == bs == 0x80162000); bit 15 set
 *   nostp   mode 0: bit 15 clear
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "mdec_native.h"

extern void DecDCTReset(s32 mode);
extern void DecDCToutCallback(void (*fn)(void));
extern void DecDCTinCallback(void (*fn)(void));
extern void DecDCTvlc(void *bs, void *vlcbuf);
extern void DecDCTin(void *vlcbuf, s32 mode);
extern void DecDCTout(u16 *buf, s32 nwords);

/* pc/hal/cd.c, system.c and game symbols libpress.c links against; the
 * movie path is not exercised here */
const char *cd_hal_current_file(void) { return NULL; }
s32 cd_hal_current_sector(void) { return 0; }
const char *cd_hal_file_for_sector(s32 s, char *o, int n) { (void)s; (void)o; (void)n; return NULL; }
u32 cd_hal_stream_start(u8 m) { (void)m; return 0; }
int cd_hal_stream_wait_sector(u32 l) { (void)l; return 0; }
void hal_cpu_spend(u32 t) { (void)t; }
u64 hal_clock_ticks(void) { return 0; }
u32 hal_clock_ticks_per_frame(void) { return 564480; }
s32 vblnk_counter;
void *movie_state;

#define MBS 6
#define W 32
#define H 48
#define SLICE_WORDS (16 * H / 2)

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

static u32 build(u8 *bs, u32 cap) {
    u32 position = 0;
    int mb, block;
    memset(bs, 0, cap);
    bs[0] = 64;          /* run-level size in words: 36 halfwords used */
    bs[2] = 0x00;
    bs[3] = 0x38;
    bs[4] = 1;           /* qscale */
    bs[6] = 2;           /* version */
    for (mb = 0; mb < MBS; mb++) {
        for (block = 0; block < 6; block++) {
            /* Cr, Cb flat 0; the four Y blocks carry the macroblock's level */
            s32 dc = block < 2 ? 0 : 60 * mb - 150;
            put_code(bs, &position, (u32)dc & 0x3FF, 10);
            put_code(bs, &position, 2, 2);   /* EOB */
        }
    }
    return 8 + (position + 15) / 16 * 2;
}

static void check(int stp) {
    static u8 ram[2048];                 /* bitstream; decoded into in place */
    static u8 vlc[64];
    u16 want[W * H];
    u16 *slice = (u16 *)ram;
    u32 size = build(ram, sizeof ram);
    int col, y, x;

    assert(sizeof ram >= SLICE_WORDS * 4);
    assert(mdec_decode_bs_native(ram, size, W, H, want, 0) == 0);
    for (x = 1; x < MBS; x++)            /* the levels really are distinct */
        assert(want[(x % 3) * 16 * W + (x / 3) * 16] != want[0]);

    DecDCTReset(0);
    DecDCToutCallback(0);
    DecDCTinCallback(0);
    DecDCTvlc(ram, vlc);
    DecDCTin(vlc, stp ? 2 : 0);
    for (col = 0; col < W / 16; col++) {
        DecDCTout(slice, SLICE_WORDS);
        for (y = 0; y < H; y++)
            for (x = 0; x < 16; x++) {
                u16 got = slice[y * 16 + x];
                u16 exp = (u16)(want[y * W + col * 16 + x] | (stp ? 0x8000 : 0));
                if (got != exp) {
                    fprintf(stderr, "col %d (%d,%d): got %04x want %04x\n",
                            col, x, y, got, exp);
                    abort();
                }
            }
    }
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "still")) check(1);
    else if (!strcmp(argv[1], "nostp")) check(0);
    else return 2;
    return 0;
}
