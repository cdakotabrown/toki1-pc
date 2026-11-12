/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * hal_gpu_poly.h — HAL polygon-emit API for the Konami GTE/libgs wrappers.
 *
 * The PS1 build has 28 functions in 0x800A70D8..0x800B0080 that wrap libgs
 * primitive emission: each does GTE projection on a polyHdr_* struct, then
 * builds a GP0 primitive packet and XOR-swaps it into the ordering-table
 * bucket. Per analyst guidance (Q4), the PC port replaces these with
 * specific typed wrappers that:
 *   1. Read screen-XY, UVs, color, tpage, clut from the packed header.
 *   2. Skip real GTE projection (Tokimeki is mostly 2D — input coords
 *      are usually already screen-space).
 *   3. Build the GP0 packet directly, advance the packet pointer, and
 *      link into the ordering-table bucket.
 *
 * Each Konami wrapper at GsTMDfastTNF3 etc. becomes a thin C function
 * that fills out a primitive struct and calls one of these.
 *
 * Status: API skeleton only. Implementations in pc/hal/gpu.c are no-ops
 * pending a render-path overhaul. The headers exist so the 28 stubs can
 * be converted incrementally without further HAL churn.
 */
#ifndef HAL_GPU_POLY_H
#define HAL_GPU_POLY_H

#include "types.h"

typedef unsigned char PACKET;

/* PS1 polygon header structures (sizes verified by disasm of stride
 * advances in the wrapper functions). All fields are little-endian. */

typedef struct {
    /* 28 bytes — polyHdr_FT3 (textured triangle) */
    u8  code;       /* 0 — primitive code, OR'd with 0x24 (POLY_FT3) */
    u8  r0, g0, b0; /* 1..3 — flat color */
    u8  u0, v0;     /* 4..5 — UV vertex 0 */
    u16 clut;       /* 6..7 — CLUT id */
    u8  u1, v1;     /* 8..9 */
    u16 tpage;      /* 10..11 — tpage id */
    u8  u2, v2;     /* 12..13 */
    u16 _pad;       /* 14..15 */
    s16 vtx0_idx;   /* 16..17 */
    s16 vtx1_idx;   /* 18..19 */
    s16 vtx2_idx;   /* 20..21 */
    u8  data[6];    /* 22..27 — extra (varies) */
} polyHdr_FT3;

typedef struct {
    /* 36 bytes — polyHdr_FT4 (textured quad) */
    u8  code;
    u8  r0, g0, b0;
    u8  u0, v0;
    u16 clut;
    u8  u1, v1;
    u16 tpage;
    u8  u2, v2;
    u16 _pad0;
    u8  u3, v3;
    u16 _pad1;
    s16 vtx0_idx, vtx1_idx, vtx2_idx, vtx3_idx;  /* 20..27 */
    u8  data[8];                                 /* 28..35 */
} polyHdr_FT4;

/* Vertex base — array of pre-projected screen-space verts (stride 8) */
typedef struct {
    s16 x, y;       /* 0..3 — screen XY */
    s16 _pad[2];    /* 4..7 */
} libgs_vtx;

/* OT bucket type — opaque pointer that the HAL XOR-swap-prepends into. */
typedef u32 *ot_bucket_t;

/* ---- Per-primitive emit functions ----
 * Each takes the header, vertex base, current PACKET cursor, OT-bucket
 * pointer, and z-shift; returns the advanced PACKET cursor. The HAL
 * impl currently no-ops (returns out unchanged) — convert stubs at
 * leisure once the render path is ready. */

PACKET *hal_emit_poly_f3(void *hdr, libgs_vtx *vtxBase,
                         PACKET *out, ot_bucket_t ot, s32 shift);
PACKET *hal_emit_poly_ft3(void *hdr, libgs_vtx *vtxBase,
                          PACKET *out, ot_bucket_t ot, s32 shift);
PACKET *hal_emit_poly_g3(void *hdr, libgs_vtx *vtxBase,
                         PACKET *out, ot_bucket_t ot, s32 shift);
PACKET *hal_emit_poly_gt3(void *hdr, libgs_vtx *vtxBase,
                          PACKET *out, ot_bucket_t ot, s32 shift);
PACKET *hal_emit_poly_f4(void *hdr, libgs_vtx *vtxBase,
                         PACKET *out, ot_bucket_t ot, s32 shift);
PACKET *hal_emit_poly_ft4(void *hdr, libgs_vtx *vtxBase,
                          PACKET *out, ot_bucket_t ot, s32 shift);
PACKET *hal_emit_poly_g4(void *hdr, libgs_vtx *vtxBase,
                         PACKET *out, ot_bucket_t ot, s32 shift);
PACKET *hal_emit_poly_gt4(void *hdr, libgs_vtx *vtxBase,
                          PACKET *out, ot_bucket_t ot, s32 shift);

/* Batch processors: same as above but iterate `count` headers. */
PACKET *hal_emit_poly_f3_batch (void *hdr, libgs_vtx *vtxBase, PACKET *out,
                                u32 count, ot_bucket_t ot, s32 shift);
PACKET *hal_emit_poly_ft3_batch(void *hdr, libgs_vtx *vtxBase, PACKET *out,
                                u32 count, ot_bucket_t ot, s32 shift);
PACKET *hal_emit_poly_f4_batch (void *hdr, libgs_vtx *vtxBase, PACKET *out,
                                u32 count, ot_bucket_t ot, s32 shift);
PACKET *hal_emit_poly_ft4_batch(void *hdr, libgs_vtx *vtxBase, PACKET *out,
                                u32 count, ot_bucket_t ot, s32 shift);
PACKET *hal_emit_poly_gt4_batch(void *hdr, libgs_vtx *vtxBase, PACKET *out,
                                u32 count, ot_bucket_t ot, s32 shift);
PACKET *hal_emit_poly_gt3_batch(void *hdr, libgs_vtx *vtxBase, PACKET *out,
                                u32 count, ot_bucket_t ot, s32 shift);

#endif
