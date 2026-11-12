/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * gpu_poly.c — HAL polygon emit, no-op skeleton.
 *
 * Per Q4 analyst guidance: each Konami GTE-wrapper stub at 0x800A70D8..
 * 0x800B0080 should call into typed HAL emitters. This file provides the
 * landing zone so stubs can be converted incrementally.
 *
 * Current implementation: each HAL function does NOTHING (returns out
 * unchanged). That matches the existing behavior of every stub in
 * src/main/semi-matched/func_800A*. When the render path is ready,
 * fill these in with real GP0 packet building + OT linking.
 *
 * The point today is to give each stub a target signature so future
 * batches can convert them mechanically without touching this file.
 */
#include "hal_gpu_poly.h"

#define IMPL(NAME) \
    PACKET *NAME(void *hdr, libgs_vtx *vtxBase, PACKET *out, ot_bucket_t ot, s32 shift) { \
        (void)hdr; (void)vtxBase; (void)ot; (void)shift; \
        return out; \
    }

#define IMPL_BATCH(NAME) \
    PACKET *NAME(void *hdr, libgs_vtx *vtxBase, PACKET *out, u32 count, ot_bucket_t ot, s32 shift) { \
        (void)hdr; (void)vtxBase; (void)count; (void)ot; (void)shift; \
        return out; \
    }

IMPL(hal_emit_poly_f3)
IMPL(hal_emit_poly_ft3)
IMPL(hal_emit_poly_g3)
IMPL(hal_emit_poly_gt3)
IMPL(hal_emit_poly_f4)
IMPL(hal_emit_poly_ft4)
IMPL(hal_emit_poly_g4)
IMPL(hal_emit_poly_gt4)

IMPL_BATCH(hal_emit_poly_f3_batch)
IMPL_BATCH(hal_emit_poly_ft3_batch)
IMPL_BATCH(hal_emit_poly_f4_batch)
IMPL_BATCH(hal_emit_poly_ft4_batch)
IMPL_BATCH(hal_emit_poly_gt4_batch)
IMPL_BATCH(hal_emit_poly_gt3_batch)
