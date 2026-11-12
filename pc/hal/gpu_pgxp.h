/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gpu_pgxp.h -- sub-pixel vertex precision for the high-resolution shadow
 * (the enhanced profile's GPU_PGXP; docs/pc-port/ENHANCED-MODE.md,
 * "Sub-pixel vertex precision").
 *
 * The GTE's perspective step computes each screen coordinate as a 16.16
 * sum, SX = (n * IR1 + OFX) >> 16 (pc/hal/gte.c gte_rtp), and the game only
 * ever sees the integer.  A polygon drawn at N x from those integers still
 * snaps every vertex to the 1x grid, so 3D "wobbles" as it moves.  This
 * module keeps the dropped fraction:
 *
 *   - gte_rtp records, for every projected vertex that was not saturated,
 *     the SXY word it produced (the integer result, exactly as it enters
 *     the SXY FIFO) together with the 16-bit fractions of X and Y;
 *   - after every RTPT / RTPS the three FIFO words SXY0..SXY2 are also
 *     recorded as one PRIMITIVE key with their six fractions: a TMD
 *     builder stores exactly that triple into one packet (RTPT -> v0 v1 v2;
 *     the RTPS of a quad leaves v1 v2 v3);
 *   - the GPU, before drawing a polygon into the shadow, looks up its
 *     vertex words: first as primitive triples (v0 v1 v2, and v1 v2 v3 for
 *     a quad), then, for any vertex still without fractions, as single
 *     words.  Only if EVERY vertex of the polygon is found does the shadow
 *     pass place the vertices at N * SX + (frac * N >> 16); otherwise the
 *     polygon is drawn from the integers as before.
 *
 * The triple key is what makes this precise: two different vertices often
 * land on the same integer pixel in one frame (a distant asteroid: dozens
 * of vertices in a few pixels), which makes the single word ambiguous, but
 * three words of one primitive essentially never repeat with different
 * fractions.  The single-word table covers polygons the game assembles
 * itself from RotTransPers results (taco_mesh_disp.c's grid quads).
 *
 * Keying is by VALUE (the 32-bit SXY words), not by memory address: the
 * port's TMD builders store the FIFO with plain C stores
 * (src/main/pc-port/tmd_gte_builders.c `out[k] = gte_cop2_sxy(i)`) and the
 * TACO game code copies RotTransPers results halfword by halfword into its
 * packets (taco_mesh_disp.c), so a value key follows the word through every
 * copy without instrumenting the stores.  Its soundness rests on three
 * rules, each tested (pc/tools/test_gpu_pgxp.py):
 *
 *   1. generations: the table holds the vertices projected since the last
 *      DrawOTag (vblank_function sorts, then GsDrawOt's the same OT), and
 *      an entry of an older generation is never returned;
 *   2. ambiguity: two projections in one generation that give the same
 *      word (or the same triple) with different fractions mark it
 *      ambiguous, and it is never returned;
 *   3. all-or-nothing: a polygon uses fractions only when all of its
 *      vertex words hit, so a 2D polygon whose one corner happens to equal
 *      a projected vertex keeps its integer corners.
 *
 * Whatever is found, floor(precise) == the integer the native pass uses,
 * so the shadow never moves a vertex by a whole native pixel.  The native
 * VRAM is never touched by any of this: the native pass reads the same
 * integers as ever.  Off (parity, scale 1, or [graphics] subpixel = false)
 * the GTE side is one not-taken branch per vertex.
 */
#ifndef PC_GPU_PGXP_H
#define PC_GPU_PGXP_H

#define GPU_PGXP_TABLE_BITS 14            /* 16,384 slots */
#define GPU_PGXP_TABLE_SIZE (1u << GPU_PGXP_TABLE_BITS)
#define GPU_PGXP_TABLE_FILL 12288u        /* stop recording past 3/4 full */

typedef struct {
    unsigned long recorded;       /* vertices recorded */
    unsigned long saturated;      /* projections not recorded (clamped) */
    unsigned long overflow;       /* not recorded: table full */
    unsigned long ambiguous;      /* words marked ambiguous */
    unsigned long polys;          /* shadow polygons looked up */
    unsigned long polys_precise;  /* ... drawn from fractions */
    unsigned long verts;          /* vertex words looked up */
    unsigned long verts_hit;      /* ... found */
    unsigned long verts_ambig;    /* ... projected, but ambiguous */
    unsigned long polys_gte;      /* polygons with any projected vertex */
    unsigned long polys_tri;      /* ... all vertices from primitive triples */
    unsigned long tris;           /* primitive triples recorded */
    unsigned long tris_ambiguous; /* ... marked ambiguous */
    unsigned long lists;          /* generations closed (DrawOTag) */
    /* placement error of the shadow vertices drawn from fractions, in
     * native pixels (X and Y pooled): from the integers (`int`: the
     * fraction itself) and from the fractions (`sub`: the fraction's
     * remainder below the N x grid) -- the per-vertex wobble without and
     * with sub-pixel precision, measured on what the game drew */
    double err_n, err_int_sum, err_int_sq, err_sub_sum, err_sub_sq;
} GpuPgxpStats;

/* Nonzero when sub-pixel precision is in effect (GPU_PGXP on, the shadow
 * running, and [graphics] subpixel / TOKI_SUBPIXEL not 0).  Read by
 * pc/hal/gte.c through a weak reference. */
extern int g_gpu_pgxp_active;
extern GpuPgxpStats g_gpu_pgxp_stats;

/* gpu_hires_init, once the shadow exists. */
void gpu_pgxp_init(void);
/* The settings menu's live toggle (only effective with the shadow on). */
void gpu_pgxp_set_enabled(int on);
/* gte_rtp: the SXY word and the fractions (low 16 bits of the 16.16 sums). */
void gpu_pgxp_record(unsigned int sxy, unsigned int fx, unsigned int fy);
/* The fractions recorded for `word` in this generation: 1; 0 if none,
 * -1 if ambiguous (never used). */
int gpu_pgxp_lookup(unsigned int word, unsigned int *fx, unsigned int *fy);
/* After RTPT / RTPS: the FIFO's three words and their fractions. */
void gpu_pgxp_record_tri(const unsigned int *w, const unsigned int *fx,
                         const unsigned int *fy);
/* The fractions of the primitive triple `w` in this generation: 1; 0 if
 * none, -1 if ambiguous. */
int gpu_pgxp_lookup_tri(const unsigned int *w, unsigned int *fx, unsigned int *fy);
/* DrawOTag finished: the next projections start a new generation. */
void gpu_pgxp_list_done(void);
/* PGXP_STATS=1: one summary line to stdout (every 600 lists, and at exit). */
void gpu_pgxp_print_stats(const char *why);

#endif
