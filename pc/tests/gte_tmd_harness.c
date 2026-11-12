/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gte_tmd_harness.c -- drives pc/hal/gte.c and the port's TMD builders
 * (src/main/pc-port/tmd_gte_builders.c) for pc/tools/test_gte_tmd.py.
 *
 *   gte_tmd_harness rot  IN OUT   IN: n * (vx vy vz) s32;  OUT: n * 9 s32
 *   gte_tmd_harness sqrt IN OUT   IN: n s32;  OUT: n * (SquareRoot0, 12)
 *   gte_tmd_harness rtp  IN OUT   see run_rtp()
 *   gte_tmd_harness tmd  IN OUT   see run_tmd()
 *
 * All words little-endian 32-bit.  The builders keep PS1 addresses in 32
 * bits, so their memory lives in a 2 MB arena mapped below 4 GB (at 1 GB,
 * which is ordinary application memory even under AddressSanitizer); the
 * arena base is reported so the caller can compare addresses relative to
 * it.  Uses only the real HAL/builder code -- no replacement GTE. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "hal.h"
#include "hal_gte_cop2.h"

extern void gte_test_tables_init(void);

s32 GsTON;

void debug_log(const char *fmt, ...) { (void)fmt; }

void *hal_ps1_ptr(const void *p) { return (void *)p; }

extern s32 GsTMDfastNF3(u8 *, u8 *, s32, u16, s32, s32, s32);
extern s32 GsTMDfastNF4(u8 *, u8 *, s32, u16, s32, s32, s32);
extern s32 GsTMDfastTNF4(u8 *, u8 *, s32, u16, s32, s32, s32);
extern s32 GsTMDfastNG3(u8 *, u8 *, s32, u16, s32, s32);
extern s32 GsTMDfastNG4(u8 *, u8 *, s32, u16, s32, s32);
extern s32 GsTMDfastTNF3(u8 *, u8 *, s32, u16, s32, s32);
extern s32 GsTMDfastTNG3(u8 *, u8 *, s32, u16, s32, s32);
extern s32 GsTMDfastTNG4(u8 *, u8 *, s32, u16, s32, s32);

static u32 *g_in;
static size_t g_words;

static void load(const char *path)
{
    FILE *f = fopen(path, "rb");
    long size;
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_words = (size_t)size / 4;
    g_in = (u32 *)malloc((size_t)size + 4);
    if (!g_in || fread(g_in, 4, g_words, f) != g_words) {
        perror("read");
        exit(2);
    }
    fclose(f);
}

static FILE *g_out;

static void put(u32 v)
{
    if (fwrite(&v, 4, 1, g_out) != 1) { perror("write"); exit(2); }
}

static void load_matrix(const u32 *w)
{
    MATRIX m;
    int i;
    for (i = 0; i < 9; i++) m.m[i / 3][i % 3] = (s16)w[i];
    for (i = 0; i < 3; i++) m.t[i] = (s32)w[9 + i];
    SetRotMatrix(&m);
    SetTransMatrix(&m);
}

/* rtp IN: rot[9] trans[3] h ofx ofy n, then n groups of 4 vertices (x y z).
 * OUT per group: RotTransPers of each vertex (sxy p flag ret) x4,
 * RotTransPers3 of the first three (sxy0 sxy1 sxy2 p flag ret),
 * AverageZ3 and AverageZ4 of the four RotTransPers SZ values recovered as
 * ret * 4 is lossy, so the SZ FIFO after RotTransPers4 is read instead
 * (ReadSZfifo4), then AverageZ3(sz1..3) and AverageZ4(sz0..3). */
static void run_rtp(void)
{
    const u32 *w = g_in;
    u32 n, g;
    InitGeom();
    load_matrix(w);
    SetGeomScreen((s32)w[12]);
    SetGeomOffset((s32)w[13], (s32)w[14]);
    n = w[15];
    w += 16;
    for (g = 0; g < n; g++, w += 12) {
        SVECTOR v[4];
        s32 i, sxy[4], p, flag, ret, szx, sz0, sz1, sz2;
        for (i = 0; i < 4; i++) {
            v[i].vx = (s16)w[i * 3];
            v[i].vy = (s16)w[i * 3 + 1];
            v[i].vz = (s16)w[i * 3 + 2];
            v[i].pad = 0;
        }
        for (i = 0; i < 4; i++) {
            ret = RotTransPers(&v[i], &sxy[0], &p, &flag);
            put((u32)sxy[0]); put((u32)p); put((u32)flag); put((u32)ret);
        }
        ret = RotTransPers3(&v[0], &v[1], &v[2], &sxy[0], &sxy[1], &sxy[2],
                            &p, &flag);
        put((u32)sxy[0]); put((u32)sxy[1]); put((u32)sxy[2]);
        put((u32)p); put((u32)flag); put((u32)ret);
        RotTransPers4(&v[0], &v[1], &v[2], &v[3], &sxy[0], &sxy[1], &sxy[2],
                      &sxy[3], &p, &flag);
        ReadSZfifo4(&szx, &sz0, &sz1, &sz2);
        put((u32)AverageZ3(sz0, sz1, sz2));
        put((u32)AverageZ4(szx, sz0, sz1, sz2));
    }
}

#define ARENA_BASE 0x40000000UL
#define ARENA_SIZE 0x200000UL
#define VTX_OFF    0x010000UL
#define PRIM_OFF   0x020000UL
#define PKT_OFF    0x040000UL
#define OT_OFF     0x080000UL
#define ORG_OFF    0x081000UL
#define PKT_WORDS  1024
#define OT_SLOTS   256

/* tmd IN: family(0..7) count shift ton rot[9] trans[3] h ofx ofy ot_offset
 * nvert, nvert * (x y z), nprimbytes, prim bytes (padded to words).
 * OUT: arena base, cursor - base, PKT_WORDS packet words, OT_SLOTS words. */
static void run_tmd(void)
{
    const u32 *w = g_in;
    u8 *arena;
    u32 family, count, shift, nvert, nbytes, i;
    s32 ot, packet, cursor;
    u32 *pkt, *org, *otp;
    SVECTOR *vtx;
    u8 *prim;

    arena = (u8 *)mmap((void *)ARENA_BASE, ARENA_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                       -1, 0);
    if (arena == (u8 *)MAP_FAILED || (unsigned long)arena != ARENA_BASE) {
        fprintf(stderr, "arena mmap failed: %s\n", strerror(errno));
        exit(3);
    }
    family = w[0];
    count = w[1];
    shift = w[2];
    GsTON = (s32)w[3];
    InitGeom();
    load_matrix(w + 4);
    SetGeomScreen((s32)w[16]);
    SetGeomOffset((s32)w[17], (s32)w[18]);
    vtx = (SVECTOR *)(arena + VTX_OFF);
    prim = arena + PRIM_OFF;
    pkt = (u32 *)(arena + PKT_OFF);
    otp = (u32 *)(arena + OT_OFF);
    org = (u32 *)(arena + ORG_OFF);
    otp[0] = 8;
    otp[1] = (u32)(unsigned long)org;
    otp[2] = w[19];
    nvert = w[20];
    w += 21;
    for (i = 0; i < nvert; i++, w += 3) {
        vtx[i].vx = (s16)w[0];
        vtx[i].vy = (s16)w[1];
        vtx[i].vz = (s16)w[2];
        vtx[i].pad = 0;
    }
    nbytes = *w++;
    memcpy(prim, w, nbytes);
    for (i = 0; i < PKT_WORDS; i++) pkt[i] = 0xCDCDCDCDu;
    /* Distinct OT words: a DMA tag byte on top, a fake next link below. */
    for (i = 0; i < OT_SLOTS; i++) org[i] = 0xA5000000u | (i * 0x10u);
    ot = (s32)(u32)(unsigned long)otp;
    packet = (s32)(u32)(unsigned long)pkt;
#define B7(f) f(prim, (u8 *)vtx, packet, (u16)count, (s32)shift, ot, 0)
#define B6(f) f(prim, (u8 *)vtx, packet, (u16)count, (s32)shift, ot)
    switch (family) {
    case 0: cursor = B7(GsTMDfastNF3); break;
    case 1: cursor = B7(GsTMDfastNF4); break;
    case 2: cursor = B6(GsTMDfastNG3); break;
    case 3: cursor = B6(GsTMDfastNG4); break;
    case 4: cursor = B6(GsTMDfastTNF3); break;
    case 5: cursor = B7(GsTMDfastTNF4); break;
    case 6: cursor = B6(GsTMDfastTNG3); break;
    case 7: cursor = B6(GsTMDfastTNG4); break;
    default: fprintf(stderr, "bad family\n"); exit(2);
    }
#undef B7
#undef B6
    put((u32)ARENA_BASE);
    put((u32)cursor - (u32)ARENA_BASE);
    for (i = 0; i < PKT_WORDS; i++) put(pkt[i]);
    for (i = 0; i < OT_SLOTS; i++) put(org[i]);
    munmap(arena, ARENA_SIZE);
}

extern s32 GsTMDfastF3L(u8 *, u8 *, u8 *, s32, u16, s32, s32);
extern s32 GsTMDfastTF3L(u8 *, u8 *, u8 *, s32, u16, s32, s32);
extern s32 GsTMDfastTG3LFG(u8 *, u8 *, u8 *, s32, u16, s32, s32);
extern void SetFarColor(s32, s32, s32);
extern void SetDQA(s32);
extern void SetDQB(s32);
extern s32 GsTMDfastTG3L(u8 *, u8 *, u8 *, s32, u16, s32, s32);

#define NORM_OFF   0x018000UL

static void load_s16_matrix(MATRIX *m, const u32 *w)
{
    int k;
    memset(m, 0, sizeof(*m));
    for (k = 0; k < 9; k++) m->m[k / 3][k % 3] = (s16)w[k];
}

/* lit IN: family(0 F3L, 1 TF3L, 2 TG3L, 3 TG3LFG) count shift ton rot[9]
 * trans[3] h ofx ofy ot_offset llm[9] lcm[9] back[3] (SetBackColor
 * arguments) far[3] (SetFarColor arguments) dqa dqb (SetDQA / SetDQB)
 * nvert, nvert * (x y z), nnorm, nnorm * (x y z), nprimbytes, prim bytes.
 * The lighting registers are loaded through the libgte setters the game
 * uses (SetLightMatrix, SetColorMatrix, SetBackColor, SetFarColor,
 * SetDQA, SetDQB).
 * OUT: as tmd. */
static void run_lit(void)
{
    const u32 *w = g_in;
    u8 *arena;
    u32 family, count, shift, n, nbytes, i;
    s32 ot, packet, cursor;
    u32 *pkt, *org, *otp;
    SVECTOR *vtx, *nrm;
    MATRIX m;
    u8 *prim;

    arena = (u8 *)mmap((void *)ARENA_BASE, ARENA_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                       -1, 0);
    if (arena == (u8 *)MAP_FAILED || (unsigned long)arena != ARENA_BASE) {
        fprintf(stderr, "arena mmap failed: %s\n", strerror(errno));
        exit(3);
    }
    family = w[0];
    count = w[1];
    shift = w[2];
    GsTON = (s32)w[3];
    InitGeom();
    load_matrix(w + 4);
    SetGeomScreen((s32)w[16]);
    SetGeomOffset((s32)w[17], (s32)w[18]);
    vtx = (SVECTOR *)(arena + VTX_OFF);
    nrm = (SVECTOR *)(arena + NORM_OFF);
    prim = arena + PRIM_OFF;
    pkt = (u32 *)(arena + PKT_OFF);
    otp = (u32 *)(arena + OT_OFF);
    org = (u32 *)(arena + ORG_OFF);
    otp[0] = 8;
    otp[1] = (u32)(unsigned long)org;
    otp[2] = w[19];
    load_s16_matrix(&m, w + 20);
    SetLightMatrix(&m);
    load_s16_matrix(&m, w + 29);
    SetColorMatrix(&m);
    SetBackColor((s32)w[38], (s32)w[39], (s32)w[40]);
    SetFarColor((s32)w[41], (s32)w[42], (s32)w[43]);
    SetDQA((s32)w[44]);
    SetDQB((s32)w[45]);
    w += 46;
    n = *w++;
    for (i = 0; i < n; i++, w += 3) {
        vtx[i].vx = (s16)w[0]; vtx[i].vy = (s16)w[1]; vtx[i].vz = (s16)w[2];
        vtx[i].pad = 0;
    }
    n = *w++;
    for (i = 0; i < n; i++, w += 3) {
        nrm[i].vx = (s16)w[0]; nrm[i].vy = (s16)w[1]; nrm[i].vz = (s16)w[2];
        nrm[i].pad = 0;
    }
    nbytes = *w++;
    memcpy(prim, w, nbytes);
    for (i = 0; i < PKT_WORDS; i++) pkt[i] = 0xCDCDCDCDu;
    for (i = 0; i < OT_SLOTS; i++) org[i] = 0xA5000000u | (i * 0x10u);
    ot = (s32)(u32)(unsigned long)otp;
    packet = (s32)(u32)(unsigned long)pkt;
#define BL(f) f(prim, (u8 *)vtx, (u8 *)nrm, packet, (u16)count, (s32)shift, ot)
    switch (family) {
    case 0: cursor = BL(GsTMDfastF3L); break;
    case 1: cursor = BL(GsTMDfastTF3L); break;
    case 2: cursor = BL(GsTMDfastTG3L); break;
    case 3: cursor = BL(GsTMDfastTG3LFG); break;
    default: fprintf(stderr, "bad family\n"); exit(2);
    }
#undef BL
    put((u32)ARENA_BASE);
    put((u32)cursor - (u32)ARENA_BASE);
    for (i = 0; i < PKT_WORDS; i++) put(pkt[i]);
    for (i = 0; i < OT_SLOTS; i++) put(org[i]);
    munmap(arena, ARENA_SIZE);
}

int main(int argc, char **argv)
{
    size_t i;
    if (argc != 4) {
        fprintf(stderr, "usage: %s rot|sqrt|rtp|tmd IN OUT\n", argv[0]);
        return 2;
    }
    gte_test_tables_init();
    load(argv[2]);
    g_out = fopen(argv[3], "wb");
    if (!g_out) { perror(argv[3]); return 2; }
    if (!strcmp(argv[1], "rot")) {
        for (i = 0; i + 3 <= g_words; i += 3) {
            SVECTOR r;
            MATRIX m;
            int k;
            r.vx = (s16)g_in[i];
            r.vy = (s16)g_in[i + 1];
            r.vz = (s16)g_in[i + 2];
            r.pad = 0;
            memset(&m, 0, sizeof(m));
            RotMatrix(&r, &m);
            for (k = 0; k < 9; k++) put((u32)(s32)m.m[k / 3][k % 3]);
        }
    } else if (!strcmp(argv[1], "sqrt")) {
        for (i = 0; i < g_words; i++) {
            put((u32)SquareRoot0((s32)g_in[i]));
            put((u32)SquareRoot12((s32)g_in[i]));
        }
    } else if (!strcmp(argv[1], "rtp")) {
        run_rtp();
    } else if (!strcmp(argv[1], "tmd")) {
        run_tmd();
    } else if (!strcmp(argv[1], "lit")) {
        run_lit();
    } else if (!strcmp(argv[1], "rotaxis")) {
        /* IN: n * (entry a m[9]); entry 0..5 = TACO_RotMatrixX/Y/Z,
         * DATE_RotMatrixX/Y/Z (pc/hal/gte_address_entries.c).
         * OUT: n * (m[9], returned == m). */
        extern MATRIX *TACO_RotMatrixX(s32, MATRIX *);
        extern MATRIX *TACO_RotMatrixY(s32, MATRIX *);
        extern MATRIX *TACO_RotMatrixZ(s32, MATRIX *);
        extern MATRIX *DATE_RotMatrixX(s32, MATRIX *);
        extern MATRIX *DATE_RotMatrixY(s32, MATRIX *);
        extern MATRIX *DATE_RotMatrixZ(s32, MATRIX *);
        static MATRIX *(*const fn[6])(s32, MATRIX *) = {
            TACO_RotMatrixX, TACO_RotMatrixY, TACO_RotMatrixZ,
            DATE_RotMatrixX, DATE_RotMatrixY, DATE_RotMatrixZ };
        for (i = 0; i + 11 <= g_words; i += 11) {
            MATRIX m, *r;
            int k;
            memset(&m, 0, sizeof(m));
            for (k = 0; k < 9; k++) m.m[k / 3][k % 3] = (s16)g_in[i + 2 + k];
            r = fn[g_in[i] % 6]((s32)g_in[i + 1], &m);
            for (k = 0; k < 9; k++) put((u32)(s32)m.m[k / 3][k % 3]);
            put(r == &m);
        }
    } else {
        fprintf(stderr, "unknown mode %s\n", argv[1]);
        return 2;
    }
    if (fclose(g_out)) { perror("close"); return 2; }
    free(g_in);
    return 0;
}
