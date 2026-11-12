/* SPDX-License-Identifier: GPL-3.0-only */
/* gpu.c — GPU/Display HAL: software VRAM + SDL2 texture display
 *
 * Emulates the PS1's 1024x512 16-bit VRAM. All GPU operations (LoadImage,
 * ClearImage, etc.) operate on this buffer. Before every host present, the
 * active display region is uploaded to an SDL texture and rendered.
 *
 * PS1 pixel format: 1555 ABGR (1-bit semi-transparency, 5-5-5 BGR)
 *
 * Copyright (C) 2026 cdakotabrown and contributors.
 * Portions (the polygon rasteriser raster_xfp, raster_xfp_step,
 * raster_xfp_int, raster_delta, raster_span, raster_span_hires and
 * raster_triangle; the texture/CLUT cache; size limits) derived from
 * Mednafen's PSX GPU, (C) Mednafen Team, GPL-2.0-or-later.
 * Draw-time rules from DuckStation (C) Connor McLaughlin, GPL-3.0,
 * commit aa955b8ae.
 *
 * This file is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License, version 3, as
 * published by the Free Software Foundation (pc/LICENSE).  Because it
 * contains DuckStation-derived code, which DuckStation licenses under
 * GPL-3.0 without an "or later" grant, the file as a whole is GPL-3.0-only;
 * the rest of the PC port is GPL-3.0-or-later.  It is distributed WITHOUT
 * ANY WARRANTY; see the licence for details.
 */
#include "hal.h"
#include "gpu_display.h"
#include "gpu_reset.h"
#include "gpu_queue.h"
#include "gpu_cpu.h"
#include "gpu_clear.h"
#include "gpu_vram.h"
#include "gpu_texture.h"
#include "nbn_trace.h"
#include "cpu_cost.h"
#include "profile.h"
#include <SDL2/SDL.h>
#include <stdlib.h>
#include "../3rd/mednafen-gpu/shim/mdfn_gpu_shim.h"

/* MDFN_GPU routes GP0 primitives through the vendored Mednafen PSX
 * rasterizer shim.  Still opt-in (MDFN_GPU=1), but no longer because it
 * is unstable.
 *
 * HISTORY: an attempted default-on flip on 2026-04-20 regressed 197/211
 * headless sweep scenes (14-154 exit 126, 155-210 exit 139), every crash
 * landing at search_tpage+0xf8 fault=0xfe1000 regardless of SCENE -- which
 * read as the shim corrupting game globals.
 *
 * ROOT CAUSE (found 2026-08-22): it was corrupting globals, by writing past
 * g_vram[].  Upstream's PlotPixel masks Y (`y &= 511`) but ORs X straight
 * into the VRAM index, so nothing bounds the drawing area; a DRAWENV whose
 * clip rectangle ran past x=1023 walked off the end of the 1 MiB buffer into
 * whatever followed it.  mdfn_set_drawing_area now clamps to 1023x511, which
 * is what the GP0(E3h)/GP0(E4h) 10-bit/9-bit fields do on hardware anyway.
 *
 * STATUS 2026-08-22: full headless sweep SCENE=0..210 is 211/211 with
 * MDFN_GPU=1, and the per-scene crash-handler counts are IDENTICAL with it on
 * and off (scene 48: 8 both ways; scene 64: 4 both ways), so the crashes that
 * remain are pre-existing and unrelated to the rasterizer.  On scene 64 it
 * draws 153,717 non-black VRAM pixels vs 143,476 for the in-house path, with
 * 6.67% of VRAM differing -- i.e. it renders strictly more content.
 *
 * The shim's three dispatch gaps were closed on 2026-08-22: all five blend
 * modes, MaskEval_TA, and sprite FlipX/FlipY are now dispatched, and this file
 * feeds them the real state (see mdfn_blend_for and the GP0(E6h) handler).
 *
 * NEXT: the default flip is now gated only on VISUAL verification -- are the
 * extra pixels correct?  See docs/pc-port/PC-PORT-TODO.md item 1. */
static int g_use_mdfn_gpu = -1;
static int mdfn_gpu_enabled(void) {
#ifdef MDFN_GPU_BACKEND
    if (g_use_mdfn_gpu < 0) {
        const char *e = getenv("MDFN_GPU");
        g_use_mdfn_gpu = (e && *e && *e != '0') ? 1 : 0;
    }
    return g_use_mdfn_gpu;
#else
    /* The parity backend is not in the tree (THIRD-PARTY.md); the build has
     * no rasterizer to route to, so the switch cannot be on. */
    return 0;
#endif
}

/* ---- GP0 opcode tracer (Phase 1 of real-content plan) ----
 * GP0_TRACE=1 records every primitive code byte walked by DrawOTag plus
 * the VRAM-transfer ops (ClearImage=0x02, MoveImage=0x80, LoadImage=0xA0,
 * StoreImage=0xC0) and the handled/unhandled status of each primitive
 * opcode. Dumps a machine-readable histogram to $GP0_TRACE_LOG (default
 * gp0_trace.log) on process exit, tagged with $SCENE for sweep
 * aggregation. Zero overhead when gate is off.
 * GP0_TRACE_FRAME=N (with GP0_TRACE=1) also writes every command of the
 * Nth DrawOTag call to ./gp0_frame.log: opcode, word count, the texture
 * page/ABR, mask and drawing-offset state it executes under, and its words
 * (the first three of an upload).
 */
#include <signal.h>
#include <stdio.h>
static int g_gp0_trace = -1;
static unsigned long g_gp0_hist[256];
static unsigned long g_gp0_unhandled[256];
static unsigned long g_gp0_vram_clear, g_gp0_vram_move,
                     g_gp0_vram_load, g_gp0_vram_store,
                     g_gp0_setdrawmode;
/* Commands cut off by the end of their DMA packet (dropped, not drawn). */
static unsigned long g_gp0_incomplete;
/* Variable-size rectangles whose size word has bits above the hardware's
 * 10-bit width / 9-bit height (a negative "flip" width included), and E1
 * words with the texture-flip bits 12/13 set.  The HAL's negative-width
 * handling differs from hardware masking only when these are non-zero. */
static unsigned long g_gp0_rect_oversize, g_gp0_e1_flip;
/* Semi-transparent primitives seen, bucketed by the tpage ABR mode.
 * Proves whether a scene uses anything other than 50/50 blending. */
unsigned long g_blend_abr_hist[4];

static int gp0_trace_enabled(void) {
    if (g_gp0_trace < 0) {
        const char *e = getenv("GP0_TRACE");
        g_gp0_trace = (e && *e && *e != '0') ? 1 : 0;
    }
    return g_gp0_trace;
}
static const char *gp0_opcode_name(unsigned char code) {
    /* Names follow PS1 SDK primitive macros; first byte of P_TAG code. */
    if (code == 0x02) return "FILL_RECT";
    switch (code >= 0xE0 ? code : (code & 0xFC)) {
    case 0x00: return "NOP_OR_CACHE_CLEAR";
    case 0x20: return "POLY_F3";
    case 0x24: return "POLY_FT3";
    case 0x28: return "POLY_F4";
    case 0x2C: return "POLY_FT4";
    case 0x30: return "POLY_G3";
    case 0x34: return "POLY_GT3";
    case 0x38: return "POLY_G4";
    case 0x3C: return "POLY_GT4";
    case 0x40: return "LINE_F2";
    case 0x48: return "LINE_F_POLY";
    case 0x50: return "LINE_G2";
    case 0x58: return "LINE_G_POLY";
    case 0x60: return "TILE";
    case 0x64: return "SPRT";
    case 0x68: return "TILE_1";
    case 0x6C: return "TILE_VAR";
    case 0x70: return "TILE_8";
    case 0x74: return "SPRT_8";
    case 0x78: return "TILE_16";
    case 0x7C: return "SPRT_16";
    case 0xE0: return "DR_TPAGE";
    case 0xE1: return "DR_MODE";
    case 0xE2: return "DR_TWIN";
    case 0xE3: return "DR_AREA_TL";
    case 0xE4: return "DR_AREA_BR";
    case 0xE5: return "DR_OFFSET";
    case 0xE6: return "DR_MASK";
    default: return "UNKNOWN";
    }
}
static void gp0_trace_dump(void) {
    if (!gp0_trace_enabled()) return;
    const char *path = getenv("GP0_TRACE_LOG");
    if (!path || !*path) path = "gp0_trace.log";
    const char *scene = getenv("SCENE");
    if (!scene) scene = "boot";
    FILE *f = fopen(path, "a");
    if (!f) f = stderr;
    int op;
    for (op = 0; op < 256; op++) {
        if (g_gp0_hist[op] == 0) continue;
        fprintf(f, "[GP0_HIST] scene=%s op=0x%02X name=%s count=%lu unhandled=%lu\n",
                scene, op, gp0_opcode_name((unsigned char)op),
                g_gp0_hist[op], g_gp0_unhandled[op]);
    }
    fprintf(f, "[GP0_HIST] scene=%s semi_abr=[%lu,%lu,%lu,%lu] ", scene,
            g_blend_abr_hist[0], g_blend_abr_hist[1],
            g_blend_abr_hist[2], g_blend_abr_hist[3]);
    fprintf(f, "[GP0_HIST] scene=%s vram_clear=%lu vram_move=%lu "
               "vram_load=%lu vram_store=%lu setdrawmode=%lu incomplete=%lu "
               "rect_oversize=%lu e1_flip=%lu\n",
            scene, g_gp0_vram_clear, g_gp0_vram_move,
            g_gp0_vram_load, g_gp0_vram_store, g_gp0_setdrawmode,
            g_gp0_incomplete, g_gp0_rect_oversize, g_gp0_e1_flip);
    if (f != stderr) fclose(f);
}
static void gp0_trace_sig(int sig) { (void)sig; gp0_trace_dump(); _exit(128 + sig); }
static void gp0_trace_install(void) {
    static int installed = 0;
    if (installed) return;
    installed = 1;
    if (!gp0_trace_enabled()) return;
    atexit(gp0_trace_dump);
    signal(SIGTERM, gp0_trace_sig);
    signal(SIGINT, gp0_trace_sig);
}

/* Local P_TAG definition matching the PS1 layout */
typedef struct {
    unsigned addr : 24;
    unsigned len  :  8;
    u8 r0, g0, b0, code;
} GPU_P_TAG;

/* ---- OT Pointer Side Table ----
 * On PS1, P_TAG.addr is 24-bit and stores low bits of a 32-bit pointer.
 * On 64-bit PC, we can't fit pointers in 24 bits. Solution: store full
 * pointers in a hash table keyed by the P_TAG's own address.
 *
 * This is only used by addPrim/getaddr/setaddr macros. The OT and
 * primitive buffers are small (~200KB total), so a simple hash works.
 */
#define OT_HASH_SIZE 131072
#define OT_HASH_MASK (OT_HASH_SIZE - 1)
static struct { void *key; void *val; } g_ot_ptrs[OT_HASH_SIZE];

static unsigned int ot_hash(void *p) {
    unsigned long v = (unsigned long)p;
    return (unsigned int)((v >> 2) ^ (v >> 18)) & OT_HASH_MASK;
}

static int g_setaddr_count = 0;
static int g_addprim_count = 0;
unsigned long g_ot_setaddr_calls = 0;   /* OT_TRACE: total setaddr calls */

void pc_ot_setaddr(void *p, void *addr) {
    unsigned int h = ot_hash(p);
    /* Linear probe — find slot for this key */
    int i;

    g_setaddr_count++;
    g_ot_setaddr_calls++;

    for (i = 0; i < 64; i++) {
        unsigned int idx = (h + i) & OT_HASH_MASK;
        if (g_ot_ptrs[idx].key == p || g_ot_ptrs[idx].key == NULL) {
            g_ot_ptrs[idx].key = p;
            g_ot_ptrs[idx].val = addr;
            /* Also store truncated value in the actual P_TAG for compatibility */
            ((GPU_P_TAG *)p)->addr = (unsigned long)addr & 0xFFFFFF;
            return;
        }
    }
    /* Table full — just store truncated (won't work but won't crash) */
    printf("[OT] WARNING: hash table full for key %p!\n", p);
    ((GPU_P_TAG *)p)->addr = (unsigned long)addr & 0xFFFFFF;
}

/* Raw DMA links are PS1 physical addresses, not truncated host pointers. */
static void *ot_ptr_from_link(u32 link) {
    if (link == 0 || link == 0xFFFFFFu)
        return (void *)(uintptr_t)link;
    return (void *)(uintptr_t)(0x80000000u | link);
}

void *pc_ot_getaddr(void *p) {
    unsigned int h = ot_hash(p);
    u32 link = ((GPU_P_TAG *)p)->addr;
    int i;
    for (i = 0; i < 64; i++) {
        unsigned int idx = (h + i) & OT_HASH_MASK;
        if (g_ot_ptrs[idx].key == p) {
            /* Matched PS1 producers also write raw links directly. Once
             * observed, discard the obsolete full-host association without
             * removing its key (which would break the hash probe chain). */
            if (((uintptr_t)g_ot_ptrs[idx].val & 0xFFFFFFu) != link)
                g_ot_ptrs[idx].val = ot_ptr_from_link(link);
            return g_ot_ptrs[idx].val;
        }
        if (g_ot_ptrs[idx].key == NULL) break;
    }
    return ot_ptr_from_link(link);
}
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* 1024x512 16-bit VRAM (non-static so debug.c can access for screenshots) */
#define VRAM_WIDTH  1024
#define VRAM_HEIGHT 512
u16 g_vram[VRAM_HEIGHT][VRAM_WIDTH];

/* Internal resolution scale (pc/hal/gpu_hires.h).  scale 1 = off: every
 * hook below is a not-taken branch and nothing else is touched.  Only
 * gpu_hires_init (pc/hal/gpu_hires.c, weak here so the GPU harnesses can
 * link this file alone) ever raises it. */
#include "gpu_hires.h"
GpuHiresState g_hires = { 1 };
extern void gpu_hires_init(void) __attribute__((weak));
extern void gpu_hires_mirror_rect(int x, int y, int w, int h) __attribute__((weak));
extern void gpu_hires_move(int sx, int sy, int w, int h, int dx, int dy) __attribute__((weak));
extern const unsigned int *gpu_hires_pack_display(int *width, int *height) __attribute__((weak));
#define HIRES_ON() (g_hires.scale > 1)

/* Sub-pixel vertex precision for the shadow (pc/hal/gpu_pgxp.h; enhanced
 * GPU_PGXP).  gpu_draw_primitive looks the polygon's vertex words up in the
 * GTE's fraction table; when all of them hit, g_pgxp_prim holds each
 * vertex's native position (as the rasterizer receives it: decoded, plus
 * the drawing offset) and its fractions, and the shadow pass of every
 * triangle of that polygon places its vertices at N * x + (fx * N >> 16).
 * The native pass never reads it.  Weak: harnesses link this file alone. */
#include "gpu_pgxp.h"
#pragma weak g_gpu_pgxp_active
#pragma weak g_gpu_pgxp_stats
#pragma weak gpu_pgxp_lookup
#pragma weak gpu_pgxp_lookup_tri
#pragma weak gpu_pgxp_list_done
#define PGXP_ON() (HIRES_ON() && &g_gpu_pgxp_active && g_gpu_pgxp_active)
static struct {
    int n;                  /* 0: draw the shadow from integers */
    s32 x[4], y[4];
    u32 fx[4], fy[4];
} g_pgxp_prim;
/* The triangle whose native pass is running had its shadow drawn from
 * fractions (hires_native_stored). */
static int g_pgxp_tri;

/* Texture dumping and replacement packs (pc/hal/gpu_texrep.h).  Both flags
 * stay 0 unless gpu_texrep_init (weak: the harnesses link this file alone)
 * finds the profile switch on and a dump or a pack asked for. */
#include "gpu_texrep.h"
int g_texrep_on, g_texrep_replace;
static int g_texrep_prim;     /* the current shadow primitive samples a pack */
extern void gpu_texrep_init(void) __attribute__((weak));
extern void gpu_texrep_upload(int x, int y, int w, int h) __attribute__((weak));
extern void gpu_texrep_move(int sx, int sy, int w, int h, int dx, int dy) __attribute__((weak));
extern void gpu_texrep_note_draw(unsigned short clut, unsigned short tpage,
                                 int u0, int v0, int u1, int v1) __attribute__((weak));
extern int gpu_texrep_prim_begin(unsigned short clut, unsigned short tpage) __attribute__((weak));
extern int gpu_texrep_sample(unsigned u, unsigned v, unsigned fu, unsigned fv,
                             unsigned int *rgba) __attribute__((weak));
extern int gpu_texrep_texel(unsigned u, unsigned v, GpuTexrepTexel *t) __attribute__((weak));

/* The extras' gallery (pc/hal/extras_gallery.c, enhanced TOKI_EXTRAS):
 * bit 0 = it watches uploads, bit 1 = draws (only while a picture it saw
 * loaded waits to be shown).  0 unless the extras are on: every hook is a
 * not-taken branch.  Both hooks only read. */
int g_extras_gpu;
extern void extras_gallery_upload(int x, int y, int w, int h, const void *data) __attribute__((weak));
extern void extras_gallery_draw(unsigned tpage, int u0, int v0, int u1, int v1) __attribute__((weak));
/* A GP0(02h)-style fill (gpu_vram_fill's rect) mirrored into the shadow. */
static void hires_mirror_fill(u32 position, u32 size)
{
    if (!HIRES_ON()) return;
    gpu_hires_mirror_rect((int)(position & 0x3F0u), (int)((position >> 16) & 511u),
                          (int)(((size & 1023u) + 15u) & ~15u), (int)((size >> 16) & 511u));
}

/* Display state (non-static for debug.c access) */
s32 g_disp_x, g_disp_y, g_disp_w, g_disp_h, g_disp_rgb24;
s32 g_draw_x, g_draw_y, g_draw_w = 320, g_draw_h = 240;
/* GP0 E3/E4/E5 device words, independent of the SDK environment cache. */
static u32 g_area_tl, g_area_br, g_draw_offset;
static SDL_Texture *g_display_tex;
static s32 g_disp_mask = 1;

/* Current texture page state (non-static for debug.c access) */
s32 g_tpage_x, g_tpage_y, g_tpage_bpp, g_tpage_abr;

/* GP0(E6h) DR_MASK state: bit 0 forces the mask bit set on every pixel
 * written, bit 1 makes the GPU skip pixels whose destination mask bit is
 * already set.  Tracked here and handed to the rasterizer; previously
 * neither the opcode nor the state existed, so mask-eval was always off. */
static s32 g_mask_set_or = 0;
static s32 g_mask_eval_and = 0;
static int g_dither_enabled = 0;

/* Texture/CLUT cache events; definitions and policy with texel_fetch. */
static void gpu_set_texpage_state(s32 x, s32 y, s32 bpp, s32 abr);
static void gpu_cache_clear(void);
static void gpu_vram_transfer_started(void);

static void gpu_backend_tpage(u16 page) {
    mdfn_set_texpage((page & 0x1FFu) |
                    (g_dither_enabled ? 0x200u : 0));
}



/* Primitive counters from debug.c */
extern int g_frame_prim_count;
extern int g_frame_sprt_count;
extern int g_frame_tile_count;
extern int g_frame_poly_count;
extern int g_frame_dr_mode_count;

/* Set default texture page for scenes that don't properly initialize it */
void gpu_set_default_tpage(s32 x, s32 y, s32 bpp) {
    static int trace = 0;
    gpu_set_texpage_state(x, y, bpp, g_tpage_abr);
    if (trace < 3) {
        printf("[GPU] gpu_set_default_tpage(%d, %d, %d)\n", x, y, bpp);
        trace++;
    }
}

extern SDL_Renderer *hal_get_renderer(void);

/* Convert PS1 1555 ABGR to RGBA8888 for SDL */
static u32 psx_to_rgba(u16 psx) {
    u32 r = (psx & 0x001F) << 3;
    u32 g = ((psx >> 5) & 0x1F) << 3;
    u32 b = ((psx >> 10) & 0x1F) << 3;
    return (r << 24) | (g << 16) | (b << 8) | 0xFF;
}

/*
 * GPU callback: this is the function that graph_jtb->func_ptr points to.
 * It gets called by LoadImage as: func_ptr(field_20, rect_ptr, mode, data_ptr)
 * mode=8 means LoadImage (write data to VRAM).
 * mode=6 means StoreImage (read VRAM to buffer).
 * mode=5 means ClearImage.
 */
static s32 gpu_callback(s32 field_20, s32 rect_ptr, s32 mode, s32 data_ptr) {
    /* Cast through u32 first to force zero-extension. PS1 stores host
     * pointers as s32 via `(s32)&D_800CXXXX`, which truncates the high
     * bits of a host address whose low 32 bits are 0x800CXXXX. A direct
     * (long)s32 cast sign-extends → kernel-space fault. */
    RECT *rect = (RECT *)(uintptr_t)(u32)rect_ptr;
    u32 *data  = (u32  *)(uintptr_t)(u32)data_ptr;
    (void)field_20;

    if (!rect) return 0;

    switch (mode) {
    case 8: /* LoadImage */
        if (data) LoadImage(rect, data);
        break;
    case 6: /* StoreImage */
        if (data) StoreImage(rect, data);
        break;
    case 5: /* ClearImage — data_ptr holds packed RGB */
        {
            u8 r = data_ptr & 0xFF;
            u8 g = (data_ptr >> 8) & 0xFF;
            u8 b = (data_ptr >> 16) & 0xFF;
            ClearImage(rect, r, g, b);
        }
        break;
    default:
        break;
    }
    return 0;
}

/*
 * GPU state struct that graph_jtb points to (Sony SPU/SEQ driver context).
 *
 * Different decomp files define incompatible struct overlays. We create a
 * large backing array and fill ALL possible function pointer offsets with
 * valid stubs to prevent null dereferences regardless of which struct
 * overlay accesses graph_jtb.
 *
 * Known 64-bit offsets from various struct definitions:
 *   0x04: func_04 (Struct_800A0488)
 *   0x08: func_ptr/func (multiple structs)
 *   0x10: func_10/func16 (AudioStruct, SStruct)
 *   0x2C: func_2C/func_28 (ClearOTagR, AudioStruct on 64-bit)
 *   0x38: func_34/func (SeqStruct, ContRpn2)
 *   0x3C: func_3C (Struct_8009FEA8)
 *
 * Data fields (filled with 0 — they're read-only in these contexts):
 *   0x1C: field_18 (64-bit offset)
 *   0x20: value (StoreImage)
 *   0x24: field_20 (SomeStruct_800A00E0 64-bit offset)
 */
typedef struct {
    u8 data[96];  /* Raw backing storage — larger to cover all offsets */
} GPUState;

/* The function-pointer slots, stored with memcpy: the offsets are the
 * PS1 struct's 4-byte ones, so an 8-byte slot is not 8-byte aligned (a
 * plain pointer store there is undefined behaviour; UBSan explorer runs,
 * 2026-11-01).  The bytes written are the same, in the same order. */
static void gpu_func_set(GPUState *s, unsigned off, s32 (*fn)())
{
    memcpy(&s->data[off], &fn, sizeof(fn));
}
#define GPU_SET_04(s, f) gpu_func_set((s), 0x04, (s32 (*)())(f))
#define GPU_SET_08(s, f) gpu_func_set((s), 0x08, (s32 (*)())(f))
#define GPU_SET_10(s, f) gpu_func_set((s), 0x10, (s32 (*)())(f))
#define GPU_SET_2C(s, f) gpu_func_set((s), 0x2C, (s32 (*)())(f))
#define GPU_SET_38(s, f) gpu_func_set((s), 0x38, (s32 (*)())(f))
#define GPU_SET_3C(s, f) gpu_func_set((s), 0x3C, (s32 (*)())(f))
/* On 64-bit, struct field after `s32 pad[15]` (60 bytes) is padded to
 * offset 0x40 due to 8-byte alignment of the function pointer. Match
 * by also installing the same stub at 0x40 so callers reading the
 * 64-bit-laid-out struct see the same handler at the offset their
 * compiler computed. */
#define GPU_SET_40(s, f) gpu_func_set((s), 0x40, (s32 (*)())(f))

static GPUState g_gpu_state;

/* Forward declaration for use in hal_gpu_init */
void ClearImage(RECT *rect, u8 r, u8 g, u8 b);

/* Stub callbacks for SEQ/SPU context functions that aren't GPU-related.
 * These get called during sound processing but don't need real implementation
 * since we use TinySoundFont for audio. Return 0 to satisfy callers. */
static s32 stub_func_noop(void) { return 0; }
static s32 stub_func_1arg(s32 arg) { (void)arg; return 0; }
static s32 stub_func_vararg(s32 a, ...) { (void)a; return 0; }

void hal_gpu_init(void) {
    SDL_Renderer *renderer = hal_get_renderer();
    gp0_trace_install();
    if (renderer) {
        g_display_tex = SDL_CreateTexture(renderer,
            SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING,
            320, 240);
    }
    memset(g_vram, 0, sizeof(g_vram));

    /* Host bootstrap runs before the game's SDK limits are initialized.
     * These two host buffers use the raw data plane, not an SDK request. */
    gpu_vram_fill(&g_vram[0][0], 0x400000u, 0, (240u << 16) | 320u);
    gpu_vram_fill(&g_vram[0][0], 0x400000u, 240u << 16,
                  (240u << 16) | 320u);

    g_disp_x = 0; g_disp_y = 0;
    g_disp_w = 320; g_disp_h = 240;
    g_draw_x = 0; g_draw_y = 0;
    g_draw_w = 320; g_draw_h = 240;

    /* Initialize the GPU/SEQ callback state — this is what graph_jtb points to.
     * Fill ALL function pointer slots with valid stubs to prevent crashes. */
    memset(&g_gpu_state, 0, sizeof(g_gpu_state));
    GPU_SET_04(&g_gpu_state, stub_func_vararg); /* Struct_800A0488 */
    GPU_SET_08(&g_gpu_state, gpu_callback);     /* GPU image operations */
    GPU_SET_10(&g_gpu_state, stub_func_1arg);   /* SEQ: key control / func16 */
    GPU_SET_2C(&g_gpu_state, stub_func_vararg); /* ClearOTagR func_2C */
    GPU_SET_38(&g_gpu_state, stub_func_noop);   /* SeqStruct func_34 / ContRpn2 */
    GPU_SET_3C(&g_gpu_state, stub_func_1arg);   /* DrawSync DrawSync (32-bit layout) */
    GPU_SET_40(&g_gpu_state, stub_func_1arg);   /* same — for 64-bit padded struct (Struct_8009FEA8) */

    /* Set graph_jtb to point to our GPU state.  (It used to be written into
     * the retail data blob too, at offset 0x5DA44; nothing read it there,
     * and the blob is gone: the binary carries no retail data.) */
    {
        extern void *graph_jtb;
        /* Real 64-bit pointer storage — callers via SomeStruct_A0070 *
         * read 8 bytes. Don't truncate. */
        graph_jtb = &g_gpu_state;
    }

    printf("[HAL/GPU] VRAM initialized (%dx%d, 16-bit)\n", VRAM_WIDTH, VRAM_HEIGHT);
    printf("[HAL/GPU] GPU callback registered at graph_jtb\n");

    if (mdfn_gpu_enabled()) {
        mdfn_gpu_init(&g_vram[0][0]);
        mdfn_set_drawing_area(g_draw_x, g_draw_y,
                              g_draw_x + g_draw_w - 1,
                              g_draw_y + g_draw_h - 1);
        mdfn_set_draw_offset(0, 0);
        printf("[HAL/GPU] MDFN_GPU=1: Mednafen shim active\n");
    } else {
        /* the native rasterizer only; the Mednafen backend has no shadow.
         * Textures first: a pack can raise the shadow's scale. */
        if (gpu_texrep_init) gpu_texrep_init();
        if (gpu_hires_init) gpu_hires_init();
    }
}

/* The drawing area (E3h/E4h) as gpu_raster_store clips to it. */
void gpu_get_draw_area(int *x, int *y, int *w, int *h) {
    *x = (int)(g_area_tl & 1023u);
    *y = (int)((g_area_tl >> 10) & 1023u);
    *w = g_draw_w;
    *h = g_draw_h;
}

void gpu_get_scanout_state(GpuScanoutState *state) {
    state->x = g_disp_x;
    state->y = g_disp_y;
    state->width = g_disp_w;
    state->height = g_disp_h;
    state->rgb24 = g_disp_rgb24;
    state->enabled = g_disp_mask;
}

/* The presenter (pc/hal/present.c): the scale filter and layout before the
 * scanout is copied, scanlines / toasts after.  Weak: harnesses that build
 * this file without it present exactly as before. */
extern void pc_present_frame(SDL_Renderer *r, SDL_Texture *t) __attribute__((weak));
extern void pc_present_overlay(SDL_Renderer *r) __attribute__((weak));
/* The presenter's CRT filter (presentation only): the pixels to upload
 * (the picture itself, or its gamma-adjusted copy), then either it draws
 * the picture (1) or leaves the plain copy below to this file (0, also
 * when it is off). */
extern const unsigned int *pc_present_crt_picture(SDL_Renderer *r, const unsigned int *px,
                                                  int w, int h, int stride) __attribute__((weak));
extern int pc_present_crt(SDL_Renderer *r, SDL_Texture *t, const unsigned int *px,
                          int w, int h, int stride) __attribute__((weak));
#define CRT_UPLOAD(px, w, h) \
    (pc_present_crt_picture ? pc_present_crt_picture(renderer, (px), (w), (h), (w)) : (px))

/* Debug: show full VRAM (toggled with Shift+F11) */
static int g_show_full_vram = 0;

void gpu_toggle_vram_view(void) {
    g_show_full_vram = !g_show_full_vram;
    printf("[GPU] VRAM view: %s\n", g_show_full_vram ? "FULL (1024x512)" : "DISPLAY");
}

/* Upload display region (or full VRAM) to screen */
void gpu_display(void) {
    SDL_Renderer *renderer = hal_get_renderer();
    if (!renderer) return;
    /* SDL invalidates the backbuffer after each present. A disabled PS1
     * display emits black; it does not preserve a previous host buffer. */
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    if (!g_display_tex || !g_disp_mask) {
        if (pc_present_overlay) pc_present_overlay(renderer);   /* toasts on a blank screen */
        return;
    }

    if (g_show_full_vram) {
        /* Show entire 1024x512 VRAM scaled to window */
        static SDL_Texture *vram_tex = NULL;
        static u32 vram_pixels[1024 * 512];
        s32 i, j;
        if (!vram_tex) {
            vram_tex = SDL_CreateTexture(renderer,
                SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING,
                1024, 512);
        }
        for (j = 0; j < 512; j++) {
            for (i = 0; i < 1024; i++) {
                vram_pixels[j * 1024 + i] = psx_to_rgba(g_vram[j][i]);
            }
        }
        SDL_UpdateTexture(vram_tex, NULL, vram_pixels, 1024 * 4);
        if (pc_present_frame) pc_present_frame(renderer, vram_tex);
        SDL_RenderCopy(renderer, vram_tex, NULL, NULL);
    } else {
        GpuScanoutState state;
        u32 pixels[GPU_SCANOUT_PIXELS];
        int width, height;

        const unsigned int *hires = NULL;
        static SDL_Texture *hires_tex;
        static int hires_w, hires_h;

        if (HIRES_ON()) hires = gpu_hires_pack_display(&width, &height);
        if (hires) {
            /* The N x display area into the same 320x240 logical space:
             * SDL samples it at the window's real resolution. */
            if (!hires_tex || hires_w != width || hires_h != height) {
                if (hires_tex) SDL_DestroyTexture(hires_tex);
                hires_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                    SDL_TEXTUREACCESS_STREAMING, width, height);
                hires_w = width;
                hires_h = height;
            }
        }
        if (hires && hires_tex) {
            SDL_UpdateTexture(hires_tex, NULL, CRT_UPLOAD(hires, width, height), width * 4);
            if (pc_present_frame) pc_present_frame(renderer, hires_tex);
            if (!(pc_present_crt && pc_present_crt(renderer, hires_tex, hires, width, height, width)))
                SDL_RenderCopy(renderer, hires_tex, NULL, NULL);
        } else {
            gpu_get_scanout_state(&state);
            if (!gpu_scanout_pack(&state, &g_vram[0][0], pixels,
                                  GPU_SCANOUT_PIXELS, &width, &height)) return;
            SDL_UpdateTexture(g_display_tex, NULL, CRT_UPLOAD(pixels, width, height), width * 4);
            if (pc_present_frame) pc_present_frame(renderer, g_display_tex);
            if (!(pc_present_crt && pc_present_crt(renderer, g_display_tex, pixels, width, height, width)))
                SDL_RenderCopy(renderer, g_display_tex, NULL, NULL);
        }
    }

    /* The retail scanout stays untouched unless F12 enables the HUD. */
    if (debug_overlay_is_active()) {
        extern u8 now_step;
        extern u8 now_sub_step;
        extern volatile int g_crash_count;
        extern const char *overlay_get_name(u8);

        const char *ovl_name = overlay_get_name(now_step);
        char hud_line1[128];
        SDL_Rect hud_bg = {0, 228, 320, 12};

        snprintf(hud_line1, sizeof(hud_line1), "S:%d/%d [%s] C:%d",
                 now_step, now_sub_step, ovl_name, g_crash_count);

        /* Semi-transparent background bar at bottom */
        SDL_SetRenderDrawColor(renderer, 0, 0, 40, 200);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_RenderFillRect(renderer, &hud_bg);

        /* Render text as colored rectangles (3x5 pixel "chars") */
        {
            int cx, cy = 230;
            SDL_SetRenderDrawColor(renderer, 200, 255, 200, 255);
            for (cx = 0; hud_line1[cx] && cx < 60; cx++) {
                int ch = hud_line1[cx];
                if (ch > 32 && ch < 127) {
                    SDL_Rect dot = {2 + cx * 5, cy, 4, 7};
                    SDL_RenderFillRect(renderer, &dot);
                }
            }
        }
    }
    if (pc_present_overlay) pc_present_overlay(renderer);
}

/* --- PsyQ GPU API implementations --- */

u32 GetTPage(s32 tp, s32 abr, s32 x, s32 y);

DRAWENV *SetDefDrawEnv(DRAWENV *env, s32 x, s32 y, s32 w, s32 h) {
    HAL_SDK_COST("SetDefDrawEnv");
    if (!env) return NULL;
    /* The SDK initializes the scalar fields, leaving reserved packet
     * storage intact until SetDrawEnv rebuilds its live words. */
    memset(env, 0, 28);
    env->clip.x = x; env->clip.y = y;
    env->clip.w = w; env->clip.h = h;
    env->ofs[0] = x; env->ofs[1] = y;
    env->tpage = GetTPage(0, 0, 640, 0);
    env->dtd = 1;
    env->dfe = h != 480;
    return env;
}

DISPENV *SetDefDispEnv(DISPENV *env, s32 x, s32 y, s32 w, s32 h) {
    HAL_SDK_COST("SetDefDispEnv");
    if (!env) return NULL;
    memset(env, 0, sizeof(*env));
    env->disp.x = x; env->disp.y = y;
    env->disp.w = w; env->disp.h = h;
    return env;
}

extern void SetDrawEnv(u32 *packet, DRAWENV *env);
extern void hal_cpu_spend(u32 ticks);
static int gpu_draw_command(u32 command);
static u32 gpu_clear_tile(u32 color, u32 position, u32 size);

static s32 gpu_signed11(u32 value) {
    s32 coordinate = value & 0x7FFu;
    return coordinate >= 0x400 ? coordinate - 0x800 : coordinate;
}


/* PutDrawEnv's CPU work (reference Interpreter, warm; the 1,173
 * vblank_function calls of a 2,400-frame no-input boot): SetDrawEnv's
 * packet build and _addque2's entry reach set_alarm at 742 and
 * SetIntrMask(0) at 901 (880..901 by DRAWENV); the masked dispatch takes
 * 257 and the return 285, 1,443 in all. */
#define PUTDRAWENV_TO_ALARM   742u
#define PUTDRAWENV_TO_MASK    159u
#define PUTDRAWENV_MASKED     257u
#define PUTDRAWENV_TO_RETURN  285u

DRAWENV *PutDrawEnv(DRAWENV *env) {
    u32 i, count, ticket;
    s32 clip_x0, clip_y0, clip_x1, clip_y1;
    unsigned long long mark;
    if (!env) return NULL;
    /* The 742 ticks to set_alarm include the matched SetDrawEnv and its
     * get_cs .. get_tw calls (retail: SetDrawEnv 31 ticks after entry,
     * _addque2 at 727), which charge their own rows: charge the rest. */
    mark = hal_cost_mark();
    SetDrawEnv(env->dr_env, env);
    gpu_cpu_submit_entry(hal_cost_net(PUTDRAWENV_TO_ALARM, mark), PUTDRAWENV_TO_MASK);
    ticket = gpu_queue_begin();
    env->dr_env[0] = (env->dr_env[0] & 0xFF000000u) | 0xFFFFFFu;
    /* PC-PORT FIX: Use ofs[2] as draw offset, clip as clipping area.
     * Previously used clip.x/y as offset which was wrong - clip defines
     * where primitives are allowed to draw, ofs defines the coordinate shift. */
    clip_x0 = env->dr_env[1] & 0x3FF;
    clip_y0 = (env->dr_env[1] >> 10) & 0x3FF;
    clip_x1 = env->dr_env[2] & 0x3FF;
    clip_y1 = (env->dr_env[2] >> 10) & 0x3FF;
    g_draw_x = gpu_signed11(env->dr_env[3]);
    g_draw_y = gpu_signed11(env->dr_env[3] >> 11);
    g_draw_w = clip_x1 >= clip_x0 ? clip_x1 - clip_x0 + 1 : 0;
    g_draw_h = clip_y1 >= clip_y0 ? clip_y1 - clip_y0 + 1 : 0;
    /* Apply texture page state from DRAWENV */
    gpu_set_texpage_state(env->tpage & 0xF, (env->tpage >> 4) & 1,
                          (env->tpage >> 7) & 3, (env->tpage >> 5) & 3);
    g_dither_enabled = env->dtd != 0;
    if (mdfn_gpu_enabled()) {
        /* Clipping area uses clip rectangle, draw offset is separate */
        mdfn_set_drawing_area(clip_x0, clip_y0, clip_x1, clip_y1);
        mdfn_set_draw_offset(g_draw_x, g_draw_y);
        gpu_backend_tpage(env->tpage);
        mdfn_set_mask(g_mask_set_or, g_mask_eval_and);
    }
    {
        static int pde_trace = 0;
        if (pde_trace < 10) {
            printf("[GPU] PutDrawEnv: clip=(%d,%d,%d,%d) ofs=(%d,%d) tpage=0x%04X → (%d,%d,bpp=%d)\n",
                   env->clip.x, env->clip.y, env->clip.w, env->clip.h,
                   env->ofs[0], env->ofs[1],
                   env->tpage, g_tpage_x, g_tpage_y, g_tpage_bpp);
            pde_trace++;
        }
    }
    /* Consume actual state words and the optional raw background clear. */
    count = env->dr_env[0] >> 24;
    for (i = 1; i <= count; i++) {
        u32 command = env->dr_env[i];
        if (gpu_draw_command(command)) continue;
        if ((command >> 24) == 0x02 && i + 2 <= count) {
            gpu_vram_fill(&g_vram[0][0], command,
                          env->dr_env[i + 1], env->dr_env[i + 2]);
            hires_mirror_fill(env->dr_env[i + 1], env->dr_env[i + 2]);
            i += 2;
        } else if ((command >> 24) == 0x60 && i + 2 <= count) {
            gpu_clear_tile(command, env->dr_env[i + 1], env->dr_env[i + 2]);
            i += 2;
        }
    }
    gpu_cache_draw_env(env);
    gpu_queue_end(ticket);
    /* The return's 285 is vblank_function's tail; at the boot, its caller's
     * measured one (cpu_cost.c hal_cpu_cost_tail, CALL_CONTINUATION). */
    gpu_cpu_masked(PUTDRAWENV_MASKED,
                   hal_cpu_cost_tail ? hal_cpu_cost_tail("PutDrawEnv@tail",
                                           __builtin_return_address(0),
                                           PUTDRAWENV_TO_RETURN)
                                     : PUTDRAWENV_TO_RETURN);
    return env;
}

extern void *gpu_put_disp_caller;
DISPENV *PutDispEnv(DISPENV *env) {
    if (!env) return NULL;
    gpu_put_disp_caller = __builtin_return_address(0);
    g_disp_x = env->disp.x; g_disp_y = env->disp.y;
    g_disp_w = env->disp.w; g_disp_h = env->disp.h;
    g_disp_rgb24 = env->isrgb24 != 0;
    {
        static int pde_trace = 0;
        if (pde_trace < 10) {
            printf("[GPU] PutDispEnv: disp=(%d,%d,%d,%d)\n",
                   env->disp.x, env->disp.y, env->disp.w, env->disp.h);
            pde_trace++;
        }
    }
    gpu_apply_disp_env(env);
    return env;
}

/* SetDispMask's CPU work (same probe, 1,171 calls): 69 ticks to the
 * GP1(03h) write, 31 more to return. */
void SetDispMask(s32 mask) {
    hal_cpu_spend(69u);
    gpu_control_shadow(mask ? 0x03000000u : 0x03000001u);
    g_disp_mask = mask;
    hal_cpu_spend(31u);
}

s32 DrawSync(s32 mode) {
    static int ds_trace = 0;
    if (ds_trace < 10) {
        printf("[GPU] DrawSync called (mode=%d)\n", mode);
        ds_trace++;
    }

    /* GPU completion is not scanout. VSync's host present refreshes VRAM
     * even when movies issue LoadImage without an intervening DrawSync. */
    return gpu_queue_sync(mode);
}

/* pc_ptr_is_mapped: hal.h (unsigned long, as every caller declares it) */

static int g_mainrom_loaded = 0;
/* libgpu ClearImage's CPU work (checkRECT, _addque2, set_alarm, _clr and
 * _cwc when the GPU is idle), measured like LoadImage below: entry to the
 * caller's next call 678 ticks (206 of 304 calls in the new_game probe,
 * all 66 of the boot's vram_area_clear strips), 317..337 when the request
 * is queued behind a busy GPU.  The port charged nothing: the boot's
 * 64-strip VRAM clear ran ~23,000 ticks short.  GPU_CLEARIMAGE_CPU=0 is
 * the negative control. */
#define CLEARIMAGE_CPU_IMMEDIATE 678u
#define CLEARIMAGE_CPU_TO_CWC    560u   /* entry -> _cwc, which writes the fill */
#define CLEARIMAGE_CPU_QUEUED    337u
extern int gpu_queue_device_busy(void);
extern void hal_cpu_charge(unsigned ticks) __attribute__((weak));
static int clearimage_cpu_on(void) {
    static int on = -1;
    if (on < 0) {
        on = hal_model_on("GPU_CLEARIMAGE_CPU");
    }
    return on;
}
/* GPU_CLEARIMAGE_NET: gpu_clear_image (pc/hal/gpu_clear.c) already spends
 * the SDK prefix up to SetIntrMask(0), 245 ticks, before it submits, so the
 * charges above counted that prefix twice and the fill started 805 ticks
 * after ClearImage's entry.  On the reference (the boot's 64-strip
 * vram_area_clear, probes on ClearImage and on _cwc's DMA2 CHCR store
 * 0x800A1DE8) the fill's DMA starts 600 ticks after the entry and the
 * caller's DrawSync follows 78 later (678 in all, as above): the strip
 * period was 2,077 ticks against retail's 1,907.  Now the charges are the
 * measured spans less that prefix.  GPU_CLEARIMAGE_NET=0 is the negative
 * control (the double charge). */
#define CLEARIMAGE_CPU_TO_FILL   600u
#define CLEARIMAGE_SDK_PREFIX    245u   /* gpu_clear_image's own spends */
static int clearimage_net_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("GPU_CLEARIMAGE_NET");
        on = !(e && e[0] == '0');
    }
    return on;
}
void ClearImage(RECT *rect, u8 r, u8 g, u8 b) {
    unsigned after = 0;
    if (hal_cpu_charge && clearimage_cpu_on()) {
        if (gpu_queue_device_busy()) {
            hal_cpu_charge(clearimage_net_on() ?
                           CLEARIMAGE_CPU_QUEUED - CLEARIMAGE_SDK_PREFIX :
                           CLEARIMAGE_CPU_QUEUED);
        } else if (clearimage_net_on()) {
            hal_cpu_charge(CLEARIMAGE_CPU_TO_FILL - CLEARIMAGE_SDK_PREFIX);
            after = CLEARIMAGE_CPU_IMMEDIATE - CLEARIMAGE_CPU_TO_FILL;
        } else {
            /* the fill starts at _cwc's write, 560 ticks in; 118 after it */
            hal_cpu_charge(CLEARIMAGE_CPU_TO_CWC);
            after = CLEARIMAGE_CPU_IMMEDIATE - CLEARIMAGE_CPU_TO_CWC;
        }
    }
    rect = (RECT *)hal_ps1_ptr(rect);
    if (!rect || !pc_ptr_is_mapped(rect, sizeof(*rect))) return;
    if (gp0_trace_enabled()) g_gp0_vram_clear++;
    gpu_clear_image(rect, (u32)r | ((u32)g << 8) | ((u32)b << 16));
    if (after) hal_cpu_charge(after);
}

/* libgpu LoadImage's CPU work (checkRECT, _addque2, and _dws when the GPU
 * is idle), measured on the reference emulator (Interpreter; the new_game.txt
 * phase probed on every main-binary entry, exceptions excluded): entry to
 * the caller's next call 989 ticks (median, p10 989 / p90 1,019, n 913)
 * when it runs the transfer at once (the caller's DrawSync follows); 367
 * (297 / 405, n 1,440) when the GPU is busy and the request is queued for
 * the DMA callback.  The port charged nothing: an 11-glyph name-entry kana
 * frame lost ~11,000 ticks against retail.  Larger images take longer on
 * retail (p90 of the next-LoadImage cases ~4,200); not modelled.
 * GPU_LOADIMAGE_CPU=0 is the negative control. */
#define LOADIMAGE_CPU_IMMEDIATE 989u
#define LOADIMAGE_CPU_QUEUED    367u
extern int gpu_queue_device_busy(void);
/* Weak: the GPU harnesses link gpu.c without the CPU model (cpu_cost.c). */
extern void hal_cpu_charge(unsigned ticks) __attribute__((weak));
static int loadimage_cpu_on(void) {
    static int on = -1;
    if (on < 0) {
        on = hal_model_on("GPU_LOADIMAGE_CPU");
    }
    return on;
}

void LoadImage(RECT *rect, u32 *data) {
    unsigned int width, height;
    size_t pixels;
    u32 ticket;
    if (hal_cpu_charge && loadimage_cpu_on())
        hal_cpu_charge(gpu_queue_device_busy() ? LOADIMAGE_CPU_QUEUED : LOADIMAGE_CPU_IMMEDIATE);
    /* load_image_rect passes the source as an s32 PS1 address: fold the
     * sign-extended value back into the mapped RAM window (hal_ps1_ptr). */
    data = (u32 *)hal_ps1_ptr(data);
    if (gp0_trace_enabled()) g_gp0_vram_load++;
    if (!rect || !data) {
        printf("[LoadImage] SKIP: rect=%p data=%p\n", (void*)rect, (void*)data);
        return;
    }
    if (!pc_ptr_is_mapped(rect, sizeof(RECT))) {
        printf("[LoadImage] SKIP: rect %p not mapped\n", (void*)rect);
        return;
    }
    /* SDK workers clamp dimensions and reject empty images before GP0.
     * A raw zero dimension means full extent, not an empty SDK request. */
    pixels = gpu_vram_image_pixels(rect->w, rect->h, &width, &height);
    if (!pixels) return;
    if (!pc_ptr_is_mapped(data, pixels * sizeof(u16))) {
        printf("[LoadImage] SKIP: source range %p not mapped\n", (void*)data);
        return;
    }
    ticket = gpu_queue_begin();
    /* Every upload (PC_DEBUG=gpu): the title re-uploads two sheets each
     * frame, which flooded the player's log (430 KB in a minute). */
    {   /* self-contained (the GPU test harnesses link gpu.c alone) */
        static int on = -1;
        if (on < 0) {
            const char *e = getenv("PC_DEBUG");
            on = e && (strstr(e, "gpu") || !strcmp(e, "1") || !strcmp(e, "all"));
        }
        if (on)
            printf("[LoadImage] rect=(%d,%d,%d,%d) data=%p\n",
                   rect->x, rect->y, rect->w, rect->h, (void*)data);
    }
    nbn_trace_load_image(data, (u32)(pixels * sizeof(u16)),
                         rect->x, rect->y, (s32)width, (s32)height);
    /* Debug: verify write */
    if (rect->x == 960 && rect->y == 128) {
        printf("[LoadImage] MAINROM pre-write: g_vram[128][960]=0x%04X\n", g_vram[128][960]);
    }
    /* _dws: GP0(01h) then GP0(A0h). */
    gpu_cache_clear();
    gpu_vram_transfer_started();
    gpu_vram_load(&g_vram[0][0], rect->x, rect->y,
                   (int)width, (int)height, (const u16 *)data,
                   g_mask_set_or != 0, g_mask_eval_and != 0);
    /* gpu_vram_load's wrapped rect, upscaled into the shadow */
    if (HIRES_ON())
        gpu_hires_mirror_rect(rect->x & 1023, rect->y & 511,
                              (int)((((unsigned)width - 1u) & 1023u) + 1u),
                              (int)((((unsigned)height - 1u) & 511u) + 1u));
    if (g_texrep_on)
        gpu_texrep_upload(rect->x & 1023, rect->y & 511,
                          (int)((((unsigned)width - 1u) & 1023u) + 1u),
                          (int)((((unsigned)height - 1u) & 511u) + 1u));
    if ((g_extras_gpu & 1) && extras_gallery_upload)
        extras_gallery_upload(rect->x & 1023, rect->y & 511,
                              (int)((((unsigned)width - 1u) & 1023u) + 1u),
                              (int)((((unsigned)height - 1u) & 511u) + 1u), data);
    /* Debug: verify write */
    if (rect->x == 960 && rect->y == 128) {
        printf("[LoadImage] MAINROM post-write: g_vram[128][960]=0x%04X g_vram[128][961]=0x%04X\n",
               g_vram[128][960], g_vram[128][961]);
        g_mainrom_loaded = 1;
    }
    gpu_queue_end(ticket);
}

/* Debug: check MAINROM integrity */
void debug_check_mainrom(const char *context) {
    if (g_mainrom_loaded) {
        u16 v = g_vram[128][960];
        if (v == 0) {
            printf("[GPU] MAINROM CORRUPTED at %s! g_vram[128][960]=0x%04X\n", context, v);
        }
    }
}

void StoreImage(RECT *rect, u32 *data) {
    unsigned int width, height;
    size_t pixels;
    u32 ticket;

    data = (u32 *)hal_ps1_ptr(data);
    if (gp0_trace_enabled()) g_gp0_vram_store++;
    if (!rect || !data || !pc_ptr_is_mapped(rect, sizeof(RECT))) return;
    pixels = gpu_vram_image_pixels(rect->w, rect->h, &width, &height);
    if (!pixels || !pc_ptr_is_mapped(data, pixels * sizeof(u16))) return;
    ticket = gpu_queue_begin();
    /* _drs: GP0(01h) then GP0(C0h). */
    gpu_cache_clear();
    gpu_vram_transfer_started();
    gpu_vram_store(&g_vram[0][0], rect->x, rect->y,
                    (int)width, (int)height, (u16 *)data);
    gpu_queue_end(ticket);
}

void MoveImage(RECT *src, s32 dx, s32 dy) {
    /* Copy VRAM rectangle to another location */
    s32 x, y;
    u32 ticket;
    if (gp0_trace_enabled()) g_gp0_vram_move++;
    if (!src) return;
    ticket = gpu_queue_begin();
    nbn_trace_move_image(src->x, src->y, src->w, src->h, dx, dy);
    /* MoveImage's packet is GP0(80h) alone: no 01h. */
    gpu_vram_transfer_started();
    /* TODO: measure hardware overlap ordering before changing this copy. */
    for (y = 0; y < src->h; y++) {
        for (x = 0; x < src->w; x++) {
            s32 sx = src->x + x, sy = src->y + y;
            s32 dxp = dx + x, dyp = dy + y;
            if (sx >= 0 && sx < VRAM_WIDTH && sy >= 0 && sy < VRAM_HEIGHT &&
                dxp >= 0 && dxp < VRAM_WIDTH && dyp >= 0 && dyp < VRAM_HEIGHT) {
                g_vram[dyp][dxp] = g_vram[sy][sx];
            }
        }
    }
    if (HIRES_ON()) gpu_hires_move(src->x, src->y, src->w, src->h, dx, dy);
    /* a moved sheet keys as the upload it came from (gpu_texrep.h) */
    if (g_texrep_on) gpu_texrep_move(src->x, src->y, src->w, src->h, dx, dy);
    gpu_queue_end(ticket);
}

void ClearOTag(u32 *ot_raw, s32 n) {
    /* Initialize OT as forward linked list using the side table.
     * Also reset the primitive pool cursor so allocations start fresh. */
    s32 i;
    extern void func_80049370_reset(s32 buf_idx);
    extern s32 activeBuff;
    if (!ot_raw || n <= 0) return;
    func_80049370_reset(activeBuff);
    for (i = 0; i < n - 1; i++) {
        pc_ot_setaddr(&ot_raw[i], &ot_raw[i + 1]);
    }
    pc_ot_setaddr(&ot_raw[n - 1], (void *)0xFFFFFF);
}

/* libgpu ClearOTagR (with its _otc, set_alarm and VSync(-1)) is ~640
 * ticks of CPU on retail: charged when game code calls it
 * (cpu_cost.h HAL_SDK_COST, measure_cpu_costs.py analyze-hal).  The
 * port's own extra clear in pc/src/main.c calls gpu_clear_otag_r. */
void gpu_clear_otag_r(u32 *ot_raw, s32 n);
void ClearOTagR(u32 *ot_raw, s32 n) {
    HAL_SDK_COST("ClearOTagR");
    gpu_clear_otag_r(ot_raw, n);
}

void gpu_clear_otag_r(u32 *ot_raw, s32 n) {
    /* Initialize OT as reverse linked list (depth-sorted).
     * Clear the pointer side table first — old frame's entries are stale.
     * Also reset the primitive pool cursor so allocations start fresh. */
    s32 i;
    extern void func_80049370_reset(s32 buf_idx);
    extern s32 activeBuff;
    if (!ot_raw || n <= 0) return;
    memset(g_ot_ptrs, 0, sizeof(g_ot_ptrs));
    func_80049370_reset(activeBuff);
    for (i = n - 1; i > 0; i--) {
        pc_ot_setaddr(&ot_raw[i], &ot_raw[i - 1]);
    }
    pc_ot_setaddr(&ot_raw[0], (void *)0xFFFFFF);
}

/* GPU primitive code bytes (bits 5-7 of code field) */
#define GPU_CODE_POLY_F3   0x20
#define GPU_CODE_POLY_FT3  0x24
#define GPU_CODE_POLY_F4   0x28
#define GPU_CODE_POLY_FT4  0x2C
#define GPU_CODE_POLY_G3   0x30
#define GPU_CODE_POLY_GT3  0x34
#define GPU_CODE_POLY_G4   0x38
#define GPU_CODE_POLY_GT4  0x3C
#define GPU_CODE_LINE_F2   0x40
#define GPU_CODE_LINE_G2   0x50
#define GPU_CODE_TILE      0x60
#define GPU_CODE_SPRT      0x64
#define GPU_CODE_TILE_1    0x68
#define GPU_CODE_TILE_8    0x70
#define GPU_CODE_SPRT_8    0x74
#define GPU_CODE_TILE_16   0x78
#define GPU_CODE_SPRT_16   0x7C
#define GPU_CODE_DR_TPAGE  0xE1
#define GPU_CODE_DR_MODE   0xE1

/* Current texture page state — declared above near g_draw_x */

/* Producer-side format bookkeeping, NOT GPU sampling state.
 * GsSortSprite still consults this registry; that upstream adapter needs a
 * separate retail-contract audit. The sampler only accepts commanded depth.
 *
 * Layout: g_vram_actual_bpp[tpage_index] where tpage_index = X/64
 * Value: 0=4bpp, 1=8bpp, 2=15bpp, -1=unset (use requested)
 */
static s8 g_vram_actual_bpp[16] = {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};

void gpu_set_region_bpp(s32 tpage_x, s32 bpp) {
    if (tpage_x >= 0 && tpage_x < 16) {
        g_vram_actual_bpp[tpage_x] = (s8)bpp;
        printf("[GPU] Set tpage %d actual bpp=%d\n", tpage_x, bpp);
    }
}

/* Get actual bpp for a tpage region.
 * Returns: 0=4bpp, 1=8bpp, 2=15bpp, or -1 if unset (use default).
 * Called by the existing GsSortSprite producer adapter. */
s32 gpu_get_region_bpp(s32 tpage_x) {
    if (tpage_x >= 0 && tpage_x < 16) {
        return (s32)g_vram_actual_bpp[tpage_x];
    }
    return -1;
}

/* Sampling obeys the command, never an asset registry or scene mirror. */
/* ---- Texture and CLUT caches ----
 * The GPU samples through a 2 KiB texture cache and a CLUT cache that is
 * "preserved between GPU drawing commands" and "invalidated when different
 * CLUT index values are used or when GP0(01h) is issued"; GP0(01h) is to
 * be issued after VRAM is modified (PSX-SPX, GPU Texture Caching / Clear
 * Cache).  Retail libgpu does exactly that: _dws (LoadImage) sends 01h
 * then A0h and _drs (StoreImage) 01h then C0h, while MoveImage's packet is
 * only 80h and ClearImage's is E3..E6 + 60h/02h (src/main/matched/_dws.c,
 * _drs.c, MoveImage.c data at 0x800E3B74, _clr.c).
 *
 * The cache organisation, the CLUT cache key ((clut & 7FFFh) | depth) and
 * the remaining invalidation events are those of Mednafen's PS_GPU, the
 * MDFN_GPU=1 rasterizer: the texture cache is cleared by 01h, by the start
 * of A0h/80h/C0h, and by a texture page change in X, Y or between 4-bit and
 * the other depths; NOT by fills, drawing, CLUT or texture-window changes.
 * The CLUT cache is cleared only by 01h and reloaded when its key differs
 * at the start of a textured primitive.  The same policy drives both
 * backends (see mdfn_gpu_shim.cpp).  Not established on hardware here:
 * whether a page change flushes the texture cache, and the reference emulator (the
 * AV reference) has no texture cache and does not reload a CLUT cached as
 * 8-bit for a following 4-bit use of the same CLUT.
 * VRAM addressing wraps at the right edge: a texture word or CLUT entry
 * past X=1023 is read from X & 1023 of the same row (no carry into Y),
 * the VRAM edge rule PSX-SPX gives for copies and fills and the one both
 * reference rasterizers apply to texel and CLUT fetches (Mednafen
 * GetTexel/Update_CLUT_Cache `& 1023`, DuckStation `% VRAM_WIDTH`).
 * Page 15 (X=960) in 8/15-bit mode and a CLUT whose 256 entries start
 * past X=768 reach it.  Tags hold (address & ~3) | 1, so zeroed storage
 * is the invalid state. */
static struct { u16 data[4]; u32 tag; } g_tex_cache[256];
static u16 g_clut_cache[256];
static u32 g_clut_cache_key = ~0u;

static void gpu_tex_cache_invalidate(void) {
    memset(g_tex_cache, 0, sizeof(g_tex_cache));
}

/* GP0(01h): both caches. */
static void gpu_cache_clear(void) {
    gpu_tex_cache_invalidate();
    g_clut_cache_key = ~0u;
    if (mdfn_gpu_enabled()) mdfn_clear_cache();
}

/* Start of GP0(80h/A0h/C0h): the texture cache only. */
static void gpu_vram_transfer_started(void) {
    gpu_tex_cache_invalidate();
    if (mdfn_gpu_enabled()) mdfn_invalidate_tex_cache();
}

/* Every texture page state change goes through here (E1, polygon page,
 * DRAWENV, reset).  The Mednafen backend applies the same rule itself in
 * mdfn_set_texpage. */
static void gpu_set_texpage_state(s32 x, s32 y, s32 bpp, s32 abr) {
    if (x != g_tpage_x || y != g_tpage_y || (bpp == 0) != (g_tpage_bpp == 0))
        gpu_tex_cache_invalidate();
    g_tpage_x = x;
    g_tpage_y = y;
    g_tpage_bpp = bpp;
    g_tpage_abr = abr;
}

/* Called once at the start of each textured primitive, as the GPU does. */
static void gpu_clut_cache_update(u16 clut, u16 tpage) {
    u32 depth = (tpage >> 7) & 3u, key, count, i, x, y;
    if (depth >= 2) return;
    key = (clut & 0x7FFFu) | (depth << 16);
    if (key == g_clut_cache_key) return;
    count = depth ? 256u : 16u;
    x = (clut & 63u) * 16u;
    y = (clut >> 6) & 511u;
    for (i = 0; i < count; i++)
        g_clut_cache[i] = g_vram[y][(x + i) & 1023u];
    g_clut_cache_key = key;
}

static u16 texel_fetch(u8 u, u8 v, u16 clut, u16 tpage) {
    u32 depth = (tpage >> 7) & 3u, x, y, address, line, word;
    int slot;
    (void)clut;
    if (depth == 3u) return 0;
    x = ((tpage & 15u) * 64u + ((u32)u >> (2u - depth))) & 1023u;
    y = ((tpage >> 4) & 1u) * 256u + v;
    address = y * 1024u + x;
    if (depth == 0u)
        slot = (int)(((address >> 2) & 3u) | ((address >> 8) & 0xFCu));
    else
        slot = (int)(((address >> 2) & 7u) | ((address >> 7) & 0xF8u));
    line = (address & ~3u) | 1u;
    if (g_tex_cache[slot].tag != line) {
        for (word = 0; word < 4u; word++)
            g_tex_cache[slot].data[word] = g_vram[y][(x & ~3u) + word];
        g_tex_cache[slot].tag = line;
    }
    word = g_tex_cache[slot].data[address & 3u];
    if (depth == 2u) return (u16)word;
    if (depth == 0u) return g_clut_cache[(word >> ((u & 3u) * 4u)) & 15u];
    return g_clut_cache[(word >> ((u & 1u) * 8u)) & 255u];
}

/* Debug version of texel_fetch for diagnostics: reads VRAM directly and
 * leaves the caches untouched. */
u16 texel_fetch_debug(u8 u, u8 v, u16 clut, u16 tpage) {
    return gpu_texture_sample(&g_vram[0][0], u, v, clut, tpage);
}

/* ---- Semi-Transparency Blending ----
 * PS1 has 4 blend modes controlled by ABR bits in tpage:
 *   0: 0.5×Back + 0.5×Front (50% blend)
 *   1: 1.0×Back + 1.0×Front (additive)
 *   2: 1.0×Back - 1.0×Front (subtractive)
 *   3: 1.0×Back + 0.25×Front (25% additive)
 * Semi-transparency is enabled per-primitive via bit 1 of the code byte.
 */
/* Map a PS1 primitive's semi-transparency to a Mednafen blend mode.
 *
 * `semi` is bit 1 of the primitive's code byte (semi-transparency on/off);
 * the MODE comes from the tpage ABR field, which is why passing a single
 * "is blended" bit is not enough.  Until 2026-08-22 the Mednafen path did
 * exactly that and collapsed every semi-transparent primitive to 50/50,
 * so additive and subtractive effects rendered at the wrong intensity --
 * the in-house rasterizer beside it had always honoured all four modes.
 */
static int mdfn_blend_for(int semi, s32 abr) {
    if (!semi) return MDFN_BLEND_OFF;
    g_blend_abr_hist[abr & 3]++;
    switch (abr & 3) {
    case 0:  return MDFN_BLEND_HALF;     /* 0.5 x B + 0.5 x F */
    case 1:  return MDFN_BLEND_ADD;      /* 1.0 x B + 1.0 x F */
    case 2:  return MDFN_BLEND_SUB;      /* 1.0 x B - 1.0 x F */
    default: return MDFN_BLEND_ADD_QTR;  /* 1.0 x B + 0.25 x F */
    }
}

static u16 blend_pixel(u16 back, u16 front, s32 abr) {
    s32 br = back & 0x1F;
    s32 bg = (back >> 5) & 0x1F;
    s32 bb = (back >> 10) & 0x1F;
    s32 fr = front & 0x1F;
    s32 fg = (front >> 5) & 0x1F;
    s32 fb = (front >> 10) & 0x1F;
    s32 r, g, b;

    switch (abr) {
    case 0: /* 50% + 50% */
        r = (br + fr) >> 1;
        g = (bg + fg) >> 1;
        b = (bb + fb) >> 1;
        break;
    case 1: /* Additive */
        r = br + fr;
        g = bg + fg;
        b = bb + fb;
        break;
    case 2: /* Subtractive */
        r = br - fr;
        g = bg - fg;
        b = bb - fb;
        break;
    case 3: /* 100% + 25% */
        r = br + (fr >> 2);
        g = bg + (fg >> 2);
        b = bb + (fb >> 2);
        break;
    default:
        r = fr; g = fg; b = fb;
        break;
    }

    /* Clamp to 5-bit range */
    if (r < 0) r = 0; else if (r > 31) r = 31;
    if (g < 0) g = 0; else if (g > 31) g = 31;
    if (b < 0) b = 0; else if (b > 31) b = 31;

    return (u16)(r | (g << 5) | (b << 10));
}

/* ---- PS1 Dithering ----
 * The PS1 uses a 4x4 ordered dither matrix to reduce banding in gouraud shading.
 * Values are added to RGB before quantizing to 5-bit.
 */
static const s8 g_dither_matrix[4][4] = {
    { -4,  +0,  -3,  +1 },
    { +2,  -2,  +3,  -1 },
    { -3,  +1,  -4,  +0 },
    { +3,  -1,  +2,  -2 }
};


static u16 dither_and_pack(s32 x, s32 y, s32 r, s32 g, s32 b) {
    if (g_dither_enabled) {
        s32 dither = g_dither_matrix[y & 3][x & 3];
        r += dither;
        g += dither;
        b += dither;
    }
    /* Clamp to 0-255 then quantize to 5-bit */
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return (u16)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
}

/* The shadow side of a native store (gpu_hires.h).  In the native pass of
 * a polygon the shadow pass has already drawn it: the block stays valid
 * for the new value only if that pass touched it.  Otherwise (sprites,
 * tiles, lines) the native pixel is mirrored: each of its N x N shadow
 * pixels takes the same front colour, blended with its own background. */
static void hires_native_stored(s32 x, s32 y, u16 old, u16 front, int blend, s32 abr)
{
    int s = g_hires.scale, i, j;
    unsigned int at = (unsigned int)y * 1024u + (unsigned int)x;
    u16 *row;
    if (g_hires.in_poly) {
        /* With sub-pixel vertices (gpu_pgxp.h) the shadow's edge is the
         * precise one, up to a native pixel inside the integer one, so a
         * native edge pixel can fall in a block the shadow polygon missed:
         * the block's high-resolution content (the background, valid for
         * the old native value) IS the answer there, as for an
         * alpha-tested texel; it becomes valid for the new value instead
         * of being refilled from it at 1x. */
        if (g_hires.stamp[at] == g_hires.prim ||
            (g_pgxp_tri && g_hires.ref[at] == old))
            g_hires.ref[at] = g_vram[y][x];
        return;
    }
    if (!blend) {
        gpu_hires_block_set(x, y, g_vram[y][x]);
        return;
    }
    if (g_hires.ref[at] != old) gpu_hires_block_set(x, y, old);
    row = g_hires.vram + (size_t)y * s * g_hires.stride + (size_t)x * s;
    for (j = 0; j < s; j++, row += g_hires.stride)
        for (i = 0; i < s; i++)
            row[i] = (blend_pixel(row[i], front, abr) | (front & 0x8000)) | g_mask_set_or;
    g_hires.ref[at] = g_vram[y][x];
}

/* Raster writes obey the drawing area and mask register for every family. */
static void gpu_raster_store(s32 x, s32 y, u16 pixel, int blend, s32 abr)
{
    s32 left = g_area_tl & 1023u;
    s32 top = (g_area_tl >> 10) & 1023u;
    u16 *target, old, front;
    if (x < left || y < top || x >= left + g_draw_w ||
        y >= top + g_draw_h || x < 0 || x >= VRAM_WIDTH ||
        y < 0 || y >= VRAM_HEIGHT) return;
    target = &g_vram[y][x];
    if (*target & g_mask_eval_and) return;
    old = *target;
    front = pixel;
    if (blend) pixel = blend_pixel(*target, pixel, abr) | (pixel & 0x8000);
    *target = pixel | g_mask_set_or;
    if (HIRES_ON()) hires_native_stored(x, y, old, front, blend, abr);
}

/* ---- Texture Window ----
 * Texture coordinates are masked/offset by the texture window settings.
 * Window is set via GP0(E2h) command stored in DR_MODE primitives.
 */
static u8 g_tex_window_mask_x = 0;   /* Mask: AND with ~(mask<<3) */
static u8 g_tex_window_mask_y = 0;
static u8 g_tex_window_off_x = 0;    /* Offset: OR with (off<<3) */
static u8 g_tex_window_off_y = 0;

void gpu_set_texture_window(u8 mask_x, u8 mask_y, u8 off_x, u8 off_y) {
    g_tex_window_mask_x = mask_x;
    g_tex_window_mask_y = mask_y;
    g_tex_window_off_x = off_x;
    g_tex_window_off_y = off_y;
}

/* GP1(00h) resets device registers and command state, preserving VRAM
 * and the caller-owned ordering-table links. GP1(01h) soft resets are
 * synchronous in this HAL and are handled by gpu_reset_graph. */
void gpu_reset_device(void) {
    g_disp_mask = 0;
    g_disp_x = g_disp_y = 0;
    g_disp_w = 256; g_disp_h = 240; g_disp_rgb24 = 0;
    g_draw_x = g_draw_y = 0;
    g_draw_w = g_draw_h = 1;
    g_area_tl = g_area_br = g_draw_offset = 0;
    gpu_set_texpage_state(0, 0, 0, 0);
    gpu_cache_clear();
    g_mask_set_or = g_mask_eval_and = 0;
    g_dither_enabled = 0;
    gpu_set_texture_window(0, 0, 0, 0);
    if (mdfn_gpu_enabled()) {
        mdfn_set_drawing_area(0, 0, 0, 0);
        mdfn_set_draw_offset(0, 0);
        mdfn_set_texpage(0);
        mdfn_set_texwindow(0, 0, 0, 0);
        mdfn_set_mask(0, 0);
    }
}

static void apply_texture_window(u8 *u, u8 *v) {
    /* Apply texture window masking and offset */
    if (g_tex_window_mask_x || g_tex_window_mask_y) {
        u8 orig_u = *u, orig_v = *v;
        *u = (*u & ~(g_tex_window_mask_x << 3)) | ((g_tex_window_off_x & g_tex_window_mask_x) << 3);
        *v = (*v & ~(g_tex_window_mask_y << 3)) | ((g_tex_window_off_y & g_tex_window_mask_y) << 3);
        /* Trace texture window effects on MAINROM-range UVs */
        static int tw_trace = 0;
        if (tw_trace < 10 && orig_u >= 128) {
            printf("[TEXWIN] mask=(%d,%d) off=(%d,%d) UV(%d,%d)->(%d,%d)\n",
                   g_tex_window_mask_x, g_tex_window_mask_y,
                   g_tex_window_off_x, g_tex_window_off_y,
                   orig_u, orig_v, *u, *v);
            tw_trace++;
        }
    }
}

/* Wrapper for texel fetch with texture window applied */
static u16 texel_fetch_windowed(u8 u, u8 v, u16 clut, u16 tpage) {
    apply_texture_window(&u, &v);
    return texel_fetch(u, v, clut, tpage);
}

/* The shadow pass's texel fetch (gpu_hires.h): the same texel the native
 * pass would read -- through a texture-cache line it holds, else from
 * VRAM, and through the CLUT cache the primitive has already loaded -- but
 * it never fills or evicts a line, so the native pass that follows sees the
 * caches exactly as it would at 1x. */
static u16 texel_peek_windowed(u8 u, u8 v, u16 clut, u16 tpage) {
    u32 depth = (tpage >> 7) & 3u, x, y, address, word;
    int slot;
    (void)clut;
    apply_texture_window(&u, &v);
    if (depth == 3u) return 0;
    x = ((tpage & 15u) * 64u + ((u32)u >> (2u - depth))) & 1023u;
    y = ((tpage >> 4) & 1u) * 256u + v;
    address = y * 1024u + x;
    if (depth == 0u)
        slot = (int)(((address >> 2) & 3u) | ((address >> 8) & 0xFCu));
    else
        slot = (int)(((address >> 2) & 7u) | ((address >> 7) & 0xF8u));
    if (g_tex_cache[slot].tag == ((address & ~3u) | 1u))
        word = g_tex_cache[slot].data[address & 3u];
    else
        word = g_vram[y][x];
    if (depth == 2u) return (u16)word;
    if (depth == 0u) return g_clut_cache[(word >> ((u & 3u) * 4u)) & 15u];
    return g_clut_cache[(word >> ((u & 1u) * 8u)) & 255u];
}

/* GPU size limits (PSX-SPX "Render Polygon Commands"; Mednafen's
 * Command_DrawPolygon/Command_DrawLine): a triangle whose vertices are
 * 1024+ apart in X or 512+ apart in Y, or a line spanning that much, is
 * not drawn.  Differences do not depend on the drawing offset. */
static int gpu_span_fits(s32 a, s32 b, s32 limit) {
    return (a > b ? a - b : b - a) < limit;
}

static int gpu_triangle_fits(s32 x0, s32 y0, s32 x1, s32 y1,
                             s32 x2, s32 y2) {
    return gpu_span_fits(x0, x1, 1024) && gpu_span_fits(x1, x2, 1024) &&
           gpu_span_fits(x0, x2, 1024) && gpu_span_fits(y0, y1, 512) &&
           gpu_span_fits(y1, y2, 512) && gpu_span_fits(y0, y2, 512);
}

/* Draw a flat-colored rectangle (TILE primitive) with optional blending */
void draw_tile_blend(s32 x, s32 y, s32 w, s32 h, u8 r, u8 g, u8 b, int blend, s32 abr) {
    s32 ix, iy;
    u16 pixel = ((r >> 3) & 0x1F) | (((g >> 3) & 0x1F) << 5) | (((b >> 3) & 0x1F) << 10);
    x = gpu_signed11((u32)(x + g_draw_x));
    y = gpu_signed11((u32)(y + g_draw_y));
    for (iy = y; iy < y + h && iy < VRAM_HEIGHT; iy++) {
        if (iy < 0) continue;
        for (ix = x; ix < x + w && ix < VRAM_WIDTH; ix++) {
            if (ix < 0) continue;
            gpu_raster_store(ix, iy, pixel, blend, abr);
                }
    }
}

/* Legacy wrapper for compatibility */
void draw_tile(s32 x, s32 y, s32 w, s32 h, u8 r, u8 g, u8 b) {
    draw_tile_blend(x, y, w, h, r, g, b, 0, 0);
}

/* A replacement texel (gpu_texrep_sample's 0xRRGGBBAA) as a 15-bit texel
 * before colour modulation; the semi-transparency bit is the native
 * texel's (`stp`): alpha in a pack is coverage only (gpu_texrep.h). */
static u16 texrep_texel(unsigned int c, u16 stp)
{
    return (u16)(((c >> 27) & 31u) | (((c >> 19) & 31u) << 5) | (((c >> 11) & 31u) << 10) |
                 (stp & 0x8000u));
}

/* The shadow pass of a sprite while a texture pack is loaded
 * (pc/hal/gpu_texrep.h): like a polygon's, it runs before the native pass
 * and claims (stamps) every native pixel it covers.  Each sub-pixel samples
 * the replacement at its own position inside the texel; a texel without a
 * replacement fills its block exactly as the 1x mirror would (the native
 * front colour, blended per sub-pixel). */
static void hires_sprite_shadow(s32 x, s32 y, s32 w, s32 h, u8 u0, u8 v0,
                                u16 clut, u16 tpage, u8 r, u8 g, u8 b, int blend, s32 abr)
{
    s32 left = g_area_tl & 1023u, top = (g_area_tl >> 10) & 1023u;
    s32 sc = g_hires.scale, ix, iy, i, j;
    u32 prim = ++g_hires.prim;
    u16 mask_eval = (u16)g_mask_eval_and, mask_set = (u16)g_mask_set_or;
    for (iy = 0; iy < h; iy++) {
        s32 dy = y + iy;
        if (dy < 0 || dy >= VRAM_HEIGHT || dy < top || dy >= top + g_draw_h) continue;
        for (ix = 0; ix < w; ix++) {
            s32 dx = x + ix;
            unsigned at;
            u8 tu = (u8)(u0 + ix), tv = (u8)(v0 + iy);
            unsigned int c;
            GpuTexrepTexel tex;
            u16 *row, ntex;
            if (dx < 0 || dx >= VRAM_WIDTH || dx < left || dx >= left + g_draw_w) continue;
            at = (unsigned)dy * VRAM_WIDTH + (unsigned)dx;
            if (g_hires.stamp[at] != prim) {
                if (g_hires.ref[at] != g_vram[dy][dx]) {
                    gpu_hires_block_set(dx, dy, g_vram[dy][dx]);
                    g_hires.resyncs++;
                }
                g_hires.stamp[at] = prim;
            }
            row = g_hires.vram + (size_t)dy * sc * g_hires.stride + (size_t)dx * sc;
            ntex = texel_peek_windowed((u8)(u0 + ix), (u8)(v0 + iy), clut, tpage);
            apply_texture_window(&tu, &tv);
            if (!gpu_texrep_texel(tu, tv, &tex)) {
                /* no replacement for this texel: the 1x mirror's colour */
                u16 texel = ntex;
                s32 red, green, blue;
                if (texel == 0) continue;
                red = ((texel & 31) * r) >> 7;
                green = (((texel >> 5) & 31) * g) >> 7;
                blue = (((texel >> 10) & 31) * b) >> 7;
                if (red > 31) red = 31;
                if (green > 31) green = 31;
                if (blue > 31) blue = 31;
                texel = (texel & 0x8000) | red | (green << 5) | (blue << 10);
                for (j = 0; j < sc; j++, row += g_hires.stride)
                    for (i = 0; i < sc; i++) {
                        u16 px = texel;
                        if (row[i] & mask_eval) continue;
                        if (blend && (texel & 0x8000))
                            px = blend_pixel(row[i], texel, abr) | (texel & 0x8000);
                        row[i] = px | mask_set;
                    }
                continue;
            }
            if (tex.kx == (unsigned)sc << 16 && tex.ky == (unsigned)sc << 16 &&
                r == 128 && g == 128 && b == 128) {
                /* the pack is at the shadow's scale and the sprite is not
                 * tinted: block (i, j) is replacement pixel (tx*N+i, ty*N+j) */
                const u16 *src = tex.px16 + (size_t)tex.ty * sc * tex.w + (size_t)tex.tx * sc;
                for (j = 0; j < sc; j++, row += g_hires.stride, src += tex.w) {
                    for (i = 0; i < sc; i++) {
                        u16 px = src[i];
                        if (px == GPU_TEXREP_PX16_CLEAR || (row[i] & mask_eval)) continue;
                        if (px == GPU_TEXREP_PX16_MIX) {
                            c = tex.px[(size_t)(tex.ty * sc + j) * tex.w + tex.tx * sc + i];
                            px = texrep_texel(c, ntex);
                        } else {
                            px |= ntex & 0x8000;
                        }
                        if (blend && (px & 0x8000))
                            px = blend_pixel(row[i], px, abr) | (px & 0x8000);
                        if (src[i] == GPU_TEXREP_PX16_MIX)
                            px = gpu_texrep_alpha_mix(row[i], px, c & 255u);
                        row[i] = px | mask_set;
                    }
                }
                continue;
            }
            for (j = 0; j < sc; j++, row += g_hires.stride) {
                for (i = 0; i < sc; i++) {
                    unsigned fu = (unsigned)((i * 256 + 128) / sc);
                    unsigned fv = (unsigned)((j * 256 + 128) / sc);
                    s32 red, green, blue;
                    u16 px;
                    c = gpu_texrep_texel_at(&tex, fu, fv);
                    if ((c & 255u) < GPU_TEXREP_ALPHA_CLEAR) continue;
                    if (row[i] & mask_eval) continue;
                    red = (s32)(((c >> 24) * r) >> 7);
                    green = (s32)((((c >> 16) & 255u) * g) >> 7);
                    blue = (s32)((((c >> 8) & 255u) * b) >> 7);
                    if (red > 255) red = 255;
                    if (green > 255) green = 255;
                    if (blue > 255) blue = 255;
                    px = (u16)((red >> 3) | ((green >> 3) << 5) | ((blue >> 3) << 10) |
                               (ntex & 0x8000));
                    if (blend && (px & 0x8000))
                        px = blend_pixel(row[i], px, abr) | (px & 0x8000);
                    if ((c & 255u) < GPU_TEXREP_ALPHA_SOLID)
                        px = gpu_texrep_alpha_mix(row[i], px, c & 255u);
                    row[i] = px | mask_set;
                }
            }
        }
    }
}

/* Draw a textured sprite (SPRT primitive) with optional blending — non-static for test rendering */
void draw_sprt_blend(s32 x, s32 y, s32 w, s32 h, u8 u0, u8 v0,
                     u16 clut, u16 tpage, u8 r, u8 g, u8 b, int blend, s32 abr) {
    s32 ix, iy, red, green, blue;
    int shadow = 0;
    gpu_clut_cache_update(clut, tpage);
    if (g_texrep_on)
        gpu_texrep_note_draw(clut, tpage, u0, v0, u0 + w - 1, v0 + h - 1);
    if ((g_extras_gpu & 2) && extras_gallery_draw)
        extras_gallery_draw(tpage, u0, v0, u0 + w - 1, v0 + h - 1);
    x = gpu_signed11((u32)(x + g_draw_x));
    y = gpu_signed11((u32)(y + g_draw_y));
    if (HIRES_ON() && g_texrep_replace && gpu_texrep_prim_begin(clut, tpage)) {
        /* a pack: the shadow pass samples it; the native pass below keeps
         * the blocks it claimed valid (as a polygon's does) */
        hires_sprite_shadow(x, y, w, h, u0, v0, clut, tpage, r, g, b, blend, abr);
        g_hires.in_poly = 1;
        shadow = 1;
    }
    for (iy = 0; iy < h; iy++) {
        s32 dy = y + iy;
        if (dy < 0 || dy >= VRAM_HEIGHT) continue;
        for (ix = 0; ix < w; ix++) {
            s32 dx = x + ix;
            u16 texel;
            if (dx < 0 || dx >= VRAM_WIDTH) continue;
            texel = texel_fetch_windowed(u0 + ix, v0 + iy, clut, tpage);
            if (texel == 0) continue;
            /* 0x80 is unity. Test transparency before multiplication:
             * an opaque texel tinted black still writes a pixel. Sprites
             * do not dither. Keep the source STP bit for blending/masks. */
            red = ((texel & 31) * r) >> 7;
            green = (((texel >> 5) & 31) * g) >> 7;
            blue = (((texel >> 10) & 31) * b) >> 7;
            if (red > 31) red = 31;
            if (green > 31) green = 31;
            if (blue > 31) blue = 31;
            texel = (texel & 0x8000) | red | (green << 5) | (blue << 10);
            gpu_raster_store(dx, dy, texel, blend && (texel & 0x8000), abr);
        }
    }
    if (shadow) g_hires.in_poly = 0;
}

/* Legacy wrapper for compatibility */
void draw_sprt(s32 x, s32 y, s32 w, s32 h, u8 u0, u8 v0,
               u16 clut, u16 tpage, u8 r, u8 g, u8 b) {
    draw_sprt_blend(x, y, w, h, u0, v0, clut, tpage, r, g, b, 0, 0);
}

/* ---- Triangle rasterizer ----
 * One triangle model for every polygon family, the GPU's as Mednafen's
 * software renderer (DrawTriangle/DrawSpan/CalcIDeltas, which agrees with
 * the retail reference) and PSX-SPX ("Render Polygon Commands") describe it:
 *
 * - Coverage: vertices sorted by Y; each scanline from the top vertex's row
 *   down to, not including, the bottom vertex's row is filled from its left
 *   edge up to, not including, its right edge.  Edges step in 32.32 fixed
 *   point from x + 1 - 2^-21 (so a left edge exactly on a pixel centre
 *   includes it), and MakePolyXFPStep rounds slopes away from zero.  So the
 *   bottom row and right column of a polygon are not drawn (top-left rule),
 *   and the two halves of a quad, split by the GPU into (v0,v1,v2) and
 *   (v1,v2,v3), never both draw a pixel on their shared edge -- which
 *   matters under semi-transparency.
 * - Colour and texture coordinates: per-triangle plane gradients d/dx and
 *   d/dy, 12.12 fixed point truncated once, stepped from the "core" vertex
 *   (the leftmost one by the GPU's unsorted-input rule) with a half-unit
 *   bias; each value is the top 8 bits of a 32-bit sum, which wraps rather
 *   than clamps.  Flat polygons use the core vertex's (= vertex 0's)
 *   colour; UVs always interpolate.
 * - Textured pixels: texel 0000h is transparent; raw mode writes the texel;
 *   otherwise each 5-bit channel is multiplied by the 8-bit colour,
 *   (texel * colour) >> 4 with 80h as unity, then dithered when E1 enables
 *   it (flat or gouraud) and saturated to 5 bits.  Bit 15 is kept and
 *   selects semi-transparency.  Untextured gouraud pixels dither; flat
 *   untextured ones do not.
 * - Spans start at the scanline's left edge or the drawing area's left
 *   side, whichever is greater; rows outside the drawing area are skipped
 *   (rows above it when the half is walked upwards end it).
 *
 * The rasterizer is selected by RasterMode; RasterVertex coordinates
 * already include the drawing offset. */
typedef struct {
    s32 x, y;
    s32 r, g, b;
    s32 u, v;
} RasterVertex;

typedef struct {
    int gouraud;            /* colour gradients (otherwise vertex 0's) */
    int textured;           /* sample clut/tpage at the interpolated UV */
    int raw;                /* textured: no colour modulation */
    int blend;              /* semi-transparency requested */
    s32 abr;
    u16 clut, tpage;
} RasterMode;

typedef struct {
    u32 r, g, b, u, v;
} RasterValues;

typedef struct {
    RasterValues dx, dy;
} RasterDeltas;

/* 32.32 fixed-point X with the GPU's left-edge bias. */
static u64 raster_xfp(s32 x)
{
    return ((u64)(u32)x << 32) + ((1ull << 32) - (1u << 11));
}

/* 32.32 step dx/dy (dy > 0), rounded away from zero. */
static s64 raster_xfp_step(s32 dx, s32 dy)
{
    s64 dx_ex = (s64)dx * 4294967296ll;
    if (dx_ex < 0) dx_ex -= dy - 1;
    if (dx_ex > 0) dx_ex += dy - 1;
    return dx_ex / dy;
}

static s32 raster_xfp_int(u64 xfp)
{
    return (s32)(u32)(xfp >> 32);
}

/* d(value)/dx or /dy as 8.24, the GPU's CalcIDeltas: (n * 4096 / denom)
 * truncated toward zero, then shifted into the top bits. */
static u32 raster_delta(s64 n, s64 denom)
{
    return (u32)(s32)(n * 4096 / denom) << 12;
}

static void raster_span(s32 yi, s32 x_start, s32 x_bound,
                        const RasterValues *core, s32 cx, s32 cy,
                        const RasterDeltas *d, const RasterMode *mode)
{
    s32 left = g_area_tl & 1023u;
    s32 right = (g_area_br & 1023u);
    s32 x_ig = x_start, w = x_bound - x_start, x = gpu_signed11((u32)x_start);
    s32 y = gpu_signed11((u32)yi);
    u32 ox, oy;
    RasterValues v;

    if (x < left) {
        x_ig += left - x;
        w -= left - x;
        x = left;
    }
    if (x + w > right + 1) w = right + 1 - x;
    if (w <= 0) return;
    ox = (u32)(x_ig - cx);
    oy = (u32)(yi - cy);
    v.r = core->r + d->dx.r * ox + d->dy.r * oy;
    v.g = core->g + d->dx.g * ox + d->dy.g * oy;
    v.b = core->b + d->dx.b * ox + d->dy.b * oy;
    v.u = core->u + d->dx.u * ox + d->dy.u * oy;
    v.v = core->v + d->dx.v * ox + d->dy.v * oy;
    do {
        s32 r = (s32)(v.r >> 24), g = (s32)(v.g >> 24), b = (s32)(v.b >> 24);
        u16 pixel;
        if (mode->textured) {
            u16 texel = texel_fetch_windowed((u8)(v.u >> 24), (u8)(v.v >> 24),
                                             mode->clut, mode->tpage);
            if (texel) {
                if (mode->raw)
                    pixel = texel;
                else
                    pixel = dither_and_pack(x, yi,
                                ((texel & 31) * r) >> 4,
                                (((texel >> 5) & 31) * g) >> 4,
                                (((texel >> 10) & 31) * b) >> 4) |
                            (texel & 0x8000);
                gpu_raster_store(x, y & 511, pixel,
                                 mode->blend && (texel & 0x8000), mode->abr);
            }
        } else {
            if (mode->gouraud)
                pixel = dither_and_pack(x, yi, r, g, b);
            else
                pixel = (u16)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
            gpu_raster_store(x, y & 511, pixel, mode->blend, mode->abr);
        }
        x++;
        v.r += d->dx.r;
        v.g += d->dx.g;
        v.b += d->dx.b;
        v.u += d->dx.u;
        v.v += d->dx.v;
    } while (--w > 0);
}

/* raster_span in the shadow: coordinates are N x the native ones and are
 * clipped to the drawing area N x (no 11-bit wrap); colour, texture
 * coordinates and the dither pattern are those of the native pixel the
 * shadow pixel lies in (dithering at 1x scale, as the reference emulator's default).
 *
 * The first shadow pixel this triangle COVERS in a native pixel's block
 * "claims" the block (gpu_hires.h), whether or not it is written (a
 * transparent texel, a masked pixel): a stale block is refilled from the
 * native pixel first, so the triangle never draws over content the native
 * VRAM has replaced, and the stamp lets the native pass keep the block
 * valid.  Claiming on coverage, not on writes, matters at the edges of
 * alpha-tested textures: where the native pixel samples an opaque texel and
 * all N x N sub-samples transparent ones, the shadow's answer (background)
 * is the high-resolution result, not a stale block to drop to 1x. */
#define HIRES_SPAN_CLAIM()                                                  \
    do {                                                                    \
        if (px != claimed) {                                                \
            if (srow[px] != prim) {                                         \
                if (rrow[px] != nrow[px]) {                                 \
                    gpu_hires_block_set(px, py, nrow[px]);                  \
                    g_hires.resyncs++;                                      \
                }                                                           \
                srow[px] = prim;                                            \
            }                                                               \
            claimed = px;                                                   \
        }                                                                   \
    } while (0)
#define HIRES_SPAN_STORE(pixel_expr, blend_expr)                            \
    do {                                                                    \
        u16 pixel_ = (pixel_expr), *target_ = row + x;                      \
        if (!(*target_ & mask_eval)) {                                      \
            if (blend_expr)                                                 \
                pixel_ = blend_pixel(*target_, pixel_, mode->abr) |         \
                         (pixel_ & 0x8000);                                 \
            *target_ = pixel_ | mask_set;                                   \
        }                                                                   \
    } while (0)

static void raster_span_hires(s32 yi, s32 x_start, s32 x_bound,
                              const RasterValues *core, s32 cx, s32 cy,
                              const RasterDeltas *d, const RasterMode *mode)
{
    s32 s = g_hires.scale;
    s32 left = (s32)(g_area_tl & 1023u) * s;
    s32 right = (s32)(g_area_br & 1023u) * s + s - 1;
    s32 x = x_start, w = x_bound - x_start, px, sub, py, claimed = -1;
    u32 ox, oy, prim = g_hires.prim;
    u16 mask_eval = (u16)g_mask_eval_and, mask_set = (u16)g_mask_set_or;
    u16 *row, *rrow;
    const u16 *nrow;
    unsigned int *srow;
    RasterValues v;

    if (right > VRAM_WIDTH * s - 1) right = VRAM_WIDTH * s - 1;
    if (x < left) {
        w -= left - x;
        x = left;
    }
    if (x + w > right + 1) w = right + 1 - x;
    if (w <= 0 || yi < 0 || yi >= VRAM_HEIGHT * s) return;
    ox = (u32)(x - cx);
    oy = (u32)(yi - cy);
    v.r = core->r + d->dx.r * ox + d->dy.r * oy;
    v.g = core->g + d->dx.g * ox + d->dy.g * oy;
    v.b = core->b + d->dx.b * ox + d->dy.b * oy;
    v.u = core->u + d->dx.u * ox + d->dy.u * oy;
    v.v = core->v + d->dx.v * ox + d->dy.v * oy;
    px = x / s;
    sub = x % s;
    py = yi / s;
    row = g_hires.vram + (size_t)yi * g_hires.stride;
    srow = g_hires.stamp + (size_t)py * VRAM_WIDTH;
    rrow = g_hires.ref + (size_t)py * VRAM_WIDTH;
    nrow = g_vram[py];
    if (mode->textured) {
        int rep_on = g_texrep_prim;
        do {
            u16 texel = texel_peek_windowed((u8)(v.u >> 24), (u8)(v.v >> 24),
                                            mode->clut, mode->tpage);
            int rep = 0;
            unsigned int rc = 0;
            u16 ntexel = texel;
            if (rep_on) {
                /* a texture pack (pc/hal/gpu_texrep.h): the replacement at
                 * this sub-pixel's position inside the texel */
                u8 tu = (u8)(v.u >> 24), tv = (u8)(v.v >> 24);
                apply_texture_window(&tu, &tv);
                rep = gpu_texrep_sample(tu, tv, (v.u >> 16) & 255u, (v.v >> 16) & 255u, &rc);
                if (rep) texel = 0;
            }
            HIRES_SPAN_CLAIM();
            if (texel) {
                if (mode->raw)
                    HIRES_SPAN_STORE(texel, mode->blend && (texel & 0x8000));
                else
                    HIRES_SPAN_STORE(dither_and_pack(px, py,
                            ((texel & 31) * (s32)(v.r >> 24)) >> 4,
                            (((texel >> 5) & 31) * (s32)(v.g >> 24)) >> 4,
                            (((texel >> 10) & 31) * (s32)(v.b >> 24)) >> 4) |
                        (texel & 0x8000), mode->blend && (texel & 0x8000));
            }
            if (rep == 2) {
                /* the replacement, as HIRES_SPAN_STORE stores a texel, then
                 * mixed by its alpha with what is behind */
                u16 rt = texrep_texel(rc, ntexel), *target = row + x;
                if (!(*target & mask_eval)) {
                    u16 pixel = mode->raw ? rt :
                        (u16)(dither_and_pack(px, py,
                                  (s32)(((rc >> 24) * (v.r >> 24)) >> 7),
                                  (s32)((((rc >> 16) & 255u) * (v.g >> 24)) >> 7),
                                  (s32)((((rc >> 8) & 255u) * (v.b >> 24)) >> 7)) |
                              (rt & 0x8000));
                    if (mode->blend && (rt & 0x8000))
                        pixel = blend_pixel(*target, pixel, mode->abr) | (pixel & 0x8000);
                    if ((rc & 255u) < GPU_TEXREP_ALPHA_SOLID)
                        pixel = gpu_texrep_alpha_mix(*target, pixel, rc & 255u);
                    *target = pixel | mask_set;
                }
            }
            x++;
            if (++sub == s) {
                sub = 0;
                px++;
            }
            v.r += d->dx.r;
            v.g += d->dx.g;
            v.b += d->dx.b;
            v.u += d->dx.u;
            v.v += d->dx.v;
        } while (--w > 0);
        return;
    }
    do {
        s32 r = (s32)(v.r >> 24), g = (s32)(v.g >> 24), b = (s32)(v.b >> 24);
        HIRES_SPAN_CLAIM();
        if (mode->gouraud)
            HIRES_SPAN_STORE(dither_and_pack(px, py, r, g, b), mode->blend);
        else
            HIRES_SPAN_STORE((u16)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)),
                             mode->blend);
        x++;
        if (++sub == s) {
            sub = 0;
            px++;
        }
        v.r += d->dx.r;
        v.g += d->dx.g;
        v.b += d->dx.b;
        v.u += d->dx.u;
        v.v += d->dx.v;
    } while (--w > 0);
}
#undef HIRES_SPAN_CLAIM
#undef HIRES_SPAN_STORE

static void raster_triangle(const RasterVertex *input, const RasterMode *mode,
                            int hires);
static int pgxp_shadow_vertex(const RasterVertex *in, s32 s, RasterVertex *out);

/* Every polygon family ends here.  With the internal resolution raised the
 * triangle is drawn twice: first into the shadow from its vertices times N
 * (reading, never changing, the texture caches), then natively exactly as
 * at 1x -- the native VRAM the game sees does not depend on the scale. */
static void gpu_raster_triangle(const RasterVertex *input,
                                const RasterMode *mode)
{
    if (g_texrep_on && mode->textured) {
        s32 umin = input[0].u, umax = input[0].u, vmin = input[0].v, vmax = input[0].v;
        int i;
        for (i = 1; i < 3; i++) {
            if (input[i].u < umin) umin = input[i].u;
            if (input[i].u > umax) umax = input[i].u;
            if (input[i].v < vmin) vmin = input[i].v;
            if (input[i].v > vmax) vmax = input[i].v;
        }
        gpu_texrep_note_draw(mode->clut, mode->tpage, umin, vmin, umax, vmax);
    }
    if ((g_extras_gpu & 2) && mode->textured && extras_gallery_draw) {
        s32 umin = input[0].u, umax = input[0].u, vmin = input[0].v, vmax = input[0].v;
        int i;
        for (i = 1; i < 3; i++) {
            if (input[i].u < umin) umin = input[i].u;
            if (input[i].u > umax) umax = input[i].u;
            if (input[i].v < vmin) vmin = input[i].v;
            if (input[i].v > vmax) vmax = input[i].v;
        }
        extras_gallery_draw(mode->tpage, umin, vmin, umax, vmax);
    }
    if (HIRES_ON()) {
        RasterVertex hv[3];
        int i;
        for (i = 0; i < 3; i++) {
            hv[i] = input[i];
            hv[i].x = input[i].x * g_hires.scale;
            hv[i].y = input[i].y * g_hires.scale;
        }
        if (g_pgxp_prim.n) {
            RasterVertex pv[3];
            for (i = 0; i < 3; i++) {
                pv[i] = input[i];
                if (!pgxp_shadow_vertex(&input[i], g_hires.scale, &pv[i])) break;
            }
            if (i == 3) {
                memcpy(hv, pv, sizeof hv);
                g_pgxp_tri = 1;
            }
        }
        g_hires.prim++;
        g_texrep_prim = g_texrep_replace && mode->textured &&
                        gpu_texrep_prim_begin(mode->clut, mode->tpage);
        raster_triangle(hv, mode, 1);
        g_texrep_prim = 0;
        g_hires.in_poly = 1;
        raster_triangle(input, mode, 0);
        g_hires.in_poly = 0;
        g_pgxp_tri = 0;
        return;
    }
    raster_triangle(input, mode, 0);
}

static void raster_triangle(const RasterVertex *input, const RasterMode *mode,
                            int hires)
{
    RasterVertex v[3], t;
    RasterValues core;
    RasterDeltas d;
    unsigned cvtemp, core_vertex, vo, vp, i;
    u32 bias;
    s64 denom, base_step, bound_us, bound_ls;
    u64 base_coord;
    int right_facing;
    s32 top = (g_area_tl >> 10) & 1023u;
    s32 bottom = (g_area_br >> 10) & 1023u;
    struct {
        u64 x_coord[2];
        s64 x_step[2];
        s32 y_coord, y_bound;
        int dec_mode;
    } part[2];

    v[0] = input[0]; v[1] = input[1]; v[2] = input[2];
    /* The core vertex by the unsorted rule, tracked through the Y sort. */
    if (v[1].x <= v[0].x)
        cvtemp = (v[2].x <= v[1].x) ? 4u : 2u;
    else if (v[2].x < v[0].x)
        cvtemp = 4u;
    else
        cvtemp = 1u;
    if (v[2].y < v[1].y) {
        t = v[2]; v[2] = v[1]; v[1] = t;
        cvtemp = ((cvtemp >> 1) & 2u) | ((cvtemp << 1) & 4u) | (cvtemp & 1u);
    }
    if (v[1].y < v[0].y) {
        t = v[1]; v[1] = v[0]; v[0] = t;
        cvtemp = ((cvtemp >> 1) & 1u) | ((cvtemp << 1) & 2u) | (cvtemp & 4u);
    }
    if (v[2].y < v[1].y) {
        t = v[2]; v[2] = v[1]; v[1] = t;
        cvtemp = ((cvtemp >> 1) & 2u) | ((cvtemp << 1) & 4u) | (cvtemp & 1u);
    }
    core_vertex = cvtemp >> 1;
    if (v[0].y == v[2].y) return;

#define RASTER_CALCIS(a, b) \
    ((s64)(v[1].a - v[0].a) * (v[2].b - v[1].b) - \
     (s64)(v[2].a - v[1].a) * (v[1].b - v[0].b))
    denom = RASTER_CALCIS(x, y);
    if (!denom) return;
    memset(&d, 0, sizeof(d));
    if (mode->gouraud) {
        d.dx.r = raster_delta(RASTER_CALCIS(r, y), denom);
        d.dy.r = raster_delta(RASTER_CALCIS(x, r), denom);
        d.dx.g = raster_delta(RASTER_CALCIS(g, y), denom);
        d.dy.g = raster_delta(RASTER_CALCIS(x, g), denom);
        d.dx.b = raster_delta(RASTER_CALCIS(b, y), denom);
        d.dy.b = raster_delta(RASTER_CALCIS(x, b), denom);
    }
    if (mode->textured) {
        d.dx.u = raster_delta(RASTER_CALCIS(u, y), denom);
        d.dy.u = raster_delta(RASTER_CALCIS(x, u), denom);
        d.dx.v = raster_delta(RASTER_CALCIS(v, y), denom);
        d.dy.v = raster_delta(RASTER_CALCIS(x, v), denom);
    }
#undef RASTER_CALCIS
    /* The texture coordinates' half-unit bias, in the shadow, is 1/N of
     * the native one: the N sub-pixels of a native pixel then take u/v at
     * k + 1/2 of N across it, so a polygon mapped 1:1 samples the same
     * texel in all of them -- exactly the 1x pixel -- instead of shifting
     * the texture by half a native pixel.  Colours keep the full bias (the
     * block's average stays the 1x value). */
    bias = hires ? (1u << 11) / (u32)g_hires.scale : (1u << 11);
    core.r = ((u32)v[core_vertex].r << 12 | (1u << 11)) << 12;
    core.g = ((u32)v[core_vertex].g << 12 | (1u << 11)) << 12;
    core.b = ((u32)v[core_vertex].b << 12 | (1u << 11)) << 12;
    core.u = ((u32)v[core_vertex].u << 12 | bias) << 12;
    core.v = ((u32)v[core_vertex].v << 12 | bias) << 12;

    base_coord = raster_xfp(v[0].x);
    base_step = raster_xfp_step(v[2].x - v[0].x, v[2].y - v[0].y);
    if (v[1].y == v[0].y) {
        bound_us = 0;
        right_facing = v[1].x > v[0].x;
    } else {
        bound_us = raster_xfp_step(v[1].x - v[0].x, v[1].y - v[0].y);
        right_facing = bound_us > base_step;
    }
    bound_ls = (v[2].y == v[1].y) ? 0 :
        raster_xfp_step(v[2].x - v[1].x, v[2].y - v[1].y);

    /* Each half is walked away from the core vertex's side: downwards
     * from the top, or upwards from the bottom (dec_mode). */
    vo = core_vertex ? 1u : 0u;
    vp = (core_vertex == 2) ? 3u : 0u;
    part[vo].y_coord = v[0 ^ vo].y;
    part[vo].y_bound = v[1 ^ vo].y;
    part[vo].x_coord[right_facing] = raster_xfp(v[0 ^ vo].x);
    part[vo].x_step[right_facing] = bound_us;
    part[vo].x_coord[!right_facing] = base_coord +
        (u64)((s64)(v[vo].y - v[0].y) * base_step);
    part[vo].x_step[!right_facing] = base_step;
    part[vo].dec_mode = (int)vo;
    part[vo ^ 1].y_coord = v[1 ^ vp].y;
    part[vo ^ 1].y_bound = v[2 ^ vp].y;
    part[vo ^ 1].x_coord[right_facing] = raster_xfp(v[1 ^ vp].x);
    part[vo ^ 1].x_step[right_facing] = bound_ls;
    part[vo ^ 1].x_coord[!right_facing] = base_coord +
        (u64)((s64)(v[1 ^ vp].y - v[0].y) * base_step);
    part[vo ^ 1].x_step[!right_facing] = base_step;
    part[vo ^ 1].dec_mode = (int)vp;

    if (hires) {
        /* the shadow pass: the same walk over N x rows, no 11-bit wrap */
        s32 htop = top * g_hires.scale;
        s32 hbottom = bottom * g_hires.scale + g_hires.scale - 1;
        for (i = 0; i < 2; i++) {
            s32 yi = part[i].y_coord, yb = part[i].y_bound;
            u64 lc = part[i].x_coord[0], rc = part[i].x_coord[1];
            u64 ls = (u64)part[i].x_step[0], rs = (u64)part[i].x_step[1];
            if (part[i].dec_mode) {
                while (yi > yb) {
                    yi--;
                    lc -= ls;
                    rc -= rs;
                    if (yi < htop) break;
                    if (yi > hbottom) continue;
                    raster_span_hires(yi, raster_xfp_int(lc), raster_xfp_int(rc),
                                      &core, v[core_vertex].x, v[core_vertex].y,
                                      &d, mode);
                }
            } else {
                while (yi < yb) {
                    if (yi > hbottom) break;
                    if (yi >= htop)
                        raster_span_hires(yi, raster_xfp_int(lc), raster_xfp_int(rc),
                                          &core, v[core_vertex].x, v[core_vertex].y,
                                          &d, mode);
                    yi++;
                    lc += ls;
                    rc += rs;
                }
            }
        }
        return;
    }

    for (i = 0; i < 2; i++) {
        s32 yi = part[i].y_coord, yb = part[i].y_bound;
        u64 lc = part[i].x_coord[0], rc = part[i].x_coord[1];
        u64 ls = (u64)part[i].x_step[0], rs = (u64)part[i].x_step[1];
        if (part[i].dec_mode) {
            while (yi > yb) {
                s32 y;
                yi--;
                lc -= ls;
                rc -= rs;
                y = gpu_signed11((u32)yi);
                if (y < top) break;
                if (y > bottom) continue;
                raster_span(yi, raster_xfp_int(lc), raster_xfp_int(rc),
                            &core, v[core_vertex].x, v[core_vertex].y,
                            &d, mode);
            }
        } else {
            while (yi < yb) {
                s32 y = gpu_signed11((u32)yi);
                if (y > bottom) break;
                if (y >= top)
                    raster_span(yi, raster_xfp_int(lc), raster_xfp_int(rc),
                                &core, v[core_vertex].x, v[core_vertex].y,
                                &d, mode);
                yi++;
                lc += ls;
                rc += rs;
            }
        }
    }
}

/* Draw a flat-shaded triangle with optional blending */
void draw_poly_f3_blend(s32 x0, s32 y0, s32 x1, s32 y1, s32 x2, s32 y2,
                        u8 r, u8 g, u8 b, int blend, s32 abr) {
    RasterVertex v[3];
    RasterMode mode;
    int i;

    if (!gpu_triangle_fits(x0, y0, x1, y1, x2, y2)) return;
    memset(&mode, 0, sizeof(mode));
    memset(v, 0, sizeof(v));
    v[0].x = x0; v[0].y = y0;
    v[1].x = x1; v[1].y = y1;
    v[2].x = x2; v[2].y = y2;
    for (i = 0; i < 3; i++) {
        v[i].x += g_draw_x; v[i].y += g_draw_y;
        v[i].r = r; v[i].g = g; v[i].b = b;
    }
    mode.blend = blend;
    mode.abr = abr;
    gpu_raster_triangle(v, &mode);
}

/* Legacy wrapper for compatibility */
void draw_poly_f3(s32 x0, s32 y0, s32 x1, s32 y1, s32 x2, s32 y2,
                  u8 r, u8 g, u8 b) {
    draw_poly_f3_blend(x0, y0, x1, y1, x2, y2, r, g, b, 0, 0);
}

/* Draw a gouraud-shaded triangle (per-vertex RGB) with optional blending.
 * Applies PS1 dithering when E1 enables it. */
void draw_poly_g3_blend(s32 x0, s32 y0, u8 r0, u8 g0, u8 b0,
                        s32 x1, s32 y1, u8 r1, u8 g1, u8 b1,
                        s32 x2, s32 y2, u8 r2, u8 g2, u8 b2,
                        int blend, s32 abr) {
    RasterVertex v[3];
    RasterMode mode;
    int i;

    if (!gpu_triangle_fits(x0, y0, x1, y1, x2, y2)) return;
    memset(&mode, 0, sizeof(mode));
    memset(v, 0, sizeof(v));
    v[0].x = x0; v[0].y = y0; v[0].r = r0; v[0].g = g0; v[0].b = b0;
    v[1].x = x1; v[1].y = y1; v[1].r = r1; v[1].g = g1; v[1].b = b1;
    v[2].x = x2; v[2].y = y2; v[2].r = r2; v[2].g = g2; v[2].b = b2;
    for (i = 0; i < 3; i++) {
        v[i].x += g_draw_x; v[i].y += g_draw_y;
    }
    mode.gouraud = 1;
    mode.blend = blend;
    mode.abr = abr;
    gpu_raster_triangle(v, &mode);
}

/* Legacy wrapper for compatibility */
void draw_poly_g3(s32 x0, s32 y0, u8 r0, u8 g0, u8 b0,
                  s32 x1, s32 y1, u8 r1, u8 g1, u8 b1,
                  s32 x2, s32 y2, u8 r2, u8 g2, u8 b2) {
    draw_poly_g3_blend(x0, y0, r0, g0, b0, x1, y1, r1, g1, b1, x2, y2, r2, g2, b2, 0, 0);
}

static void draw_poly_gt3_mode(s32 x0, s32 y0, u8 u0, u8 v0, u8 r0, u8 g0, u8 b0,
                         s32 x1, s32 y1, u8 u1, u8 v1, u8 r1, u8 g1, u8 b1,
                         s32 x2, s32 y2, u8 u2, u8 v2, u8 r2, u8 g2, u8 b2,
                         u16 clut, u16 tpage, int blend, s32 abr, int flags);

/* Draw a flat-textured triangle (POLY_FT3) with optional blending.
 * Per-vertex UV coords, single RGB modulation (gpu_raster_triangle). */
static void draw_poly_ft3_mode(s32 x0, s32 y0, u8 u0, u8 v0,
                         s32 x1, s32 y1, u8 u1, u8 v1,
                         s32 x2, s32 y2, u8 u2, u8 v2,
                         u16 clut, u16 tpage, u8 r, u8 g, u8 b,
                         int blend, s32 abr, int raw) {
    draw_poly_gt3_mode(x0, y0, u0, v0, r, g, b, x1, y1, u1, v1, r, g, b,
                       x2, y2, u2, v2, r, g, b, clut, tpage, blend, abr,
                       raw | 2);
}

/* Existing native callers request modulated texture color. */
void draw_poly_ft3_blend(s32 x0, s32 y0, u8 u0, u8 v0,
                         s32 x1, s32 y1, u8 u1, u8 v1,
                         s32 x2, s32 y2, u8 u2, u8 v2,
                         u16 clut, u16 tpage, u8 r, u8 g, u8 b,
                         int blend, s32 abr) {
    draw_poly_ft3_mode(x0, y0, u0, v0, x1, y1, u1, v1, x2, y2, u2, v2,
                      clut, tpage, r, g, b, blend, abr, 0);
}

/* Legacy wrapper for compatibility */
void draw_poly_ft3(s32 x0, s32 y0, u8 u0, u8 v0,
                   s32 x1, s32 y1, u8 u1, u8 v1,
                   s32 x2, s32 y2, u8 u2, u8 v2,
                   u16 clut, u16 tpage, u8 r, u8 g, u8 b) {
    draw_poly_ft3_blend(x0, y0, u0, v0, x1, y1, u1, v1, x2, y2, u2, v2,
                        clut, tpage, r, g, b, 0, 0);
}

/* Draw a textured triangle with optional blending: per-vertex UV and,
 * unless flags bit 1 marks a flat (FT) polygon, per-vertex RGB
 * modulation; flags bit 0 selects raw texture.  Applies PS1 dithering. */
static void draw_poly_gt3_mode(s32 x0, s32 y0, u8 u0, u8 v0, u8 r0, u8 g0, u8 b0,
                         s32 x1, s32 y1, u8 u1, u8 v1, u8 r1, u8 g1, u8 b1,
                         s32 x2, s32 y2, u8 u2, u8 v2, u8 r2, u8 g2, u8 b2,
                         u16 clut, u16 tpage, int blend, s32 abr, int flags) {
    RasterVertex v[3];
    RasterMode mode;
    int i;

    gpu_clut_cache_update(clut, tpage);
    if (!gpu_triangle_fits(x0, y0, x1, y1, x2, y2)) return;
    v[0].x = x0; v[0].y = y0; v[0].r = r0; v[0].g = g0; v[0].b = b0;
    v[1].x = x1; v[1].y = y1; v[1].r = r1; v[1].g = g1; v[1].b = b1;
    v[2].x = x2; v[2].y = y2; v[2].r = r2; v[2].g = g2; v[2].b = b2;
    v[0].u = u0; v[0].v = v0;
    v[1].u = u1; v[1].v = v1;
    v[2].u = u2; v[2].v = v2;
    for (i = 0; i < 3; i++) {
        v[i].x += g_draw_x; v[i].y += g_draw_y;
    }
    memset(&mode, 0, sizeof(mode));
    mode.gouraud = !(flags & 2);
    mode.textured = 1;
    mode.raw = flags & 1;
    mode.blend = blend;
    mode.abr = abr;
    mode.clut = clut;
    mode.tpage = tpage;
    gpu_raster_triangle(v, &mode);
}

void draw_poly_gt3_blend(s32 x0, s32 y0, u8 u0, u8 v0, u8 r0, u8 g0, u8 b0,
                         s32 x1, s32 y1, u8 u1, u8 v1, u8 r1, u8 g1, u8 b1,
                         s32 x2, s32 y2, u8 u2, u8 v2, u8 r2, u8 g2, u8 b2,
                         u16 clut, u16 tpage, int blend, s32 abr) {
    draw_poly_gt3_mode(x0, y0, u0, v0, r0, g0, b0,
                      x1, y1, u1, v1, r1, g1, b1,
                      x2, y2, u2, v2, r2, g2, b2, clut, tpage, blend, abr, 0);
}

/* Legacy wrapper for compatibility */
void draw_poly_gt3(s32 x0, s32 y0, u8 u0, u8 v0, u8 r0, u8 g0, u8 b0,
                   s32 x1, s32 y1, u8 u1, u8 v1, u8 r1, u8 g1, u8 b1,
                   s32 x2, s32 y2, u8 u2, u8 v2, u8 r2, u8 g2, u8 b2,
                   u16 clut, u16 tpage) {
    draw_poly_gt3_blend(x0, y0, u0, v0, r0, g0, b0,
                        x1, y1, u1, v1, r1, g1, b1,
                        x2, y2, u2, v2, r2, g2, b2,
                        clut, tpage, 0, 0);
}

/* Gouraud 2-point line (LINE_G2, GP0 opcode 0x50) with optional blending.
 * Bresenham with per-endpoint color interp. Clips to VRAM. Applies PS1 dithering. */
void draw_line_g2_blend(s32 x0, s32 y0, u8 r0, u8 g0, u8 b0,
                        s32 x1, s32 y1, u8 r1, u8 g1, u8 b1,
                        int blend, s32 abr) {
    s32 dx, dy, sx, sy, err, e2;
    s32 steps, step;
    if (!gpu_span_fits(x0, x1, 1024) || !gpu_span_fits(y0, y1, 512)) return;
    x0 += g_draw_x; y0 += g_draw_y;
    x1 += g_draw_x; y1 += g_draw_y;
    dx = x1 - x0; if (dx < 0) dx = -dx;
    dy = y1 - y0; if (dy < 0) dy = -dy;
    sx = (x0 < x1) ? 1 : -1;
    sy = (y0 < y1) ? 1 : -1;
    err = dx - dy;
    steps = (dx > dy ? dx : dy);
    if (steps < 1) steps = 1;
    step = 0;
    while (1) {
        if (x0 >= 0 && x0 < VRAM_WIDTH && y0 >= 0 && y0 < VRAM_HEIGHT) {
            s32 r = r0 + ((s32)(r1 - r0) * step) / steps;
            s32 g = g0 + ((s32)(g1 - g0) * step) / steps;
            s32 b = b0 + ((s32)(b1 - b0) * step) / steps;
            u16 pixel = dither_and_pack(x0, y0, r, g, b);
            gpu_raster_store(x0, y0, pixel, blend, abr);
        }
        if (x0 == x1 && y0 == y1) break;
        e2 = err * 2;   /* err may be negative: not a shift */
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
        step++;
        if (step > steps) break;
    }
}

/* Legacy wrapper for compatibility */
void draw_line_g2(s32 x0, s32 y0, u8 r0, u8 g0, u8 b0,
                  s32 x1, s32 y1, u8 r1, u8 g1, u8 b1) {
    draw_line_g2_blend(x0, y0, r0, g0, b0, x1, y1, r1, g1, b1, 0, 0);
}

/* Get GPU info reads device state, not the possibly stale SDK cache. */
u32 gpu_clear_get_info(s32 which) {
    if (which == 3) return g_area_tl;
    if (which == 4) return g_area_br;
    if (which == 5) return g_draw_offset;
    return 0;
}

static u32 gpu_clear_tile(u32 color, u32 position, u32 size) {
    s32 x, y, right, bottom, column, row;
    s32 left, top, area_right, area_bottom;
    u16 pixel;
    left = g_area_tl & 0x3ffu;
    top = (g_area_tl >> 10) & 0x3ffu;
    area_right = g_area_br & 0x3ffu;
    area_bottom = (g_area_br >> 10) & 0x3ffu;
    if (area_right < left || area_bottom < top) return 16u;
    x = gpu_signed11(gpu_signed11(position) + g_draw_x);
    y = gpu_signed11(gpu_signed11(position >> 16) + g_draw_y);
    right = x + (s32)(size & 0x3ffu) - 1;
    bottom = y + (s32)((size >> 16) & 0x1ffu) - 1;
    if (x < left) x = left;
    if (y < top) y = top;
    if (right > area_right) right = area_right;
    if (bottom > area_bottom) bottom = area_bottom;
    pixel = ((color >> 3) & 31u) | ((color >> 6) & 0x3e0u) |
            ((color >> 9) & 0x7c00u);
    /* Logical drawing-area Y is ten bits. Rows above 511 alias the
     * same physical VRAM; the reference corner probe exercises this.
     * Clear packets precede the tile with E6=0; plain PutDrawEnv also
     * emits E6=0. These opaque, untextured pixels share the same VRAM
     * backing with either renderer. No nested SDK submission occurs. */
    for (row = y; row <= bottom; row++)
        for (column = x; column <= right; column++)
            g_vram[row & 511][column] = pixel;
    if (HIRES_ON() && right >= x && bottom >= y)
        gpu_hires_mirror_rect(x, y, right - x + 1, bottom - y + 1);
    /* Reference timing clips at physical row 511 even where its renderer
     * wraps Y. Keep that approximation separate from the pixel operation. */
    if (bottom > 511) bottom = 511;
    if (right < x || bottom < y) return 16u;
    return 16u + (u32)(right - x + 1) * (u32)(bottom - y + 1);
}

int gpu_execute_clear_words(const u32 *words, u32 count) {
    u32 i, command, width, height, ticks;
    for (i = 0; i < count; i++) {
        command = words[i];
        if (gpu_draw_command(command)) {
            if (command >> 24) gpu_queue_work(1u);
            continue;
        }
        if (i + 2 >= count) return 0;
        if ((command >> 24) == 2) {
            gpu_vram_fill(&g_vram[0][0], command, words[i+1], words[i+2]);
            hires_mirror_fill(words[i+1], words[i+2]);
            width = ((words[i+2] & 1023u) + 15u) & ~15u;
            height = (words[i+2] >> 16) & 511u;
            ticks = 46u + (width / 8u + 9u) * height;
        } else if ((command >> 24) == 0x60)
            ticks = gpu_clear_tile(command, words[i+1], words[i+2]);
        else return 0;
        gpu_queue_work(ticks);
        i += 2;
    }
    return 1;
}

/* Decode supported GP0 drawing-state words without asset substitutions. */
static int gpu_draw_command(u32 command) {
    switch (command >> 24) {
    case 0x00: /* NOP, including get_tw(NULL)'s zero word. */
        return 1;
    case 0x01: /* Clear texture and CLUT caches. */
        gpu_cache_clear();
        return 1;
    case 0xE1:
        gpu_set_texpage_state(command & 15, (command >> 4) & 1,
                              (command >> 7) & 3, (command >> 5) & 3);
        g_dither_enabled = (command >> 9) & 1;
        if (mdfn_gpu_enabled())
            gpu_backend_tpage((u16)command);
        g_frame_dr_mode_count++;
        return 1;
    case 0xE2:
        gpu_set_texture_window(command & 31, (command >> 5) & 31,
                               (command >> 10) & 31,
                               (command >> 15) & 31);
        if (mdfn_gpu_enabled())
            mdfn_set_texwindow(g_tex_window_mask_x, g_tex_window_mask_y,
                              g_tex_window_off_x, g_tex_window_off_y);
        return 1;
    case 0xE3:
    case 0xE4:
        if ((command >> 24) == 0xe3) g_area_tl = command & 0xfffffu;
        else g_area_br = command & 0xfffffu;
        g_draw_w = (s32)(g_area_br & 1023u) -
                   (s32)(g_area_tl & 1023u) + 1;
        g_draw_h = (s32)((g_area_br >> 10) & 1023u) -
                   (s32)((g_area_tl >> 10) & 1023u) + 1;
        if (g_draw_w < 0) g_draw_w = 0;
        if (g_draw_h < 0) g_draw_h = 0;
        if (mdfn_gpu_enabled())
            mdfn_set_drawing_area(g_area_tl & 1023u,
                (g_area_tl >> 10) & 1023u, g_area_br & 1023u,
                (g_area_br >> 10) & 1023u);
        return 1;
    case 0xE5:
        g_draw_offset = command & 0x3fffffu;
        g_draw_x = gpu_signed11(command);
        g_draw_y = gpu_signed11(command >> 11);
        if (mdfn_gpu_enabled()) mdfn_set_draw_offset(g_draw_x, g_draw_y);
        return 1;
    case 0xE6:
        g_mask_set_or = (command & 1) ? 0x8000 : 0;
        g_mask_eval_and = (command & 2) ? 0x8000 : 0;
        if (mdfn_gpu_enabled())
            mdfn_set_mask(g_mask_set_or, g_mask_eval_and);
        return 1;
    default:
        return 0;
    }
}

/* Decode textured polygon records once at the native device boundary.
 * PS1 game records retain their original layout; only host-local vertices
 * reach either rasterizer. Polygon page fields update GPUSTAT bits 0..8;
 * E1-owned dither and display-area controls are not changed by that field.
 */
static void gpu_draw_textured_polygon(const u8 *prim, u8 code, u8 words)
{
    MdfnVertex vertices[4];
    MdfnTri triangle;
    const u8 *cursor, *color;
    u16 clut, page;
    int count, gouraud, raw, blend, i, face, index;
    count = (code & 8) ? 4 : 3;
    gouraud = (code & 16) != 0;
    if (words < (gouraud ? count * 3 : count * 2 + 1)) return;
    raw = code & 1;
    blend = (code & 2) != 0;
    color = prim;
    cursor = prim + 4;
    clut = page = 0;
    for (i = 0; i < count; i++) {
        if (i && gouraud) {
            color = cursor;
            cursor += 4;
        }
        vertices[i].r = color[0];
        vertices[i].g = color[1];
        vertices[i].b = color[2];
        vertices[i].x = *(const s16 *)cursor;
        vertices[i].y = *(const s16 *)(cursor + 2);
        cursor += 4;
        vertices[i].u = cursor[0];
        vertices[i].v = cursor[1];
        if (i == 0) clut = *(const u16 *)(cursor + 2);
        if (i == 1) page = *(const u16 *)(cursor + 2);
        cursor += 4;
    }
    gpu_set_texpage_state(page & 15, (page >> 4) & 1, (page >> 7) & 3,
                          (page >> 5) & 3);
    memset(&triangle, 0, sizeof(triangle));
    triangle.textured = 1;
    triangle.gouraud = gouraud;
    triangle.tex_mult = !raw;
    triangle.blend = mdfn_blend_for(blend, g_tpage_abr);
    triangle.mask_eval = g_mask_eval_and;
    if (mdfn_gpu_enabled()) {
        gpu_backend_tpage(page);
        mdfn_set_clut(clut);
    }
    for (face = 0; face < count - 2; face++) {
        for (i = 0; i < 3; i++) {
            index = face + i;
            triangle.verts[i] = vertices[index];
        }
        if (mdfn_gpu_enabled()) {
            mdfn_draw_triangle(&triangle);
        } else {
            const MdfnVertex *a, *b, *c;
            a = &triangle.verts[0];
            b = &triangle.verts[1];
            c = &triangle.verts[2];
            if (gouraud)
                draw_poly_gt3_mode(a->x, a->y, a->u, a->v, a->r, a->g, a->b,
                    b->x, b->y, b->u, b->v, b->r, b->g, b->b,
                    c->x, c->y, c->u, c->v, c->r, c->g, c->b,
                    clut, page, blend, g_tpage_abr, raw);
            else
                draw_poly_ft3_mode(a->x, a->y, a->u, a->v,
                    b->x, b->y, b->u, b->v, c->x, c->y, c->u, c->v,
                    clut, page, a->r, a->g, a->b, blend, g_tpage_abr, raw);
        }
    }
    g_frame_poly_count++;
}

/* Replace each vertex word of a render command with its decoded signed
 * 11-bit X/Y (PSX-SPX "Vertex": bits 11..15 are ignored).  Both backends
 * add the E5h drawing offset themselves; rectangles also wrap the sum. */
static void gpu_decode_vertices(u32 *words, unsigned int count)
{
    u32 op = words[0] >> 24;
    unsigned int at, step;
    if (op < 0x40) {          /* polygon: vertex, UV, colour words */
        step = 1u + ((op & 4) ? 1u : 0) + ((op & 0x10) ? 1u : 0);
    } else if (op < 0x60) {   /* two-point line */
        step = (op & 0x10) ? 2u : 1u;
    } else {                  /* rectangle: one corner */
        step = count;
    }
    for (at = 1; at < count; at += step)
        words[at] = ((u32)gpu_signed11(words[at]) & 0xFFFFu) |
                    ((u32)gpu_signed11(words[at] >> 16) << 16);
}

/* The sub-pixel fractions of a polygon's vertices (gpu_pgxp.h), from its
 * RAW vertex words (before gpu_decode_vertices): all of them, or none.
 * Primitive triples first (v0 v1 v2; a quad's v1 v2 v3), then single
 * words for whatever is left. */
static void pgxp_prim_lookup(const u32 *words, unsigned int count)
{
    u32 op = words[0] >> 24;
    unsigned int at, step = 1u + ((op & 4) ? 1u : 0) + ((op & 0x10) ? 1u : 0);
    int k, nv = (op & 8) ? 4 : 3, known = 0, from_tri = 0, t;
    u32 w[4], fx[4], fy[4];
    int have[4] = { 0, 0, 0, 0 };
    g_pgxp_prim.n = 0;
    for (k = 0, at = 1; k < nv; k++, at += step) {
        if (at >= count) return;
        w[k] = words[at];
    }
    g_gpu_pgxp_stats.polys++;
    g_gpu_pgxp_stats.verts += (unsigned long)nv;
    for (t = 0; t + 3 <= nv; t++) {
        u32 tx[3], ty[3];
        int r = gpu_pgxp_lookup_tri(&w[t], tx, ty);
        if (r) known = 1;
        if (r <= 0) continue;
        for (k = 0; k < 3; k++) {
            if (have[t + k] && (fx[t + k] != tx[k] || fy[t + k] != ty[k])) {
                g_gpu_pgxp_stats.verts_ambig++;
                return;     /* two triples disagree on a shared vertex */
            }
            fx[t + k] = tx[k];
            fy[t + k] = ty[k];
            have[t + k] = 1;
        }
    }
    for (k = 0, from_tri = 1; k < nv; k++) {
        int r;
        if (have[k]) continue;
        from_tri = 0;
        r = gpu_pgxp_lookup(w[k], &fx[k], &fy[k]);
        if (r) known = 1;
        if (r < 0) g_gpu_pgxp_stats.verts_ambig++;
        have[k] = r > 0;
    }
    for (k = 0; k < nv; k++) {
        if (!have[k]) {
            if (known) g_gpu_pgxp_stats.polys_gte++;
            return;
        }
    }
    for (k = 0; k < nv; k++) {
        g_pgxp_prim.x[k] = gpu_signed11(w[k]) + g_draw_x;
        g_pgxp_prim.y[k] = gpu_signed11(w[k] >> 16) + g_draw_y;
        g_pgxp_prim.fx[k] = fx[k];
        g_pgxp_prim.fy[k] = fy[k];
    }
    g_pgxp_prim.n = nv;
    g_gpu_pgxp_stats.verts_hit += (unsigned long)nv;
    g_gpu_pgxp_stats.polys_gte++;
    g_gpu_pgxp_stats.polys_precise++;
    if (from_tri) g_gpu_pgxp_stats.polys_tri++;
}

/* The shadow vertex of native vertex `in` (scale s): its fractions when the
 * current polygon has them (one entry at that position, or several that
 * agree); 0 when it must be drawn from the integers. */
static int pgxp_shadow_vertex(const RasterVertex *in, s32 s, RasterVertex *out)
{
    int k, found = -1;
    for (k = 0; k < g_pgxp_prim.n; k++) {
        if (g_pgxp_prim.x[k] != in->x || g_pgxp_prim.y[k] != in->y) continue;
        if (found >= 0 && (g_pgxp_prim.fx[k] != g_pgxp_prim.fx[found] ||
                           g_pgxp_prim.fy[k] != g_pgxp_prim.fy[found]))
            return 0;
        found = k;
    }
    if (found < 0) return 0;
    out->x = in->x * s + (s32)((g_pgxp_prim.fx[found] * (u32)s) >> 16);
    out->y = in->y * s + (s32)((g_pgxp_prim.fy[found] * (u32)s) >> 16);
    {
        double f[2], e;
        int a;
        f[0] = g_pgxp_prim.fx[found] / 65536.0;
        f[1] = g_pgxp_prim.fy[found] / 65536.0;
        for (a = 0; a < 2; a++) {
            g_gpu_pgxp_stats.err_n += 1;
            g_gpu_pgxp_stats.err_int_sum += f[a];
            g_gpu_pgxp_stats.err_int_sq += f[a] * f[a];
            e = f[a] - (double)(s32)(f[a] * s) / s;
            g_gpu_pgxp_stats.err_sub_sum += e;
            g_gpu_pgxp_stats.err_sub_sq += e * e;
        }
    }
    return 1;
}

/* Draw one complete GP0 render command (polygon, line or rectangle).
 * packet addresses its command word and len is the command's own word
 * count from gp0_command_words, never the rest of the packet.  Decoding
 * works on a copy: the caller's packet is game memory. */
static void gpu_draw_primitive(u8 *packet, u8 len)
{
    u32 command[12];
    u8 *prim = (u8 *)command;
    u8 r0, g0, b0, code;
    if (len > 12) len = 12; /* render commands are at most 12 words */
    memcpy(command, packet, (size_t)len * 4u);
    if (PGXP_ON() && (command[0] >> 24) >= 0x20 && (command[0] >> 24) < 0x40)
        pgxp_prim_lookup(command, len);
    gpu_decode_vertices(command, len);
    r0 = prim[0]; g0 = prim[1]; b0 = prim[2]; code = prim[3];
    switch (code & 0xFC) {

    case 0x60: { /* TILE */
        s16 x0 = *(s16 *)(prim + 4);
        s16 y0 = *(s16 *)(prim + 6);
        s16 w = *(s16 *)(prim + 8);
        s16 h = *(s16 *)(prim + 10);
        int blend = (code & 2) ? 1 : 0;
        if (mdfn_gpu_enabled()) {
            MdfnSprite s = {0};
            s.x = x0; s.y = y0; s.w = w; s.h = h;
            s.r = r0; s.g = g0; s.b = b0;
            s.textured = 0;
            s.blend = mdfn_blend_for(blend, g_tpage_abr);
            s.mask_eval = g_mask_eval_and;
            mdfn_draw_sprite(&s);
        } else {
            draw_tile_blend(x0, y0, w, h, r0, g0, b0, blend, g_tpage_abr);
        }
        g_frame_tile_count++;
        break;
    }
    case 0x64: { /* SPRT */
        static int sprt_trace = 0;
        s16 x0 = *(s16 *)(prim + 4);
        s16 y0 = *(s16 *)(prim + 6);
        u8 u0 = prim[8];
        u8 v0 = prim[9];
        u16 clut = *(u16 *)(prim + 10);
        s16 w = *(s16 *)(prim + 12);
        s16 h = *(s16 *)(prim + 14);
        int blend = (code & 2) ? 1 : 0;
        /* Portrait / non-mascot sprite shapes are diagnostics, not output:
         * ungated they emitted an 8 MiB log on single scenes (208 hit the
         * scene-matrix OUTPUT_LIMIT), because a scene drawing thousands of
         * sprites wrote a line each, every frame.  GP0_TRACE=1 arms them,
         * each with its own cap. */
        if (w == 128 && h == 200 && gp0_trace_enabled()) {
            static int portrait_trace = 0;
            if (portrait_trace < 20) {
                s32 clut_x = (clut & 0x3F) << 4;
                s32 clut_y = (clut >> 6) & 0x1FF;
                printf("[GPU] PORTRAIT SPRT: pos=(%d,%d) size=%dx%d uv=(%d,%d) clut=0x%04X→(%d,%d) tpage=%d,bpp=%d at %p\n",
                       x0, y0, w, h, u0, v0, clut, clut_x, clut_y, g_tpage_x, g_tpage_bpp, (void*)prim);
                portrait_trace++;
            }
        }
        /* Trace non-mascot SPRTs (anything not 32x32) — helps identify logo/title pieces. */
        if (!(w == 32 && h == 32) && gp0_trace_enabled()) {
            static int shape_trace = 0;
            if (shape_trace < 40) {
                s32 clut_x = (clut & 0x3F) << 4;
                s32 clut_y = (clut >> 6) & 0x1FF;
                printf("[GPU] NON-MASCOT SPRT: pos=(%d,%d) size=%dx%d uv=(%d,%d) clut=0x%04X→(%d,%d) tpage=%d,bpp=%d\n",
                       x0, y0, w, h, u0, v0, clut, clut_x, clut_y, g_tpage_x, g_tpage_bpp);
                shape_trace++;
            }
        }
        /* Trace mascot 32x32 sprites with UV >= 128 (MAINROM texture area) */
        if (w == 32 && h == 32 && u0 >= 128 && v0 >= 128 && gp0_trace_enabled()) {
            static int mascot_trace = 0;
            if (mascot_trace < 10) {
                s32 clut_x = (clut & 0x3F) << 4;
                s32 clut_y = (clut >> 6) & 0x1FF;
                /* Compute expected VRAM position for 4bpp mode */
                s32 vram_x_4bpp = g_tpage_x * 64 + u0 / 4;
                s32 vram_y_4bpp = g_tpage_y * 256 + v0;
                printf("[GPU] MASCOT SPRT[%d]: pos=(%d,%d) uv=(%d,%d) clut=(%d,%d) GLOBAL_TPAGE=%d,bpp=%d -> VRAM(%d,%d)\n",
                       mascot_trace, x0, y0, u0, v0, clut_x, clut_y, g_tpage_x, g_tpage_bpp, vram_x_4bpp, vram_y_4bpp);
                mascot_trace++;
            }
        }
        if (sprt_trace < 20 && gp0_trace_enabled()) {
            s32 clut_x = (clut & 0x3F) << 4;
            s32 clut_y = (clut >> 6) & 0x1FF;
            printf("[GPU] SPRT[%d]: pos=(%d,%d) size=%dx%d uv=(%d,%d) clut=0x%04X→(%d,%d) tpage=%d,bpp=%d\n",
                   sprt_trace, x0, y0, w, h, u0, v0, clut, clut_x, clut_y, g_tpage_x, g_tpage_bpp);
            sprt_trace++;
        }
        if (mdfn_gpu_enabled()) {
                MdfnSprite s = {0};
                s.x = x0; s.y = y0; s.w = w; s.h = h;
                s.u = u0; s.v = v0;
                s.r = r0; s.g = g0; s.b = b0;
                s.textured = 1;
                s.tex_mult = (code & 1) ? 0 : 1;
                s.blend = mdfn_blend_for(blend, g_tpage_abr);
            s.mask_eval = g_mask_eval_and;
                mdfn_set_clut(clut);
                gpu_backend_tpage((uint16_t)(g_tpage_x | (g_tpage_y << 4) |
                                            (g_tpage_abr << 5) | (g_tpage_bpp << 7)));
                mdfn_draw_sprite(&s);
            } else {
                draw_sprt_blend(x0, y0, w, h, u0, v0, clut,
                                g_tpage_x | (g_tpage_y << 4) | (g_tpage_bpp << 7),
                                (code & 1) ? 128 : r0,
                            (code & 1) ? 128 : g0,
                            (code & 1) ? 128 : b0,
                            blend, g_tpage_abr);
            }
        g_frame_sprt_count++;
        break;
    }
    case 0x7C: { /* SPRT_16 */
        s16 x0 = *(s16 *)(prim + 4);
        s16 y0 = *(s16 *)(prim + 6);
        u8 u0 = prim[8];
        u8 v0 = prim[9];
        u16 clut = *(u16 *)(prim + 10);
        int blend = (code & 2) ? 1 : 0;
        if (mdfn_gpu_enabled()) {
            MdfnSprite s = {0};
            s.x = x0; s.y = y0; s.w = 16; s.h = 16;
            s.u = u0; s.v = v0;
            s.r = r0; s.g = g0; s.b = b0;
            s.textured = 1;
            s.tex_mult = (code & 1) ? 0 : 1;
            s.blend = mdfn_blend_for(blend, g_tpage_abr);
            s.mask_eval = g_mask_eval_and;
            mdfn_set_clut(clut);
            gpu_backend_tpage((uint16_t)(g_tpage_x | (g_tpage_y << 4) |
                                        (g_tpage_abr << 5) | (g_tpage_bpp << 7)));
            mdfn_draw_sprite(&s);
        } else {
            draw_sprt_blend(x0, y0, 16, 16, u0, v0, clut,
                            g_tpage_x | (g_tpage_y << 4) | (g_tpage_bpp << 7),
                            (code & 1) ? 128 : r0,
                            (code & 1) ? 128 : g0,
                            (code & 1) ? 128 : b0,
                            blend, g_tpage_abr);
        }
        g_frame_sprt_count++;
        break;
    }
    case 0x74: { /* SPRT_8 */
        s16 x0 = *(s16 *)(prim + 4);
        s16 y0 = *(s16 *)(prim + 6);
        u8 u0 = prim[8];
        u8 v0 = prim[9];
        u16 clut = *(u16 *)(prim + 10);
        int blend = (code & 2) ? 1 : 0;
        if (mdfn_gpu_enabled()) {
            MdfnSprite s = {0};
            s.x = x0; s.y = y0; s.w = 8; s.h = 8;
            s.u = u0; s.v = v0;
            s.r = r0; s.g = g0; s.b = b0;
            s.textured = 1;
            s.tex_mult = (code & 1) ? 0 : 1;
            s.blend = mdfn_blend_for(blend, g_tpage_abr);
            s.mask_eval = g_mask_eval_and;
            mdfn_set_clut(clut);
            gpu_backend_tpage((uint16_t)(g_tpage_x | (g_tpage_y << 4) |
                                        (g_tpage_abr << 5) | (g_tpage_bpp << 7)));
            mdfn_draw_sprite(&s);
        } else {
            draw_sprt_blend(x0, y0, 8, 8, u0, v0, clut,
                            g_tpage_x | (g_tpage_y << 4) | (g_tpage_bpp << 7),
                            (code & 1) ? 128 : r0,
                            (code & 1) ? 128 : g0,
                            (code & 1) ? 128 : b0,
                            blend, g_tpage_abr);
        }
        g_frame_sprt_count++;
        break;
    }
    case 0x68:   /* TILE_1 */
    case 0x70:   /* TILE_8 */
    case 0x78: { /* TILE_16 */
        s16 tx = *(s16*)(prim+4), ty = *(s16*)(prim+6);
        u8 cm = code & 0xFC;
        s16 tw = (cm == 0x68) ? 1 : (cm == 0x70 ? 8 : 16);
        int blend = (code & 2) ? 1 : 0;
        if (mdfn_gpu_enabled()) {
            MdfnSprite s = {0};
            s.x = tx; s.y = ty; s.w = tw; s.h = tw;
            s.r = r0; s.g = g0; s.b = b0;
            s.textured = 0;
            s.blend = mdfn_blend_for(blend, g_tpage_abr);
            s.mask_eval = g_mask_eval_and;
            mdfn_draw_sprite(&s);
        } else {
            draw_tile_blend(tx, ty, tw, tw, r0, g0, b0, blend, g_tpage_abr);
        }
        g_frame_tile_count++;
        break;
    }
    case 0x20: { /* POLY_F3 */
        s16 x0p = *(s16*)(prim+4), y0p = *(s16*)(prim+6);
        s16 x1p = *(s16*)(prim+8), y1p = *(s16*)(prim+10);
        s16 x2p = *(s16*)(prim+12), y2p = *(s16*)(prim+14);
        int blend = (code & 2) ? 1 : 0;
        if (mdfn_gpu_enabled()) {
            MdfnTri t = {0};
            t.verts[0].x = x0p; t.verts[0].y = y0p;
            t.verts[1].x = x1p; t.verts[1].y = y1p;
            t.verts[2].x = x2p; t.verts[2].y = y2p;
            t.verts[0].r = t.verts[1].r = t.verts[2].r = r0;
            t.verts[0].g = t.verts[1].g = t.verts[2].g = g0;
            t.verts[0].b = t.verts[1].b = t.verts[2].b = b0;
            t.blend = mdfn_blend_for(blend, g_tpage_abr);
            t.mask_eval = g_mask_eval_and;
            mdfn_draw_triangle(&t);
        } else {
            draw_poly_f3_blend(x0p, y0p, x1p, y1p, x2p, y2p, r0, g0, b0, blend, g_tpage_abr);
        }
        g_frame_poly_count++;
        break;
    }
    case 0x24: {
        gpu_draw_textured_polygon(prim, code, len);
        break;
    }
    case 0x28: { /* POLY_F4 — two triangles */
        s16 x0p = *(s16*)(prim+4),  y0p = *(s16*)(prim+6);
        s16 x1p = *(s16*)(prim+8),  y1p = *(s16*)(prim+10);
        s16 x2p = *(s16*)(prim+12), y2p = *(s16*)(prim+14);
        s16 x3p = *(s16*)(prim+16), y3p = *(s16*)(prim+18);
        int blend = (code & 2) ? 1 : 0;
        if (mdfn_gpu_enabled()) {
            MdfnTri t = {0};
            t.verts[0].r = t.verts[1].r = t.verts[2].r = r0;
            t.verts[0].g = t.verts[1].g = t.verts[2].g = g0;
            t.verts[0].b = t.verts[1].b = t.verts[2].b = b0;
            t.blend = mdfn_blend_for(blend, g_tpage_abr);
            t.mask_eval = g_mask_eval_and;
            t.verts[0].x = x0p; t.verts[0].y = y0p;
            t.verts[1].x = x1p; t.verts[1].y = y1p;
            t.verts[2].x = x2p; t.verts[2].y = y2p;
            mdfn_draw_triangle(&t);
            t.verts[0].x = x1p; t.verts[0].y = y1p;
            t.verts[1].x = x2p; t.verts[1].y = y2p;
            t.verts[2].x = x3p; t.verts[2].y = y3p;
            mdfn_draw_triangle(&t);
        } else {
            draw_poly_f3_blend(x0p, y0p, x1p, y1p, x2p, y2p, r0, g0, b0, blend, g_tpage_abr);
            draw_poly_f3_blend(x1p, y1p, x2p, y2p, x3p, y3p, r0, g0, b0, blend, g_tpage_abr);
        }
        g_frame_poly_count++;
        break;
    }
    case 0x2C: {
        gpu_draw_textured_polygon(prim, code, len);
        break;
    }
    case 0x30: { /* POLY_G3 — gouraud triangle */
        /* Layout: r0g0b0code | x0y0 | r1g1b1_ | x1y1 | r2g2b2_ | x2y2 */
        s16 x0p = *(s16*)(prim+4),  y0p = *(s16*)(prim+6);
        s16 x1p = *(s16*)(prim+12), y1p = *(s16*)(prim+14);
        s16 x2p = *(s16*)(prim+20), y2p = *(s16*)(prim+22);
        u8 r1 = prim[8],  g1 = prim[9],  b1 = prim[10];
        u8 r2 = prim[16], g2 = prim[17], b2 = prim[18];
        int blend = (code & 2) ? 1 : 0;
        if (mdfn_gpu_enabled()) {
            MdfnTri t = {0};
            t.gouraud = 1;
            t.verts[0].x = x0p; t.verts[0].y = y0p;
            t.verts[0].r = r0;  t.verts[0].g = g0;  t.verts[0].b = b0;
            t.verts[1].x = x1p; t.verts[1].y = y1p;
            t.verts[1].r = r1;  t.verts[1].g = g1;  t.verts[1].b = b1;
            t.verts[2].x = x2p; t.verts[2].y = y2p;
            t.verts[2].r = r2;  t.verts[2].g = g2;  t.verts[2].b = b2;
            t.blend = mdfn_blend_for(blend, g_tpage_abr);
            t.mask_eval = g_mask_eval_and;
            mdfn_draw_triangle(&t);
        } else {
            draw_poly_g3_blend(x0p, y0p, r0, g0, b0,
                               x1p, y1p, r1, g1, b1,
                               x2p, y2p, r2, g2, b2,
                               blend, g_tpage_abr);
        }
        g_frame_poly_count++;
        break;
    }
    case 0x34: {
        gpu_draw_textured_polygon(prim, code, len);
        break;
    }
    case 0x38: { /* POLY_G4 — gouraud quad (two gouraud triangles) */
        /* Layout: r0g0b0code | x0y0 | r1g1b1_ | x1y1 | r2g2b2_ | x2y2 | r3g3b3_ | x3y3
         * Vertex order: v0=TL, v1=TR, v2=BL, v3=BR
         * Two triangles: (v0,v1,v2) and (v1,v2,v3) */
        s16 x0q = *(s16*)(prim+4),  y0q = *(s16*)(prim+6);
        s16 x1q = *(s16*)(prim+12), y1q = *(s16*)(prim+14);
        s16 x2q = *(s16*)(prim+20), y2q = *(s16*)(prim+22);
        s16 x3q = *(s16*)(prim+28), y3q = *(s16*)(prim+30);
        u8 r1q = prim[8],  g1q = prim[9],  b1q = prim[10];
        u8 r2q = prim[16], g2q = prim[17], b2q = prim[18];
        u8 r3q = prim[24], g3q = prim[25], b3q = prim[26];
        int blend = (code & 2) ? 1 : 0;

        if (mdfn_gpu_enabled()) {
            MdfnTri t = {0};
            t.gouraud = 1;
            t.blend = mdfn_blend_for(blend, g_tpage_abr);
            t.mask_eval = g_mask_eval_and;
            t.verts[0].x = x0q; t.verts[0].y = y0q;
            t.verts[0].r = r0;  t.verts[0].g = g0;  t.verts[0].b = b0;
            t.verts[1].x = x1q; t.verts[1].y = y1q;
            t.verts[1].r = r1q; t.verts[1].g = g1q; t.verts[1].b = b1q;
            t.verts[2].x = x2q; t.verts[2].y = y2q;
            t.verts[2].r = r2q; t.verts[2].g = g2q; t.verts[2].b = b2q;
            mdfn_draw_triangle(&t);
            t.verts[0].x = x1q; t.verts[0].y = y1q;
            t.verts[0].r = r1q; t.verts[0].g = g1q; t.verts[0].b = b1q;
            t.verts[1].x = x2q; t.verts[1].y = y2q;
            t.verts[1].r = r2q; t.verts[1].g = g2q; t.verts[1].b = b2q;
            t.verts[2].x = x3q; t.verts[2].y = y3q;
            t.verts[2].r = r3q; t.verts[2].g = g3q; t.verts[2].b = b3q;
            mdfn_draw_triangle(&t);
        } else {
            draw_poly_g3_blend(x0q, y0q, r0,  g0,  b0,
                               x1q, y1q, r1q, g1q, b1q,
                               x2q, y2q, r2q, g2q, b2q,
                               blend, g_tpage_abr);
            draw_poly_g3_blend(x1q, y1q, r1q, g1q, b1q,
                               x2q, y2q, r2q, g2q, b2q,
                               x3q, y3q, r3q, g3q, b3q,
                               blend, g_tpage_abr);
        }
        g_frame_poly_count++;
        break;
    }
    case 0x3C: {
        gpu_draw_textured_polygon(prim, code, len);
        break;
    }
    case 0x50: { /* LINE_G2 — gouraud 2-point line (GP0 0x50/0x52) */
        s16 x0l = *(s16 *)(prim + 4);
        s16 y0l = *(s16 *)(prim + 6);
        u8  r1l = prim[8];
        u8  g1l = prim[9];
        u8  b1l = prim[10];
        s16 x1l = *(s16 *)(prim + 12);
        s16 y1l = *(s16 *)(prim + 14);
        int blend = (code & 2) ? 1 : 0;
        if (mdfn_gpu_enabled()) {
            MdfnLine l = {0};
            l.verts[0].x = x0l; l.verts[0].y = y0l;
            l.verts[0].r = r0;  l.verts[0].g = g0;  l.verts[0].b = b0;
            l.verts[1].x = x1l; l.verts[1].y = y1l;
            l.verts[1].r = r1l; l.verts[1].g = g1l; l.verts[1].b = b1l;
            l.gouraud = 1;
            l.blend = mdfn_blend_for(blend, g_tpage_abr);
            l.mask_eval = g_mask_eval_and;
            mdfn_draw_line(&l);
        } else {
            draw_line_g2_blend(x0l, y0l, r0,  g0,  b0,
                               x1l, y1l, r1l, g1l, b1l,
                               blend, g_tpage_abr);
        }
        g_frame_poly_count++;
        break;
    }
    default:
        /* A GP0 render command this HAL does not draw yet: its words
         * are still consumed by gp0_command_words. */
        if (gp0_trace_enabled()) g_gp0_unhandled[code]++;
        break;
    }
    g_pgxp_prim.n = 0;
}

/* Number of words the GP0 command at words[0] consumes, or 0 when fewer
 * than that remain in the packet (avail >= 1).  Sizes are the GP0 command
 * formats (PSX-SPX "GPU Render Polygon/Line/Rectangle Commands", "VRAM
 * blitting", "GPU Rendering Attributes"): a polygon is its command word
 * plus per-vertex XY, UV (textured) and color (Gouraud, after vertex 0)
 * words; a rectangle adds XY, UV/CLUT when textured and WH when its size
 * field is variable; a polyline runs to its 5xxx5xxxh terminator, checked
 * where each new vertex starts; A0h carries (w*h+1)/2 data words.  Every
 * other command, including NOPs and E0h-FFh, is a single word.
 * Nothing here makes a command span two DMA packets: libgs never splits
 * one, and the HAL drops an incomplete command (not hardware-verified). */
static unsigned int gp0_command_words(const u32 *words, unsigned int avail)
{
    u32 op = words[0] >> 24;
    unsigned int count, vertices, step;
    u32 size, width, height;
    switch (op >> 5) {
    case 0:
        count = op == 0x02 ? 3u : 1u;
        break;
    case 1: /* polygons */
        vertices = (op & 8) ? 4u : 3u;
        if (op & 0x10)
            count = vertices * ((op & 4) ? 3u : 2u);
        else
            count = 1u + vertices * ((op & 4) ? 2u : 1u);
        break;
    case 2: /* lines */
        if (!(op & 8)) {
            count = (op & 0x10) ? 4u : 3u;
            break;
        }
        step = (op & 0x10) ? 2u : 1u;
        for (count = step + 2u; count < avail; count += step)
            if ((words[count] & 0xF000F000u) == 0x50005000u)
                return count + 1u;
        return 0;
    case 3: /* rectangles */
        count = 2u + ((op & 4) ? 1u : 0) + (((op >> 3) & 3u) ? 0 : 1u);
        break;
    case 4: /* VRAM to VRAM copy */
        count = 4u;
        break;
    case 5: /* CPU to VRAM upload */
        if (avail < 3u) return 0;
        size = words[2];
        width = ((size - 1u) & 1023u) + 1u;
        height = (((size >> 16) - 1u) & 511u) + 1u;
        count = 3u + (width * height + 1u) / 2u;
        break;
    case 6: /* VRAM to CPU: the data is read back through GPUREAD */
        count = 3u;
        break;
    default: /* E0h-FFh environment and NOP words */
        count = 1u;
        break;
    }
    return count <= avail ? count : 0;
}

/* Execute one complete GP0 command of `count` words. */
static FILE *g_gp0_frame_dump;   /* GP0_TRACE_FRAME: one DrawOTag call */
/* ---- GPU drawing time ----
 * The time the GPU takes to draw a render command, in GPU work ticks (two
 * per CPU tick, gpu_queue_work).  Sources:
 *  - psx-spx (https://psx-spx.consoledev.net/, GPU timing): the polygon
 *    setup cost by shape, 46/226/334/496 for flat/textured/shaded/both
 *    triangles and 82/262/370/532 for quads;
 *  - DuckStation (c) Connor McLaughlin, GPL-3.0, commit aa955b8ae
 *    (src/core/gpu.h AddDrawTriangleTicks / AddDrawRectangleTicks /
 *    AddDrawLineTicks, src/core/gpu_commands.cpp, src/core/gpu_sw.cpp):
 *    the 16-tick rectangle and poly-line setup, culling of primitives over
 *    1024x512 or outside the drawing area, and the fill rules.  A triangle
 *    costs the area of its drawing-area-clamped vertices, doubled when
 *    textured, +50% when semi-transparent or mask-tested.  A rectangle
 *    costs, per drawn row, its drawn width, plus for a textured one the
 *    texture fetch: one more width at 4 bpp, and at 8 / 15 bpp eight ticks
 *    per 4 / 2 pixels once the row is 32 or more pixels wide (the cache
 *    refills per row), one more width below that; +50% of the width when
 *    semi-transparent or mask-tested.  A line costs its longer drawn side.
 *  - Our own: only the first segment of a poly-line is costed, and the
 *    interlaced-field halving is not modelled.
 * Until 2026-10-31 only clears took GPU time, so a LoadImage queued
 * behind a frame's drawing never waited: retail's name-entry kana frame
 * waits ~200,000 CPU ticks in DrawSync for the previous frame's OT. */
typedef struct { s32 x0, y0, x1, y1; } PrimRect;   /* x1/y1 exclusive */

static PrimRect prim_draw_area(void)
{
    PrimRect r;
    s32 l = (s32)(g_area_tl & 0x3ffu), t = (s32)((g_area_tl >> 10) & 0x1ffu);
    s32 rr = (s32)(g_area_br & 0x3ffu), b = (s32)((g_area_br >> 10) & 0x1ffu);
    r.x1 = rr + 1 < 1024 ? rr + 1 : 1024;
    r.y1 = b + 1 < 512 ? b + 1 : 512;
    r.x0 = l < rr ? l : rr;
    r.y0 = t < b ? t : b;
    return r;
}

static s32 prim_clamp(s32 v, s32 lo, s32 hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* The part of [x0,x1) x [y0,y1) inside the drawing area; 0 if empty. */
static int prim_clip(PrimRect *p)
{
    PrimRect a = prim_draw_area();
    if (p->x0 < a.x0) p->x0 = a.x0;
    if (p->y0 < a.y0) p->y0 = a.y0;
    if (p->x1 > a.x1) p->x1 = a.x1;
    if (p->y1 > a.y1) p->y1 = a.y1;
    return p->x1 > p->x0 && p->y1 > p->y0;
}

/* One triangle of a polygon; v holds x,y pairs. */
static u32 prim_tri_cost(const s32 *v, int i0, int i1, int i2, int textured, int semi)
{
    const int corner[3] = { i0, i1, i2 };
    PrimRect bound, a;
    s32 px[3], py[3];
    s64 twice;
    u32 cost;
    int k;
    for (k = 0; k < 3; k++) {
        px[k] = v[corner[k] * 2];
        py[k] = v[corner[k] * 2 + 1];
    }
    bound.x0 = bound.x1 = px[0];
    bound.y0 = bound.y1 = py[0];
    for (k = 1; k < 3; k++) {
        if (px[k] < bound.x0) bound.x0 = px[k];
        if (px[k] > bound.x1) bound.x1 = px[k];
        if (py[k] < bound.y0) bound.y0 = py[k];
        if (py[k] > bound.y1) bound.y1 = py[k];
    }
    bound.x1++;
    bound.y1++;
    if (bound.x1 - bound.x0 > 1024 || bound.y1 - bound.y0 > 512)
        return 0;                               /* too large: culled */
    if (!prim_clip(&bound))
        return 0;                               /* off the drawing area */
    a = prim_draw_area();
    for (k = 0; k < 3; k++) {
        px[k] = prim_clamp(px[k], a.x0, a.x1);
        py[k] = prim_clamp(py[k], a.y0, a.y1);
    }
    twice = (s64)(px[1] - px[0]) * (py[2] - py[0]) -
            (s64)(px[2] - px[0]) * (py[1] - py[0]);
    if (twice < 0) twice = -twice;
    cost = (u32)(twice / 2);
    if (textured) cost *= 2u;
    if (semi || g_mask_eval_and) cost += (cost + 1u) / 2u;
    return cost;
}

/* Texture fetch cost of one textured rectangle row `width` pixels wide. */
static u32 prim_rect_fetch(u32 width)
{
    switch (g_tpage_bpp) {
    case 0:  return width;                              /* 4 bpp */
    case 1:  return width >= 32u ? (width / 4u) * 8u : width;   /* 8 bpp */
    default: return width >= 32u ? (width / 2u) * 8u : width;   /* 15 bpp */
    }
}

static u32 gpu_prim_ticks(const u32 *raw, unsigned int count)
{
    static const u16 poly_setup[2][2][2] = {   /* [quad][shaded][textured] */
        { { 46, 226 }, { 334, 496 } },
        { { 82, 262 }, { 370, 532 } }
    };
    u32 w[12];
    u32 op = raw[0] >> 24;
    int semi = (op & 2u) != 0, textured = (op & 4u) != 0;
    unsigned int n = count < 12u ? count : 12u;

    memcpy(w, raw, n * 4u);
    gpu_decode_vertices(w, n);

    if (op < 0x40u) {                           /* polygon */
        int quad = (op & 8u) != 0, shaded = (op & 0x10u) != 0;
        unsigned int verts = quad ? 4u : 3u;
        unsigned int per_vert = 1u + (unsigned int)textured + (unsigned int)shaded;
        unsigned int k, word = 1u;
        u32 cost = poly_setup[quad][shaded][textured];
        s32 v[8];
        if (count < verts * per_vert + (shaded ? 0u : 1u))
            return cost;
        for (k = 0; k < verts; k++) {
            if (shaded && k > 0) word++;        /* skip the vertex colour */
            v[k * 2] = (s16)(w[word] & 0xffffu) + g_draw_x;
            v[k * 2 + 1] = (s16)(w[word] >> 16) + g_draw_y;
            word += textured ? 2u : 1u;
        }
        cost += prim_tri_cost(v, 0, 1, 2, textured, semi);
        if (quad) cost += prim_tri_cost(v, 2, 1, 3, textured, semi);
        return cost;
    }

    if (op < 0x60u) {                           /* line */
        u32 setup = (op & 8u) ? 16u : 0u;       /* poly-line setup only */
        unsigned int end = (op & 0x10u) ? 3u : 2u;
        PrimRect r;
        s32 ax, ay, bx, by, dx, dy;
        if (count <= end) return setup;
        ax = (s16)(w[1] & 0xffffu) + g_draw_x;
        ay = (s16)(w[1] >> 16) + g_draw_y;
        bx = (s16)(w[end] & 0xffffu) + g_draw_x;
        by = (s16)(w[end] >> 16) + g_draw_y;
        r.x0 = ax < bx ? ax : bx;
        r.x1 = (ax < bx ? bx : ax) + 1;
        r.y0 = ay < by ? ay : by;
        r.y1 = (ay < by ? by : ay) + 1;
        if (r.x1 - r.x0 > 1024 || r.y1 - r.y0 > 512 || !prim_clip(&r))
            return setup;
        dx = r.x1 - r.x0;
        dy = r.y1 - r.y0;
        return setup + (u32)(dx > dy ? dx : dy);
    }

    {                                           /* rectangle */
        static const s32 fixed_size[4] = { 0, 1, 8, 16 };
        unsigned int size_code = (op >> 3) & 3u;
        unsigned int size_word = textured ? 3u : 2u;
        s32 width, height;
        u32 dw, dh, row;
        PrimRect r;
        if (count < 2u) return 16u;
        if (size_code != 0u) {
            width = height = fixed_size[size_code];
        } else {
            if (count <= size_word) return 16u;
            width = (s32)(raw[size_word] & 0x3ffu);
            height = (s32)((raw[size_word] >> 16) & 0x1ffu);
        }
        r.x0 = gpu_signed11((u32)((s16)(w[1] & 0xffffu) + g_draw_x));
        r.y0 = gpu_signed11((u32)((s16)(w[1] >> 16) + g_draw_y));
        r.x1 = r.x0 + width;
        r.y1 = r.y0 + height;
        if (!prim_clip(&r)) return 16u;
        dw = (u32)(r.x1 - r.x0);
        dh = (u32)(r.y1 - r.y0);
        row = dw;
        if (textured) row += prim_rect_fetch(dw);
        if (semi || g_mask_eval_and) row += (dw + 1u) / 2u;
        return 16u + row * dh;
    }
}

static void gpu_execute_command(u32 *words, unsigned int count)
{
    u32 op = words[0] >> 24;
    if (gp0_trace_enabled()) {
        g_gp0_hist[op]++;
        if (op >= 0x60 && op < 0x80 && ((op >> 3) & 3u) == 0 &&
            ((words[count - 1] & 0xFFFFu) > 1023u ||
             (words[count - 1] >> 16) > 511u))
            g_gp0_rect_oversize++;
        if (op == 0xE1 && (words[0] & 0x3000u))
            g_gp0_e1_flip++;
    }
    if (g_gp0_frame_dump) {
        unsigned int i, shown = count > 12u ? 3u : count;
        fprintf(g_gp0_frame_dump, "cmd %02X n=%u tpage=%d,%d bpp=%d abr=%d mask=%x/%x off=%d,%d:",
                (unsigned)op, count, (int)g_tpage_x, (int)g_tpage_y,
                (int)g_tpage_bpp, (int)g_tpage_abr, (unsigned)g_mask_set_or,
                (unsigned)g_mask_eval_and, (int)g_draw_x, (int)g_draw_y);
        for (i = 0; i < shown; i++)
            fprintf(g_gp0_frame_dump, " %08X", (unsigned)words[i]);
        fputc('\n', g_gp0_frame_dump);
    }
    if (gpu_draw_command(words[0]))
        return;
    if (op == 0x02) {
        /* the fill's drawing time, as gpu_execute_clear_words charges it */
        gpu_queue_work(46u + ((((words[2] & 1023u) + 15u) & ~15u) / 8u + 9u) *
                       ((words[2] >> 16) & 511u));
        gpu_vram_fill(&g_vram[0][0], words[0], words[1], words[2]);
        hires_mirror_fill(words[1], words[2]);
        return;
    }
    if (op >= 0x20 && op < 0x80) {
        gpu_queue_work(gpu_prim_ticks(words, count));
        gpu_draw_primitive((u8 *)words, (u8)count);
        return;
    }
    if (op >= 0x80 && op < 0xE0)
        gpu_vram_transfer_started();
    if (op >= 0xA0 && op < 0xC0) {
        gpu_vram_load(&g_vram[0][0], (int)(words[1] & 0xFFFFu),
                      (int)(words[1] >> 16), (int)(words[2] & 0xFFFFu),
                      (int)(words[2] >> 16), (const u16 *)(words + 3),
                      g_mask_set_or != 0, g_mask_eval_and != 0);
        if (HIRES_ON())
            gpu_hires_mirror_rect((int)(words[1] & 1023u), (int)((words[1] >> 16) & 511u),
                                  (int)(((words[2] & 0xFFFFu) - 1u) & 1023u) + 1,
                                  (int)((((words[2] >> 16) - 1u) & 511u) + 1u));
        if (g_texrep_on)
            gpu_texrep_upload((int)(words[1] & 1023u), (int)((words[1] >> 16) & 511u),
                              (int)(((words[2] & 0xFFFFu) - 1u) & 1023u) + 1,
                              (int)((((words[2] >> 16) - 1u) & 511u) + 1u));
        if ((g_extras_gpu & 1) && extras_gallery_upload)
            extras_gallery_upload((int)(words[1] & 1023u), (int)((words[1] >> 16) & 511u),
                                  (int)(((words[2] & 0xFFFFu) - 1u) & 1023u) + 1,
                                  (int)((((words[2] >> 16) - 1u) & 511u) + 1u), NULL);
        return;
    }
    /* 02h-1Fh misc, 80h copy (overlap order unestablished), C0h
     * readback and E0h/E7h-FFh: consumed, not executed. */
    if (gp0_trace_enabled()) g_gp0_unhandled[op]++;
}

/* A DMA packet's words are one GP0 command stream: state words apply in
 * order and every primitive in the packet draws, each consuming exactly
 * its own word count (libgs prefixes E1/E2 state to SPRT/POLY records). */
static void gpu_execute_packet(u32 *words, unsigned int len)
{
    unsigned int at, count;
    for (at = 0; at < len; at += count) {
        count = gp0_command_words(words + at, len - at);
        if (!count) {
            if (gp0_trace_enabled()) g_gp0_incomplete++;
            break;
        }
        gpu_execute_command(words + at, count);
    }
}

/* DrawOTag's CPU work (same probe, 1,170 calls): set_alarm at 72,
 * SetIntrMask(0) at 184, the masked _exeque dispatch and DMA kick 1,167
 * (1,167..1,176), the return 53: 1,404 in all, whatever the OT holds. */
#define DRAWOTAG_TO_ALARM   72u
#define DRAWOTAG_TO_MASK    112u
#define DRAWOTAG_MASKED     1167u
#define DRAWOTAG_TO_RETURN  53u

/* The next node of an ordering-table walk, or NULL at the terminator. */
static u32 *ot_next(u32 *p)
{
    void *next = pc_ot_getaddr(p);
    unsigned long next_val = (unsigned long)next;
    if (next_val == 0xFFFFFF || next_val == 0 || next == NULL) return NULL;
    return (u32 *)next;
}

/*
 * A cyclic ordering table: the node where the list re-enters itself, or
 * NULL for a list that ends.  (Floyd: a slow and a fast walker meet inside
 * the cycle; from there and from the head, the walkers meet at its entry.)
 *
 * The game builds one: GsSortClear AddPrims its static block-fill packet
 * (0x800EBD40 + PSDIDX * 16) into ot_work[activeBuff].  When TACO re-runs
 * graph_init in the middle of a frame (sub-step 3 -> 10), activeBuff turns
 * from 1 to 0 after ot_buff_set cleared OT 1, so vblank_function adds
 * packet 1 to OT 0 a second time without OT 0 having been cleared since the
 * frame before: the packet links to itself.  The console's GPU then walks
 * the fill packet over and over until the next frame's ResetGraph(1) aborts
 * the DMA; the reference shows that frame and plays on.  The port's
 * DrawOTag draws synchronously, so it draws every node of the cycle once
 * and stops where the GPU would only repeat itself (the fill is opaque, the
 * repeated draws change nothing), instead of 100,000 full-screen fills past
 * the 5 s frame watchdog (ev_final_taco98, 2026-11-01).
 */
static u32 *ot_cycle_entry(u32 *head)
{
    u32 *slow = head, *fast = head;
    for (;;) {
        slow = ot_next(slow);
        fast = ot_next(fast);
        if (fast) fast = ot_next(fast);
        if (!slow || !fast) return NULL;
        if (slow == fast) break;
    }
    slow = head;
    while (slow != fast) {
        slow = ot_next(slow);
        fast = ot_next(fast);
    }
    return slow;
}

unsigned long g_ot_cycles;   /* DrawOTag calls that met a cyclic OT */

void DrawOTag(u32 *ot) {
    /* PC port DrawOTag: traverse OT linked list using side table for pointers.
     * P_TAG layout (32-bit struct): addr:24 + len:8 = first u32, then r0/g0/b0/code.
     * The side table maps each P_TAG's address to the full 64-bit next pointer. */
    u32 *p;
    s32 prim_count = 0;
    u32 ticket;
    static int drawot_trace = 0;
    static int frame_trace = 0;
    static int call_count = 0;

    call_count++;
    if (call_count <= 3) {
        printf("[GPU] DrawOTag CALLED: ot=%p call#=%d\n", (void*)ot, call_count);
    }

    if (!ot) return;
    gpu_cpu_submit_entry(DRAWOTAG_TO_ALARM, DRAWOTAG_TO_MASK);
    ticket = gpu_queue_begin();
    frame_trace++;
    if (frame_trace == 30) {
        /* Reset trace counters at frame 30 to capture primitives for debug screenshot */
        drawot_trace = 0;
        printf("[GPU] DrawOTag: resetting trace for frame 30 screenshot\n");
    }
    if (drawot_trace < 5) {
        GPU_P_TAG *tag = (GPU_P_TAG *)ot;
        printf("[GPU] DrawOTag ot=%p addr=0x%06X len=%d\n",
               (void*)ot, tag->addr, tag->len);
        drawot_trace++;
    }

    if (gp0_trace_enabled()) {
        static long dump_call = -2;
        static long calls = 0;
        if (dump_call == -2) {
            const char *e = getenv("GP0_TRACE_FRAME");
            dump_call = e && *e ? atol(e) : -1;
        }
        if (++calls == dump_call)
            g_gp0_frame_dump = fopen("gp0_frame.log", "w");
    }
    p = ot;
    static int iter_count_trace = 0;
    int iter_count = 0;
    u32 *cycle = ot_cycle_entry(ot);
    int cycle_seen = 0;
    if (cycle) {
        g_ot_cycles++;
        if (g_ot_cycles <= 3)
            printf("[GPU] DrawOTag: cyclic ordering table, re-entered at %p "
                   "(drawn once)\n", (void *)cycle);
    }
    while (1) {
        if (p == cycle && cycle_seen++) break;
        GPU_P_TAG *tag = (GPU_P_TAG *)p;
        u8 len = tag->len;
        u8 r0 = tag->r0, g0 = tag->g0, b0 = tag->b0;
        u8 code = tag->code;
        iter_count++;

        if (len > 0) {
            if (drawot_trace < 20) {
                printf("[GPU] PRIM at %p: len=%d code=0x%02X rgb=(%d,%d,%d)\n",
                       (void*)p, len, code, r0, g0, b0);
                drawot_trace++;
            }
            /* The words after the DMA tag are one GP0 stream. */
            gpu_execute_packet((u32 *)p + 1, len);
            prim_count++;
        }

        /* Get next pointer from side table */
        p = ot_next(p);
        if (!p) break;

        if (prim_count > 100000) {
            printf("[GPU] DrawOTag: too many primitives, aborting\n");
            break;
        }
    }
    if (g_gp0_frame_dump) {
        fclose(g_gp0_frame_dump);
        g_gp0_frame_dump = NULL;
    }
    g_frame_prim_count += prim_count;
    if (iter_count_trace < 5) {
        printf("[GPU] DrawOTag: %d iterations, %d primitives rendered\n", iter_count, prim_count);
        iter_count_trace++;
    }
    if (prim_count > 0 && drawot_trace < 10) {
        printf("[GPU] DrawOTag rendered %d primitives\n", prim_count);
        drawot_trace++;
    }
    gpu_queue_end(ticket);
    /* the vertices projected for this list are spent (gpu_pgxp.h rule 1) */
    if (PGXP_ON()) gpu_pgxp_list_done();
    /* the cycle's DMA never ends on the console (gpu_queue.c gpu_queue_hang) */
    if (cycle) gpu_queue_hang();
    gpu_cpu_masked(DRAWOTAG_MASKED, DRAWOTAG_TO_RETURN);
}

/* Additional GPU functions used by the game */
/* GetTPage, GetClut and SetDrawMode are pure CPU work in libgpu; each
 * charges retail's measured time (pc/hal/cpu_cost.c hal_cpu_cost_named,
 * which includes the GetGraphType / get_mode / get_tw calls retail makes
 * from them). */
u32 GetTPage(s32 tp, s32 abr, s32 x, s32 y) {
    HAL_CPU_COST("GetTPage");
    return ((tp & 3) << 7) | ((abr & 3) << 5) | ((x / 64) & 0xF) | (((y / 256) & 1) << 4);
}

u16 GetClut(s32 x, s32 y) {
    HAL_CPU_COST("GetClut");
    return (u16)(((y & 0x1FF) << 6) | ((x >> 4) & 0x3F));
}

void SetDrawMode(void *p, s32 dfe, s32 dtd, s32 tpage, RECT *tw) {
    u32 *code;
    u8 *tag = (u8 *)p;
    /* The fifth argument: most callers (the matched dtd_on / goto_tpage,
     * TT, TACO, ETC) declare it `s32` and pass 0 or a PS1 address.  System
     * V passes it in %r8, where the 32-bit write zero-extends; the
     * Microsoft x64 convention passes it in an 8-byte stack slot the caller
     * fills with 4 bytes, so the upper half is stale (the first Windows
     * new_game faulted on tw = 0x00000b7300000002).  Every address the port
     * hands out on Windows is below 4 GB (pc/src/main.c checks the stack,
     * pc/hal/win/win_heap.c the heap), so the low word is the pointer
     * (pc/tools/audit_ps1_call_args.py --width, WIN64-STACK).  Not on
     * Linux: the register is already right, and a HAL caller or a test
     * harness may pass a host stack address above 4 GB. */
#ifdef _WIN32
    tw = (RECT *)(uintptr_t)(u32)(uintptr_t)tw;
#endif
    HAL_CPU_COST("SetDrawMode");
    if (gp0_trace_enabled()) g_gp0_setdrawmode++;
    /* Preserve the DMA link; two GP0 words follow the one-word tag. */
    tag[3] = 2; /* len = 2 words of data */

    /* The DMA tag is one word; GP0 commands begin immediately after it. */
    code = (u32 *)((u8 *)p + 4);
    /* GP0(E1): draw mode */
    code[0] = 0xE1000000u | (dfe ? 0x400 : 0) | (dtd ? 0x200 : 0) | ((u32)tpage & 0x9FF);
    /* GP0(E2): texture window */
    if (tw) {
        u32 twx = ((u32)tw->x & 0xFF) >> 3;
        u32 twy = ((u32)tw->y & 0xFF) >> 3;
        u32 tww = ((u32)(-(s32)tw->w) & 0xFF) >> 3;
        u32 twh = ((u32)(-(s32)tw->h) & 0xFF) >> 3;
        code[1] = 0xE2000000u | (twy << 15) | (twx << 10) | (twh << 5) | tww;
    } else {
        code[1] = 0;
    }
}
