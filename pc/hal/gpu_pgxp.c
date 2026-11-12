/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gpu_pgxp.c -- the sub-pixel vertex table of the high-resolution shadow.
 * Design and the three soundness rules: gpu_pgxp.h.  The GTE side is one
 * call in pc/hal/gte.c gte_rtp; the GPU side (the per-polygon lookup and
 * the shadow vertices) lives in pc/hal/gpu.c beside the shadow pass.
 */
#include "hal.h"
#include "gpu_hires.h"
#include "gpu_pgxp.h"
#include "profile.h"
#include "pc_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* settings.c is linked into the game; harnesses go without it */
extern const PcSettings *pc_settings_get(void) __attribute__((weak));

int g_gpu_pgxp_active;
GpuPgxpStats g_gpu_pgxp_stats;

typedef struct {
    unsigned int key;       /* the SXY word */
    unsigned int gen;       /* live only when == g_gen */
    unsigned short fx, fy;  /* fractions, 1/65536 of a native pixel */
    unsigned char ambiguous;
} PgxpSlot;

/* A primitive's vertices as one GTE command left them in the SXY FIFO
 * (RTPT, or RTPS after RTPT: a quad's second triangle). */
typedef struct {
    unsigned int w[3];
    unsigned int gen;
    unsigned short fx[3], fy[3];
    unsigned char ambiguous;
} PgxpTriSlot;

static PgxpSlot g_table[GPU_PGXP_TABLE_SIZE];
static PgxpTriSlot g_tri[GPU_PGXP_TABLE_SIZE];
static unsigned int g_gen = 1;       /* slot.gen 0 = never written */
static unsigned int g_fill;          /* live slots in this generation */
static unsigned int g_tri_fill;
static int g_wanted;                 /* the setting (menu toggle) */
static int g_stats_on = -1;

static unsigned int slot_of(unsigned int key)
{
    key *= 0x9E3779B1u;
    return key >> (32 - GPU_PGXP_TABLE_BITS);
}

static unsigned int tri_slot_of(const unsigned int *w)
{
    return slot_of(w[0] ^ (w[1] * 0x85EBCA6Bu) ^ (w[2] * 0xC2B2AE35u));
}

static void new_generation(void)
{
    g_gen++;
    if (!g_gen) {               /* wrapped: forget every slot */
        memset(g_table, 0, sizeof g_table);
        memset(g_tri, 0, sizeof g_tri);
        g_gen = 1;
    }
    g_fill = 0;
    g_tri_fill = 0;
}

static void update_active(void)
{
    g_gpu_pgxp_active = g_wanted && g_hires.scale > 1 &&
                        hal_feature_on("GPU_PGXP");
}

static void stats_at_exit(void)
{
    gpu_pgxp_print_stats("exit");
}

void gpu_pgxp_init(void)
{
    long on = 1;
    if (pc_settings_get && pc_settings_get()) on = pc_settings_get()->subpixel;
    on = hal_setting_long("TOKI_SUBPIXEL", on);
    g_wanted = on != 0;
    update_active();
    if (g_stats_on < 0) {
        const char *e = getenv("PGXP_STATS");
        g_stats_on = e && *e && *e != '0';
        if (g_stats_on) atexit(stats_at_exit);
    }
    if (g_gpu_pgxp_active)
        printf("[PGXP] sub-pixel vertices in the %dx shadow\n", g_hires.scale);
}

void gpu_pgxp_set_enabled(int on)
{
    g_wanted = on != 0;
    update_active();
    /* nothing recorded while off may meet a list drawn after it */
    new_generation();
}

void gpu_pgxp_record(unsigned int sxy, unsigned int fx, unsigned int fy)
{
    unsigned int i = slot_of(sxy), n;
    for (n = 0; n < GPU_PGXP_TABLE_SIZE; n++, i = (i + 1) & (GPU_PGXP_TABLE_SIZE - 1)) {
        PgxpSlot *s = &g_table[i];
        if (s->gen != g_gen) {
            if (g_fill >= GPU_PGXP_TABLE_FILL) {
                g_gpu_pgxp_stats.overflow++;
                return;
            }
            s->key = sxy;
            s->gen = g_gen;
            s->fx = (unsigned short)fx;
            s->fy = (unsigned short)fy;
            s->ambiguous = 0;
            g_fill++;
            g_gpu_pgxp_stats.recorded++;
            return;
        }
        if (s->key == sxy) {
            g_gpu_pgxp_stats.recorded++;
            if (!s->ambiguous && (s->fx != (unsigned short)fx ||
                                  s->fy != (unsigned short)fy)) {
                s->ambiguous = 1;
                g_gpu_pgxp_stats.ambiguous++;
            }
            return;
        }
    }
}

int gpu_pgxp_lookup(unsigned int word, unsigned int *fx, unsigned int *fy)
{
    unsigned int i = slot_of(word), n;
    for (n = 0; n < GPU_PGXP_TABLE_SIZE; n++, i = (i + 1) & (GPU_PGXP_TABLE_SIZE - 1)) {
        const PgxpSlot *s = &g_table[i];
        /* inserts of this generation claim the first non-live slot, so a
         * live key is never behind one */
        if (s->gen != g_gen) return 0;
        if (s->key == word) {
            if (s->ambiguous) return -1;
            *fx = s->fx;
            *fy = s->fy;
            return 1;
        }
    }
    return 0;
}

void gpu_pgxp_record_tri(const unsigned int *w, const unsigned int *fx,
                         const unsigned int *fy)
{
    unsigned int i = tri_slot_of(w), n, k;
    for (n = 0; n < GPU_PGXP_TABLE_SIZE; n++, i = (i + 1) & (GPU_PGXP_TABLE_SIZE - 1)) {
        PgxpTriSlot *s = &g_tri[i];
        if (s->gen != g_gen) {
            if (g_tri_fill >= GPU_PGXP_TABLE_FILL) {
                g_gpu_pgxp_stats.overflow++;
                return;
            }
            s->gen = g_gen;
            for (k = 0; k < 3; k++) {
                s->w[k] = w[k];
                s->fx[k] = (unsigned short)fx[k];
                s->fy[k] = (unsigned short)fy[k];
            }
            s->ambiguous = 0;
            g_tri_fill++;
            g_gpu_pgxp_stats.tris++;
            return;
        }
        if (s->w[0] == w[0] && s->w[1] == w[1] && s->w[2] == w[2]) {
            for (k = 0; k < 3 && !s->ambiguous; k++)
                if (s->fx[k] != (unsigned short)fx[k] || s->fy[k] != (unsigned short)fy[k]) {
                    s->ambiguous = 1;
                    g_gpu_pgxp_stats.tris_ambiguous++;
                }
            return;
        }
    }
}

int gpu_pgxp_lookup_tri(const unsigned int *w, unsigned int *fx, unsigned int *fy)
{
    unsigned int i = tri_slot_of(w), n, k;
    for (n = 0; n < GPU_PGXP_TABLE_SIZE; n++, i = (i + 1) & (GPU_PGXP_TABLE_SIZE - 1)) {
        const PgxpTriSlot *s = &g_tri[i];
        if (s->gen != g_gen) return 0;
        if (s->w[0] == w[0] && s->w[1] == w[1] && s->w[2] == w[2]) {
            if (s->ambiguous) return -1;
            for (k = 0; k < 3; k++) {
                fx[k] = s->fx[k];
                fy[k] = s->fy[k];
            }
            return 1;
        }
    }
    return 0;
}

void gpu_pgxp_list_done(void)
{
    new_generation();
    g_gpu_pgxp_stats.lists++;
    if (g_stats_on > 0 && g_gpu_pgxp_stats.lists % 600 == 0)
        gpu_pgxp_print_stats("lists");
}

void gpu_pgxp_print_stats(const char *why)
{
    const GpuPgxpStats *s = &g_gpu_pgxp_stats;
    printf("[PGXP] %s: lists=%lu recorded=%lu tris=%lu (ambiguous %lu) "
           "saturated=%lu overflow=%lu ambiguous=%lu polys=%lu projected=%lu precise=%lu (%.1f%% of projected) "
           "by-primitive=%lu verts=%lu hit=%lu ambiguous=%lu\n",
           why, s->lists, s->recorded, s->tris, s->tris_ambiguous, s->saturated, s->overflow, s->ambiguous,
           s->polys, s->polys_gte, s->polys_precise,
           s->polys_gte ? 100.0 * (double)s->polys_precise / (double)s->polys_gte : 0.0,
           s->polys_tri, s->verts, s->verts_hit, s->verts_ambig);
    if (s->err_n > 0) {
        double mi = s->err_int_sum / s->err_n, ms = s->err_sub_sum / s->err_n;
        double vi = s->err_int_sq / s->err_n - mi * mi, vs = s->err_sub_sq / s->err_n - ms * ms;
        printf("[PGXP] %s: vertex placement rms (native px, mean removed): "
               "integers %.4f, fractions %.4f over %.0f coordinates\n",
               why, vi > 0 ? sqrt(vi) : 0.0, vs > 0 ? sqrt(vs) : 0.0, s->err_n);
    }
    fflush(stdout);
}
