/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gs_coord_harness.c -- GsGetLw / GsGetLws / GsGetLs over a PS1-layout
 * GsCOORDINATE2 chain, for pc/tools/test_gs_coord.py.
 *
 *   gs_coord_harness IN OUT
 *   IN : ws[8] then n, then n records of 19 words (flg, coord[8], workm[8],
 *        param, super-index (-1 = none)), then the query node index.
 *   OUT: Lw[8] (GsGetLw), Lws light[8] + screen[8] (GsGetLws), Ls[8]
 *        (GsGetLs) and every record's 19 words after the three calls.
 * The records live in an arena below 4 GB so their 32-bit handles are
 * real addresses; the arena is at 1 GB (ordinary memory under ASan). */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "types.h"

s32 GsWSMATRIX[8];
u32 DSTACK[100];

extern void GsGetLw(void *coord, s32 *output);
extern void GsGetLws(void *coord, s32 *output, s32 *dest);
extern void GsGetLs(void *coord, s32 *output);

#define ARENA ((unsigned long)0x40000000UL)
#define STRIDE 80

static void put(FILE *f, const s32 *w, int n)
{
    if (fwrite(w, 4, (size_t)n, f) != (size_t)n) { perror("write"); exit(2); }
}

int main(int argc, char **argv)
{
    FILE *f;
    s32 in[4096];
    size_t got;
    s32 n, i, k, q;
    u8 *arena;
    s32 lw[8], light[8], screen[8], ls[8];
    s32 snapshot[19 * 64];
    const s32 *rec;

    if (argc != 3) return 2;
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    got = fread(in, 4, 4096, f);
    fclose(f);
    if (got < 10) return 2;
    arena = (u8 *)mmap((void *)ARENA, 0x10000, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                       -1, 0);
    if (arena == (u8 *)MAP_FAILED || (unsigned long)arena != ARENA) {
        fprintf(stderr, "arena: %s\n", strerror(errno));
        return 3;
    }
    /* Fill record padding (+76..+79) with a non-zero word: a reader that
     * takes 8 bytes at +72 sees it as a pointer's high half. */
    memset(arena, 0x5A, 0x10000);
    for (i = 0; i < 8; i++) GsWSMATRIX[i] = in[i];
    n = in[8];
    if (n < 1 || n > 60) return 2;
    rec = in + 9;
    for (i = 0; i < n; i++, rec += 19) {
        s32 *w = (s32 *)(arena + i * STRIDE);
        for (k = 0; k < 18; k++) w[k] = rec[k];
        w[18] = rec[18] < 0 ? 0
              : (s32)(u32)(ARENA + (unsigned long)rec[18] * STRIDE);
    }
    q = *rec;
    {
        void *node = arena + q * STRIDE;
        /* Each call starts from the same records: snapshot, call, restore. */
        memcpy(snapshot, arena, (size_t)n * STRIDE);
        GsGetLw(node, lw);
        memcpy(arena, snapshot, (size_t)n * STRIDE);
        GsGetLws(node, light, screen);
        memcpy(arena, snapshot, (size_t)n * STRIDE);
        GsGetLs(node, ls);
    }
    f = fopen(argv[2], "wb");
    if (!f) return 2;
    put(f, lw, 8);
    put(f, light, 8);
    put(f, screen, 8);
    put(f, ls, 8);
    for (i = 0; i < n; i++) {
        s32 w[19];
        memcpy(w, arena + i * STRIDE, sizeof(w));
        put(f, w, 19);
    }
    fclose(f);
    munmap(arena, 0x10000);
    return 0;
}
