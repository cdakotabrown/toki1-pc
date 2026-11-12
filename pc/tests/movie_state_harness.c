/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Native movie helper ABI checks; all stream/decoder calls are mocks. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../src/main/pc-port/mv_state.h"

extern void strSync(MvDec *, s32);
extern s32 strNextVlc(MvDec *);

static u32 buffers[2][16];
static u8 encoded[16];
static int next_calls, decode_calls, free_calls, misses, bad_decode;
static int operations[16], operation_count;
static u32 *expected_buffer;

static void record(int operation) {
    if (operation_count < 16) operations[operation_count] = operation;
    operation_count++;
}

u8 *strNext(MvDec *dec) {
    (void)dec;
    next_calls++;
    record(1);
    return next_calls <= misses ? NULL : encoded;
}

s32 StFreeRing(s32 address) {
    free_calls++;
    record(3);
    if (address != 0 && address != (s32)(uintptr_t)encoded) bad_decode = 1;
    return 0;
}

void DecDCTvlc(void *data, void *buffer) {
    decode_calls++;
    record(2);
    if (data != encoded || buffer != expected_buffer) bad_decode = 1;
}

static void initialize(MvDec *dec) {
    memset(dec, 0, sizeof(*dec));
    dec->vlcbuf[0] = buffers[0];
    dec->vlcbuf[1] = buffers[1];
    dec->rect[0].x = 7; dec->rect[0].y = 9;
    dec->rect[0].w = 320; dec->rect[0].h = 240;
    dec->rect[1].x = 11; dec->rect[1].y = 240;
    dec->rect[1].w = 480; dec->rect[1].h = 240;
    dec->slice.w = 16; dec->slice.h = 240;
}

int main(int argc, char **argv) {
    MvDec dec, expected;
    int i;
    static const int retry_order[] = {1, 3, 1, 3, 1, 2, 3};
    if (argc != 2) return 90;
    initialize(&dec);
    if (!strcmp(argv[1], "done")) {
        dec.isdone = 1;
        expected = dec;
        expected.isdone = 0;
        strSync(&dec, 0);
        return memcmp(&dec, &expected, sizeof(dec)) != 0;
    }
    if (!strcmp(argv[1], "sync_timeout")) {
        for (i = 0; i < 2; i++) {
            initialize(&dec);
            dec.rectid = i;
            expected = dec;
            expected.rectid = 1 - i;
            expected.slice.x = dec.rect[1 - i].x;
            expected.slice.y = dec.rect[1 - i].y;
            strSync(&dec, 0);
            if (memcmp(&dec, &expected, sizeof(dec))) return 1;
        }
        return 0;
    }
    if (!strcmp(argv[1], "vlc")) {
        for (i = 0; i < 2; i++) {
            expected_buffer = buffers[1 - i];
            if (strNextVlc(&dec) != 0 || dec.vlcid != 1 - i) return 1;
            if (dec.vlcbuf[0] != buffers[0] || dec.vlcbuf[1] != buffers[1]) return 2;
        }
        if (next_calls != 2 || decode_calls != 2 || free_calls != 2) return 3;
        for (i = 0; i < 6; i++)
            if (operations[i] != i % 3 + 1) return 4;
        return bad_decode;
    }
    if (!strcmp(argv[1], "retry")) {
        misses = 2;
        expected_buffer = buffers[1];
        if (strNextVlc(&dec) != 0 || bad_decode) return 1;
        if (operation_count != 7) return 2;
        for (i = 0; i < 7; i++)
            if (operations[i] != retry_order[i]) return 3;
        return 0;
    }
    if (!strcmp(argv[1], "vlc_timeout")) {
        misses = 0x800000;
        expected = dec;
        if (strNextVlc(&dec) != -1) return 1;
        if (next_calls != 0x800000 || free_calls != 0x7fffff || decode_calls) return 2;
        return memcmp(&dec, &expected, sizeof(dec)) != 0;
    }
    return 91;
}
