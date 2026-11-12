/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gte.c — PS1 Geometry Transform Engine emulation for PC port
 *
 * The GTE is a coprocessor that performs fixed-point 3D math:
 * - Matrix multiplication (rotation, scaling)
 * - Vector transformation
 * - Perspective projection
 * - Lighting calculations
 * - Backface culling (normal clip)
 *
 * Fixed-point format: 4.12 (ONE = 4096 represents 1.0)
 * Screen coordinates are 16-bit signed integers.
 */
#include "hal.h"
#include "cpu_cost.h"
#include "profile.h"
#include "hal_gte_cop2.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* ONE = 4096 = 1.0 in GTE fixed-point */
#define GTE_ONE 4096
#define GTE_SHIFT 12

/* GTE register state */
static struct {
    /* Control registers (set by SetRotMatrix, SetTransMatrix, etc.) */
    MATRIX rot;           /* Current rotation matrix (3x3) */
    VECTOR trans;         /* Translation vector */
    MATRIX light;         /* Light source matrix */
    MATRIX color;         /* Color matrix */
    CVECTOR rgbc;         /* RGB color + code */
    s32 ofx, ofy;         /* Screen offset (16.16 fixed point) */
    s32 h;                /* Projection plane distance */
    s32 dqa, dqb;         /* Depth cueing parameters */
    s32 zsf3, zsf4;       /* Z scale factors */

    /* Data registers (results from operations) */
    s32 mac0;             /* Sum register */
    s32 mac1, mac2, mac3; /* Vector accumulators */
    s32 ir0, ir1, ir2, ir3; /* Intermediate results (clamped) */
    s32 sz0, sz1, sz2, sz3; /* Screen Z FIFO */
    s32 sxy0, sxy1, sxy2, sxy3; /* SXY FIFO: sxy1..sxy3 = SXY0..SXY2 */
    SVECTOR v[3];         /* V0..V2 operand vectors */
    CVECTOR rgb0, rgb1, rgb2; /* Color FIFO */
    s32 otz;              /* Ordered table Z */
    s32 p;                /* Interpolation factor */
    u32 flag;             /* Error flags */

    /* Matrix stack for Push/Pop */
    MATRIX rot_stack[32];
    int stack_ptr;
} gte;

/* Clamp value to signed 16-bit range */
static s32 clamp16(s32 v) {
    if (v < -32768) return -32768;
    if (v > 32767) return 32767;
    return v;
}

/* Clamp to unsigned 8-bit */
static s32 clamp8u(s32 v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

/* Initialize GTE */
/* Retail InitGeom (asm/main/matched/InitGeom.s) writes exactly these
 * control registers: ZSF3 = 341, ZSF4 = 256, H = 1000, DQA = -4194,
 * DQB = 0x01400000, OFX = OFY = 0.  Rotation, translation and the colour
 * registers keep their values.  The old HAL used ZSF3 = 1365 and ZSF4 =
 * 1024 (four times retail), so every AVSZ OTZ -- the OT bucket of each TMD
 * primitive -- was four times too deep. */
extern void hal_cpu_charge(unsigned ticks) __attribute__((weak));
void InitGeom(void) {
    /* Retail's InitGeom first runs exception_handler_install: a critical
     * section around the BIOS FlushCache (47,725 ticks) -- 48,074 ticks of
     * CPU from InitGeom's entry to its caller's next call, exceptions
     * excluded (the reference emulator, the boot's two calls, equal).  The
     * port charges that measured time (pc/tools/cpu_costs.tsv, InitGeom
     * with its sub-calls folded in: measure_cpu_costs.py ABSORB).
     * BIOS_CPU_COST=0 is the negative control. */
    /* That row excludes exception time, and the critical section is two
     * SYSCALL exceptions: Enter 720 / 603 ticks and Exit 697 / 697 at the
     * boot's two calls (same probe; the BIOS thunks' class medians are
     * 688 / 514, bios_trampolines.c).  INITGEOM_SYSCALLS=0 is the negative
     * control (the row alone, 1,359 ticks short a call). */
    {
        int bios = hal_model_on("BIOS_CPU_COST");   /* =0: negative control */
        const char *s = getenv("INITGEOM_SYSCALLS");
        int sys = bios && !(s && s[0] == '0') && hal_cpu_charge;
        if (sys) hal_cpu_charge(662u);
        if (bios) HAL_CPU_COST("InitGeom");
        if (sys) hal_cpu_charge(697u);
    }
    gte.zsf3 = 341;
    gte.zsf4 = 256;
    gte.h = 1000;
    gte.dqa = -4194;
    gte.dqb = 0x01400000;
    gte.ofx = 0;
    gte.ofy = 0;
}

/* ---- port diagnostics -------------------------------------------------
 * The 3D path's first failure mode was "every vertex projects with SZ = 0":
 * the GTE rotation/translation registers were never loaded because the
 * matched libgs wrappers pass their matrix in $a0 without a C-level argument.
 * These counters and the state snapshot make that visible instead of guessed.
 * Read-only; no renderer behaviour depends on them. */
static unsigned long g_rot_sets, g_trans_sets, g_screen_sets;

void gte_trace_set(s32 which)
{
    if (which == 0) g_rot_sets++;
    else if (which == 1) g_trans_sets++;
    else g_screen_sets++;
}

void gte_trace_state(s32 *out)
{
    s32 i;
    out[0] = gte.h;
    out[1] = gte.ofx >> 16;
    out[2] = gte.ofy >> 16;
    for (i = 0; i < 9; i++) out[3 + i] = gte.rot.m[i / 3][i % 3];
    out[12] = gte.trans.vx;
    out[13] = gte.trans.vy;
    out[14] = gte.trans.vz;
    out[15] = (s32)g_rot_sets;
    out[16] = (s32)g_trans_sets;
    out[17] = (s32)g_screen_sets;
}

/* Retail time of the libgte calls game code makes (pc/hal/cpu_cost.c
 * HAL_SDK_COST); a HAL GTE function calling another (MulMatrix ->
 * MulMatrix0 -> SetRotMatrix) is one retail call, charged once. */
static int g_gte_nest;
#define GTE_COST(name) do { if (!g_gte_nest) HAL_SDK_COST(name); } while (0)
#define GTE_INNER(call) (g_gte_nest++, gte_inner_ret_ = (void *)(call), g_gte_nest--, gte_inner_ret_)
static void *gte_inner_ret_;

/* Set screen center offset */
void SetGeomOffset(s32 ofx, s32 ofy) {
    /* Match the MIPS SLL bit pattern, including negative/wrapped inputs,
     * without undefined signed left shifts. */
    gte.ofx = (s32)((u32)ofx << 16);
    gte.ofy = (s32)((u32)ofy << 16);
}

/* Set projection plane distance (affects perspective) */
void SetGeomScreen(s32 h) {
    GTE_COST("SetGeomScreen");
    gte.h = h;
    gte_trace_set(2);
}

/* Set rotation matrix */
void SetRotMatrix(MATRIX *m) {
    GTE_COST("SetRotMatrix");
    gte_trace_set(0);
    if (!m) return;
    memcpy(&gte.rot.m, m->m, sizeof(gte.rot.m));
}

/* Set translation vector */
void SetTransMatrix(MATRIX *m) {
    GTE_COST("SetTransMatrix");
    gte_trace_set(1);
    if (!m) return;
    gte.trans.vx = m->t[0];
    gte.trans.vy = m->t[1];
    gte.trans.vz = m->t[2];
}

/* Set light source matrix */
void SetLightMatrix(MATRIX *m) {
    GTE_COST("SetLightMatrix");
    if (!m) return;
    memcpy(&gte.light.m, m->m, sizeof(gte.light.m));
}

/* Set color matrix */
void SetColorMatrix(MATRIX *m) {
    GTE_COST("SetColorMatrix");
    if (!m) return;
    memcpy(&gte.color.m, m->m, sizeof(gte.color.m));
}

/* Set RGB color for primitives */
void SetRGBcd(CVECTOR *v) {
    if (!v) return;
    gte.rgbc = *v;
}

/* The lighting control registers.  gte.color.t holds RBK/GBK/BBK and
 * gte.light.t holds RFC/GFC/BFC, as REGISTER values: retail SetBackColor /
 * SetFarColor (asm/main/matched/SetBackColor.s, SetFarColor.s) shift each
 * component left by 4 before ctc2 (GsSetAmbient(2048, ..) -> SetBackColor
 * (128, ..) -> BK = 2048; measured in a retail TACO save state).  The old
 * HAL stored the unshifted value; LightColor / DpqColor / Intpl below keep
 * their old (unreached, approximate) arithmetic by shifting back. */
void SetBackColor(s32 rbk, s32 gbk, s32 bbk) {
    GTE_COST("SetBackColor");
    gte.color.t[0] = (s32)((u32)rbk << 4);
    gte.color.t[1] = (s32)((u32)gbk << 4);
    gte.color.t[2] = (s32)((u32)bbk << 4);
}

void SetFarColor(s32 rfc, s32 gfc, s32 bfc) {
    GTE_COST("SetFarColor");
    gte.light.t[0] = (s32)((u32)rfc << 4);
    gte.light.t[1] = (s32)((u32)gfc << 4);
    gte.light.t[2] = (s32)((u32)bfc << 4);
}

/* Push current rotation matrix to stack */
void PushMatrix(void) {
    GTE_COST("PushMatrix");
    if (gte.stack_ptr < 31) {
        gte.rot_stack[gte.stack_ptr++] = gte.rot;
    }
}

/* Pop rotation matrix from stack */
void PopMatrix(void) {
    GTE_COST("PopMatrix");
    if (gte.stack_ptr > 0) {
        gte.rot = gte.rot_stack[--gte.stack_ptr];
    }
}

/* Read current rotation matrix */
void ReadRotMatrix(MATRIX *m) {
    if (!m) return;
    memcpy(m->m, gte.rot.m, sizeof(m->m));
    m->t[0] = gte.trans.vx;
    m->t[1] = gte.trans.vy;
    m->t[2] = gte.trans.vz;
}

/* Read geometry offset */
void ReadGeomOffset(s32 *ofx, s32 *ofy) {
    if (ofx) *ofx = gte.ofx >> 16;
    if (ofy) *ofy = gte.ofy >> 16;
}

/* Read geometry screen distance */
s32 ReadGeomScreen(void) {
    return gte.h;
}

/* Read SZ FIFO values */
void ReadSZfifo3(s32 *sz0, s32 *sz1, s32 *sz2) {
    if (sz0) *sz0 = gte.sz1;
    if (sz1) *sz1 = gte.sz2;
    if (sz2) *sz2 = gte.sz3;
}

void ReadSZfifo4(s32 *szx, s32 *sz0, s32 *sz1, s32 *sz2) {
    if (szx) *szx = gte.sz0;
    if (sz0) *sz0 = gte.sz1;
    if (sz1) *sz1 = gte.sz2;
    if (sz2) *sz2 = gte.sz3;
}

/* Read screen XY FIFO */
void ReadSXSYfifo(s32 *sxy0, s32 *sxy1, s32 *sxy2) {
    if (sxy0) *sxy0 = gte.sxy1;
    if (sxy1) *sxy1 = gte.sxy2;
    if (sxy2) *sxy2 = gte.sxy3;
}

/* Read RGB FIFO */
void ReadRGBfifo(CVECTOR *v0, CVECTOR *v1, CVECTOR *v2) {
    if (v0) *v0 = gte.rgb0;
    if (v1) *v1 = gte.rgb1;
    if (v2) *v2 = gte.rgb2;
}

/* Apply rotation matrix to vector */
VECTOR *ApplyRotMatrix(SVECTOR *v0, VECTOR *v1) {
    if (!v0 || !v1) return v1;

    s32 x = v0->vx, y = v0->vy, z = v0->vz;

    v1->vx = (gte.rot.m[0][0] * x + gte.rot.m[0][1] * y + gte.rot.m[0][2] * z) >> GTE_SHIFT;
    v1->vy = (gte.rot.m[1][0] * x + gte.rot.m[1][1] * y + gte.rot.m[1][2] * z) >> GTE_SHIFT;
    v1->vz = (gte.rot.m[2][0] * x + gte.rot.m[2][1] * y + gte.rot.m[2][2] * z) >> GTE_SHIFT;

    return v1;
}

/* Apply rotation matrix to long vector */
VECTOR *ApplyRotMatrixLV(VECTOR *v0, VECTOR *v1) {
    if (!v0 || !v1) return v1;

    s64 x = v0->vx, y = v0->vy, z = v0->vz;

    v1->vx = (s32)((gte.rot.m[0][0] * x + gte.rot.m[0][1] * y + gte.rot.m[0][2] * z) >> GTE_SHIFT);
    v1->vy = (s32)((gte.rot.m[1][0] * x + gte.rot.m[1][1] * y + gte.rot.m[1][2] * z) >> GTE_SHIFT);
    v1->vz = (s32)((gte.rot.m[2][0] * x + gte.rot.m[2][1] * y + gte.rot.m[2][2] * z) >> GTE_SHIFT);

    return v1;
}

/* Apply matrix to vector */
VECTOR *ApplyMatrix(MATRIX *m, SVECTOR *v0, VECTOR *v1) {
    if (!m || !v0 || !v1) return v1;

    s32 x = v0->vx, y = v0->vy, z = v0->vz;

    v1->vx = (m->m[0][0] * x + m->m[0][1] * y + m->m[0][2] * z) >> GTE_SHIFT;
    v1->vy = (m->m[1][0] * x + m->m[1][1] * y + m->m[1][2] * z) >> GTE_SHIFT;
    v1->vz = (m->m[2][0] * x + m->m[2][1] * y + m->m[2][2] * z) >> GTE_SHIFT;

    return v1;
}

/* Apply matrix to long vector */
VECTOR *ApplyMatrixLV(MATRIX *m, VECTOR *v0, VECTOR *v1) {
    GTE_COST("ApplyMatrixLV");
    if (!m || !v0 || !v1) return v1;

    s64 x = v0->vx, y = v0->vy, z = v0->vz;

    v1->vx = (s32)((m->m[0][0] * x + m->m[0][1] * y + m->m[0][2] * z) >> GTE_SHIFT);
    v1->vy = (s32)((m->m[1][0] * x + m->m[1][1] * y + m->m[1][2] * z) >> GTE_SHIFT);
    v1->vz = (s32)((m->m[2][0] * x + m->m[2][1] * y + m->m[2][2] * z) >> GTE_SHIFT);

    return v1;
}

/* Apply matrix to short vector, result is short vector */
SVECTOR *ApplyMatrixSV(MATRIX *m, SVECTOR *v0, SVECTOR *v1) {
    if (!m || !v0 || !v1) return v1;

    s32 x = v0->vx, y = v0->vy, z = v0->vz;

    v1->vx = (s16)clamp16((m->m[0][0] * x + m->m[0][1] * y + m->m[0][2] * z) >> GTE_SHIFT);
    v1->vy = (s16)clamp16((m->m[1][0] * x + m->m[1][1] * y + m->m[1][2] * z) >> GTE_SHIFT);
    v1->vz = (s16)clamp16((m->m[2][0] * x + m->m[2][1] * y + m->m[2][2] * z) >> GTE_SHIFT);

    return v1;
}

/* Apply transpose of matrix to long vector */
VECTOR *ApplyTransposeMatrixLV(MATRIX *m, VECTOR *v0, VECTOR *v1) {
    if (!m || !v0 || !v1) return v1;

    s64 x = v0->vx, y = v0->vy, z = v0->vz;

    v1->vx = (s32)((m->m[0][0] * x + m->m[1][0] * y + m->m[2][0] * z) >> GTE_SHIFT);
    v1->vy = (s32)((m->m[0][1] * x + m->m[1][1] * y + m->m[2][1] * z) >> GTE_SHIFT);
    v1->vz = (s32)((m->m[0][2] * x + m->m[1][2] * y + m->m[2][2] * z) >> GTE_SHIFT);

    return v1;
}

/* Rotate and translate a vertex (no perspective) */
void RotTrans(SVECTOR *v0, VECTOR *v1, s32 *flag) {
    if (!v0 || !v1) return;

    s32 x = v0->vx, y = v0->vy, z = v0->vz;

    /* Apply rotation */
    gte.mac1 = (gte.rot.m[0][0] * x + gte.rot.m[0][1] * y + gte.rot.m[0][2] * z) >> GTE_SHIFT;
    gte.mac2 = (gte.rot.m[1][0] * x + gte.rot.m[1][1] * y + gte.rot.m[1][2] * z) >> GTE_SHIFT;
    gte.mac3 = (gte.rot.m[2][0] * x + gte.rot.m[2][1] * y + gte.rot.m[2][2] * z) >> GTE_SHIFT;

    /* Add translation */
    v1->vx = gte.mac1 + gte.trans.vx;
    v1->vy = gte.mac2 + gte.trans.vy;
    v1->vz = gte.mac3 + gte.trans.vz;

    gte.ir1 = clamp16(v1->vx);
    gte.ir2 = clamp16(v1->vy);
    gte.ir3 = clamp16(v1->vz);

    gte.flag = 0;
    if (flag) *flag = gte.flag;
}

/* Rotate and translate to short vector */
void RotTransSV(SVECTOR *v0, SVECTOR *v1, s32 *flag) {
    VECTOR tmp;
    RotTrans(v0, &tmp, flag);
    if (v1) {
        v1->vx = (s16)clamp16(tmp.vx);
        v1->vy = (s16)clamp16(tmp.vy);
        v1->vz = (s16)clamp16(tmp.vz);
    }
}

/* ---- COP2 perspective pipeline ----------------------------------------
 * RTPS/RTPT, NCLIP and AVSZ3/AVSZ4 as the GTE computes them (psx-spx "GTE"
 * chapter; checked bit-exact against all 12 title-card FT4 packets in a
 * retail RAM image):
 *
 *   MAC = (TR << 12 + R*V) >> 12, 44-bit overflow -> FLAG 30..25
 *   IR  = MAC saturated to -8000h..7FFFh             -> FLAG 24..22
 *   SZ3 = MAC3 saturated to 0..FFFFh                  -> FLAG 18
 *   n   = H/SZ3 by the hardware's UNR reciprocal      -> FLAG 17 (n=1FFFFh)
 *   SX2 = (n*IR1 + OFX) >> 16, SY2 likewise, saturated to -400h..3FFh
 *                                                     -> FLAG 14 / 13
 *   IR0 = (n*DQA + DQB) >> 12 saturated to 0..1000h   -> FLAG 12
 *   FLAG 31 = OR of bits 30..23 and 18..13 (bit 17 divide and bit 22 IR3
 *   are NOT part of it).
 *
 * Every command clears FLAG first; RTPT accumulates over its three vertices.
 * The libgs TMD builders test FLAG bit 31 (bltz) to drop a primitive, so the
 * bit set matters as much as the coordinates.  The previous model used a
 * truncating H/SZ division, never set bit 31 and clamped OTZ to 4095. */
static u16 gte_unr[257];
static int gte_unr_ready;

static u32 gte_divide(u32 h, u32 sz) {
    u32 z, n, d, u;
    s32 i;
    if (!gte_unr_ready) {
        for (i = 0; i < 257; i++) {
            s32 v = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
            gte_unr[i] = (u16)(v < 0 ? 0 : v);
        }
        gte_unr_ready = 1;
    }
    if (h >= sz * 2) {
        gte.flag |= 1u << 17;
        return 0x1FFFF;
    }
    z = 0;
    while (z < 16 && !((sz << z) & 0x8000u)) z++;
    n = h << z;
    d = sz << z;
    u = (u32)gte_unr[(d - 0x7FC0) >> 7] + 0x101;
    d = (0x2000080u - d * u) >> 8;
    d = (0x0000080u + d * u) >> 8;
    {
        u64 q = ((u64)n * d + 0x8000u) >> 16;
        return q > 0x1FFFF ? 0x1FFFF : (u32)q;
    }
}

static s64 gte_mac44(s64 v, s32 index) {
    if (v > 0x7FFFFFFFFFFLL) gte.flag |= 1u << (31 - index);
    else if (v < -0x80000000000LL) gte.flag |= 1u << (28 - index);
    return v;
}

static s32 gte_ir(s64 v, s32 index) {
    if (v < -0x8000) { gte.flag |= 1u << (25 - index); return -0x8000; }
    if (v > 0x7FFF) { gte.flag |= 1u << (25 - index); return 0x7FFF; }
    return (s32)v;
}

static s64 gte_mac0(s64 v) {
    if (v > 0x7FFFFFFFLL) gte.flag |= 1u << 16;
    else if (v < -0x80000000LL) gte.flag |= 1u << 15;
    return v;
}

static s32 gte_sxy(s64 v, s32 bit) {
    if (v < -0x400) { gte.flag |= 1u << bit; return -0x400; }
    if (v > 0x3FF) { gte.flag |= 1u << bit; return 0x3FF; }
    return (s32)v;
}

/* Sub-pixel vertex precision (pc/hal/gpu_pgxp.h, enhanced only): the
 * 16.16 sums whose integer parts become SX2/SY2 keep their fractions in a
 * side table keyed by the SXY word, and after each RTPT / RTPS the FIFO's
 * three words are recorded as one primitive key.  g_pf shadows the SXY
 * FIFO (SXY0..SXY2 = gte.sxy1..sxy3) with each word's fractions; a slot
 * counts only while its word is still the FIFO's (NormalClip, or anything
 * else that loads the FIFO, leaves it unmatched).  Observation only -- no
 * GTE register, flag or result depends on it.  Weak: the GTE harnesses
 * link this file without the GPU. */
#include "gpu_pgxp.h"
#pragma weak g_gpu_pgxp_active
#pragma weak g_gpu_pgxp_stats
#pragma weak gpu_pgxp_record
#pragma weak gpu_pgxp_record_tri
static struct {
    u32 word;
    u32 fx, fy;
    int ok;
} g_pf[3];

#define GTE_PGXP_ON() (&g_gpu_pgxp_active && g_gpu_pgxp_active && gpu_pgxp_record)

static void gte_pgxp_tri(void) {
    u32 w[3], fx[3], fy[3];
    s32 k;
    if (!GTE_PGXP_ON() || !gpu_pgxp_record_tri) return;
    w[0] = (u32)gte.sxy1; w[1] = (u32)gte.sxy2; w[2] = (u32)gte.sxy3;
    for (k = 0; k < 3; k++) {
        if (!g_pf[k].ok || g_pf[k].word != w[k]) return;
        fx[k] = g_pf[k].fx;
        fy[k] = g_pf[k].fy;
    }
    gpu_pgxp_record_tri(w, fx, fy);
}

static void gte_rtp(const SVECTOR *v, s32 last) {
    s64 m[3];
    s32 i;
    u32 n;
    s32 sx, sy;
    s64 sz;
    for (i = 0; i < 3; i++) {
        s64 t = (i == 0) ? gte.trans.vx : (i == 1) ? gte.trans.vy : gte.trans.vz;
        s64 acc = t * 4096
                + (s64)gte.rot.m[i][0] * v->vx
                + (s64)gte.rot.m[i][1] * v->vy
                + (s64)gte.rot.m[i][2] * v->vz;
        m[i] = gte_mac44(acc, i + 1) >> 12;
    }
    gte.mac1 = (s32)m[0];
    gte.mac2 = (s32)m[1];
    gte.mac3 = (s32)m[2];
    gte.ir1 = gte_ir(m[0], 1);
    gte.ir2 = gte_ir(m[1], 2);
    gte.ir3 = gte_ir(m[2], 3);
    sz = m[2];
    if (sz < 0) { sz = 0; gte.flag |= 1u << 18; }
    if (sz > 0xFFFF) { sz = 0xFFFF; gte.flag |= 1u << 18; }
    gte.sz0 = gte.sz1;
    gte.sz1 = gte.sz2;
    gte.sz2 = gte.sz3;
    gte.sz3 = (s32)sz;
    n = gte_divide((u32)(u16)gte.h, (u32)sz);
    {
        s64 ax = (s64)n * gte.ir1 + gte.ofx;
        s64 ay = (s64)n * gte.ir2 + gte.ofy;
        sx = gte_sxy(gte_mac0(ax) >> 16, 14);
        sy = gte_sxy(gte_mac0(ay) >> 16, 13);
        gte.sxy0 = gte.sxy1;
        gte.sxy1 = gte.sxy2;
        gte.sxy2 = gte.sxy3;
        gte.sxy3 = (s32)(((u32)(u16)sy << 16) | (u16)sx);
        if (GTE_PGXP_ON()) {
            /* only an unclamped projection: then sx/sy = floor(a / 65536)
             * exactly, and the fraction refines it inside that pixel */
            g_pf[0] = g_pf[1];
            g_pf[1] = g_pf[2];
            g_pf[2].word = (u32)gte.sxy3;
            g_pf[2].fx = (u32)ax & 0xFFFFu;
            g_pf[2].fy = (u32)ay & 0xFFFFu;
            g_pf[2].ok = (ax >> 16) == sx && (ay >> 16) == sy;
            if (g_pf[2].ok)
                gpu_pgxp_record(g_pf[2].word, g_pf[2].fx, g_pf[2].fy);
            else
                g_gpu_pgxp_stats.saturated++;
        }
    }
    if (last) {
        s64 dq = gte_mac0((s64)n * (s16)gte.dqa + gte.dqb);
        gte.mac0 = (s32)dq;
        dq >>= 12;
        if (dq < 0) { dq = 0; gte.flag |= 1u << 12; }
        if (dq > 0x1000) { dq = 0x1000; gte.flag |= 1u << 12; }
        gte.ir0 = (s32)dq;
        gte.p = gte.ir0;
    }
}

void gte_cop2_ldv(s32 slot, const SVECTOR *v) {
    if (slot >= 0 && slot < 3) gte.v[slot] = *v;
}

void gte_cop2_rtps(void) {
    gte.flag = 0;
    gte_rtp(&gte.v[0], 1);
    gte_pgxp_tri();
}

void gte_cop2_rtpt(void) {
    gte.flag = 0;
    gte_rtp(&gte.v[0], 0);
    gte_rtp(&gte.v[1], 0);
    gte_rtp(&gte.v[2], 1);
    gte_pgxp_tri();
}

s32 gte_cop2_nclip(void) {
    s64 x0 = (s16)gte.sxy1, y0 = (s16)(gte.sxy1 >> 16);
    s64 x1 = (s16)gte.sxy2, y1 = (s16)(gte.sxy2 >> 16);
    s64 x2 = (s16)gte.sxy3, y2 = (s16)(gte.sxy3 >> 16);
    gte.flag = 0;
    gte.mac0 = (s32)gte_mac0(x0 * y1 + x1 * y2 + x2 * y0
                             - x0 * y2 - x1 * y0 - x2 * y1);
    return gte.mac0;
}

static s32 gte_otz(s64 v) {
    v = gte_mac0(v);
    gte.mac0 = (s32)v;
    v >>= 12;
    if (v < 0) { v = 0; gte.flag |= 1u << 18; }
    if (v > 0xFFFF) { v = 0xFFFF; gte.flag |= 1u << 18; }
    gte.otz = (s32)v;
    return gte.otz;
}

s32 gte_cop2_avsz3(void) {
    gte.flag = 0;
    return gte_otz((s64)(s16)gte.zsf3 * (gte.sz1 + gte.sz2 + gte.sz3));
}

s32 gte_cop2_avsz4(void) {
    gte.flag = 0;
    return gte_otz((s64)(s16)gte.zsf4
                   * (gte.sz0 + gte.sz1 + gte.sz2 + gte.sz3));
}

u32 gte_cop2_flag(void) {
    u32 f = gte.flag & 0x7FFFF000u;
    if (f & 0x7F87E000u) f |= 0x80000000u;
    return f;
}

/* SXY0..SXY2 (2 = newest) and SZ0..SZ3 (3 = newest). */
u32 gte_cop2_sxy(s32 i) {
    return (u32)(i == 0 ? gte.sxy1 : i == 1 ? gte.sxy2 : gte.sxy3);
}

s32 gte_cop2_sz(s32 i) {
    return i == 0 ? gte.sz0 : i == 1 ? gte.sz1 : i == 2 ? gte.sz2 : gte.sz3;
}

s32 gte_cop2_ir0(void) {
    return gte.ir0;
}

/* ---- lighting: NCCS / NCCT (psx-spx "GTE Lighting / Color Calculation")
 * The lit TMD builders load RGBC with mtc2, the normal(s) into V0..V2 and
 * issue NCCS (0x4B08041B) or NCCT (0x4B18043F): sf=1, lm=1.  Per vertex:
 *   IR  = sat_lm((LLM * V) >> 12)
 *   IR  = sat_lm((BK * 0x1000 + LCM * IR) >> 12)
 *   MAC = ((RGB * IR) << 4) >> 12
 *   FIFO <- [sat8(MAC1 >> 4), sat8(MAC2 >> 4), sat8(MAC3 >> 4), CODE];
 *   IR = sat_lm(MAC)
 * Checked against retail: 785 TG3/TF3 packets of two TACO frames are
 * reproduced word for word. */
static u32 g_rgbc;
static u32 g_rgb_fifo[3];

void gte_cop2_set_rgbc(u32 rgbc) {
    g_rgbc = rgbc;
    gte.rgbc.r = (u8)rgbc;
    gte.rgbc.g = (u8)(rgbc >> 8);
    gte.rgbc.b = (u8)(rgbc >> 16);
    gte.rgbc.cd = (u8)(rgbc >> 24);
}

static s32 gte_ir_lm(s64 v, s32 index, s32 lm) {
    s64 lo = lm ? 0 : -0x8000;
    if (v < lo) { gte.flag |= 1u << (25 - index); return (s32)lo; }
    if (v > 0x7FFF) { gte.flag |= 1u << (25 - index); return 0x7FFF; }
    return (s32)v;
}

static u32 gte_colour(s64 v, s32 index) {
    if (v < 0) { gte.flag |= 1u << (22 - index); return 0; }
    if (v > 0xFF) { gte.flag |= 1u << (22 - index); return 0xFF; }
    return (u32)v;
}

static void gte_ncc(const SVECTOR *n, s32 sf, s32 lm) {
    s32 sh = sf ? 12 : 0;
    s32 ir[3], ir2[3], i;
    s64 mac[3];
    u32 c[3];
    for (i = 0; i < 3; i++) {
        s64 acc = (s64)gte.light.m[i][0] * n->vx + (s64)gte.light.m[i][1] * n->vy
                + (s64)gte.light.m[i][2] * n->vz;
        ir[i] = gte_ir_lm(gte_mac44(acc, i + 1) >> sh, i + 1, lm);
    }
    for (i = 0; i < 3; i++) {
        s64 acc = (s64)gte.color.t[i] * 0x1000 + (s64)gte.color.m[i][0] * ir[0]
                + (s64)gte.color.m[i][1] * ir[1] + (s64)gte.color.m[i][2] * ir[2];
        ir2[i] = gte_ir_lm(gte_mac44(acc, i + 1) >> sh, i + 1, lm);
    }
    for (i = 0; i < 3; i++) {
        s64 rgb = (s64)((g_rgbc >> (8 * i)) & 0xFFu);
        mac[i] = gte_mac44((rgb * ir2[i]) << 4, i + 1) >> sh;
        c[i] = gte_colour(mac[i] >> 4, i + 1);
    }
    gte.mac1 = (s32)mac[0];
    gte.mac2 = (s32)mac[1];
    gte.mac3 = (s32)mac[2];
    gte.ir1 = gte_ir_lm(mac[0], 1, lm);
    gte.ir2 = gte_ir_lm(mac[1], 2, lm);
    gte.ir3 = gte_ir_lm(mac[2], 3, lm);
    g_rgb_fifo[0] = g_rgb_fifo[1];
    g_rgb_fifo[1] = g_rgb_fifo[2];
    g_rgb_fifo[2] = c[0] | (c[1] << 8) | (c[2] << 16) | (g_rgbc & 0xFF000000u);
}

void gte_cop2_nccs(void) {
    gte.flag = 0;
    gte_ncc(&gte.v[0], 1, 1);
}

void gte_cop2_ncct(void) {
    gte.flag = 0;
    gte_ncc(&gte.v[0], 1, 1);
    gte_ncc(&gte.v[1], 1, 1);
    gte_ncc(&gte.v[2], 1, 1);
}

/* NCDS / NCDT (psx-spx "GTE Coordinate Calculation Commands"): the NCC
 * light and colour stages, then depth cueing toward the far colour by IR0
 * (RTPS/RTPT's depth factor from DQA / DQB):
 *   IR  = sat_lm((LLM * V) >> 12);  IR = sat_lm((BK * 0x1000 + LCM * IR) >> 12)
 *   MAC = (RGB * IR) << 4                       (not shifted yet)
 *   IR  = sat((FC * 0x1000 - MAC) >> 12)        (lm = 0)
 *   MAC = (IR * IR0 + MAC) >> 12
 *   FIFO <- [sat8(MAC >> 4) .., CODE];  IR = sat_lm(MAC)
 * FC is gte.light.t (the register value SetFarColor stores).  The fog
 * TMD builder GsTMDfastTG3LFG issues NCDT 0x0F80416 (sf=1, lm=1). */
static void gte_ncd(const SVECTOR *n, s32 lm) {
    s32 ir[3], ir2[3], i;
    s64 mac[3];
    u32 c[3];
    for (i = 0; i < 3; i++) {
        s64 acc = (s64)gte.light.m[i][0] * n->vx + (s64)gte.light.m[i][1] * n->vy
                + (s64)gte.light.m[i][2] * n->vz;
        ir[i] = gte_ir_lm(gte_mac44(acc, i + 1) >> 12, i + 1, lm);
    }
    for (i = 0; i < 3; i++) {
        s64 acc = (s64)gte.color.t[i] * 0x1000 + (s64)gte.color.m[i][0] * ir[0]
                + (s64)gte.color.m[i][1] * ir[1] + (s64)gte.color.m[i][2] * ir[2];
        ir2[i] = gte_ir_lm(gte_mac44(acc, i + 1) >> 12, i + 1, lm);
    }
    for (i = 0; i < 3; i++) {
        s64 rgb = (s64)((g_rgbc >> (8 * i)) & 0xFFu);
        s64 m0 = gte_mac44((rgb * ir2[i]) << 4, i + 1);
        s32 d = gte_ir_lm(gte_mac44((s64)gte.light.t[i] * 0x1000 - m0, i + 1) >> 12,
                          i + 1, 0);
        mac[i] = gte_mac44((s64)d * gte.ir0 + m0, i + 1) >> 12;
        c[i] = gte_colour(mac[i] >> 4, i + 1);
    }
    gte.mac1 = (s32)mac[0];
    gte.mac2 = (s32)mac[1];
    gte.mac3 = (s32)mac[2];
    gte.ir1 = gte_ir_lm(mac[0], 1, lm);
    gte.ir2 = gte_ir_lm(mac[1], 2, lm);
    gte.ir3 = gte_ir_lm(mac[2], 3, lm);
    g_rgb_fifo[0] = g_rgb_fifo[1];
    g_rgb_fifo[1] = g_rgb_fifo[2];
    g_rgb_fifo[2] = c[0] | (c[1] << 8) | (c[2] << 16) | (g_rgbc & 0xFF000000u);
}

void gte_cop2_ncdt(void) {
    gte.flag = 0;
    gte_ncd(&gte.v[0], 1);
    gte_ncd(&gte.v[1], 1);
    gte_ncd(&gte.v[2], 1);
}

/* SetDQA / SetDQB -- retail 0x800A50DC / 0x800A50E8: `ctc2 a0,$27` /
 * `ctc2 a0,$28`.  DQA is a 16-bit register (RTPS/RTPT use it signed);
 * DQB is 32-bit.  The PC stubs these replace returned 0 and stored
 * nothing, so TAIIKU's taiiku_depth_set never reached the depth factor
 * the fog builder interpolates by. */
void SetDQA(s32 a) {
    gte.dqa = (s32)(s16)a;
}

void SetDQB(s32 b) {
    gte.dqb = b;
}

/* RGB0..RGB2 (2 = newest). */
u32 gte_cop2_rgb(s32 i) {
    return g_rgb_fifo[i < 0 ? 0 : i > 2 ? 2 : i];
}

/* The wrappers store through every output pointer like retail; a NULL
 * (which retail would write to kernel RAM at 0) is skipped on the host. */
#define GTE_OUT(ptr, value) do { if (ptr) *(ptr) = (value); } while (0)

/* RotTransPers -- retail 0x800B1650: RTPS; *sxy = SXY2, *p = IR0,
 * *flag = FLAG; returns SZ3 >> 2 (no further clamp). */
s32 RotTransPers(SVECTOR *v0, s32 *sxy, s32 *p, s32 *flag) {
    gte_cop2_ldv(0, v0);
    gte_cop2_rtps();
    GTE_OUT(sxy, (s32)gte_cop2_sxy(2));
    GTE_OUT(p, gte.ir0);
    GTE_OUT(flag, (s32)gte_cop2_flag());
    return gte.sz3 >> 2;
}

/* RotTransPers3 -- retail 0x800B167C: RTPT, the three SXY, IR0, FLAG,
 * SZ3 >> 2. */
s32 RotTransPers3(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2,
                  s32 *sxy0, s32 *sxy1, s32 *sxy2, s32 *p, s32 *flag) {
    gte_cop2_ldv(0, v0);
    gte_cop2_ldv(1, v1);
    gte_cop2_ldv(2, v2);
    gte_cop2_rtpt();
    GTE_OUT(sxy0, (s32)gte_cop2_sxy(0));
    GTE_OUT(sxy1, (s32)gte_cop2_sxy(1));
    GTE_OUT(sxy2, (s32)gte_cop2_sxy(2));
    GTE_OUT(p, gte.ir0);
    GTE_OUT(flag, (s32)gte_cop2_flag());
    return gte.sz3 >> 2;
}

/* RotTransPers4: RTPT(v0..v2) then RTPS(v3); FLAG and IR0 of the RTPS.
 * (No retail listing in this binary; follows libgte's RTPT+RTPS shape.) */
s32 RotTransPers4(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2, SVECTOR *v3,
                  s32 *sxy0, s32 *sxy1, s32 *sxy2, s32 *sxy3,
                  s32 *p, s32 *flag) {
    u32 f;
    gte_cop2_ldv(0, v0);
    gte_cop2_ldv(1, v1);
    gte_cop2_ldv(2, v2);
    gte_cop2_rtpt();
    GTE_OUT(sxy0, (s32)gte_cop2_sxy(0));
    GTE_OUT(sxy1, (s32)gte_cop2_sxy(1));
    GTE_OUT(sxy2, (s32)gte_cop2_sxy(2));
    f = gte_cop2_flag();
    gte_cop2_ldv(0, v3);
    gte_cop2_rtps();
    GTE_OUT(sxy3, (s32)gte_cop2_sxy(2));
    GTE_OUT(p, gte.ir0);
    GTE_OUT(flag, (s32)(f | gte_cop2_flag()));
    return gte.sz3 >> 2;
}

/* NormalClip: NCLIP over three explicit SXY words (MAC0). */
s32 NormalClip(s32 sxy0, s32 sxy1, s32 sxy2) {
    gte.sxy1 = sxy0;
    gte.sxy2 = sxy1;
    gte.sxy3 = sxy2;
    return gte_cop2_nclip();
}

/* AverageZ3/4 -- retail leaves (asm/main/matched/AverageZ{3,4}.s):
 * load SZ1..3 / SZ0..3 and AVSZ3 / AVSZ4 with the ZSF3/ZSF4 InitGeom set
 * (341 / 256). */
s32 AverageZ3(s32 sz0, s32 sz1, s32 sz2) {
    gte.sz1 = sz0;
    gte.sz2 = sz1;
    gte.sz3 = sz2;
    return gte_cop2_avsz3();
}

s32 AverageZ4(s32 sz0, s32 sz1, s32 sz2, s32 sz3) {
    gte.sz0 = sz0;
    gte.sz1 = sz1;
    gte.sz2 = sz2;
    gte.sz3 = sz3;
    return gte_cop2_avsz4();
}

/* RotAverage3/4: RotTransPers3/4 then AVSZ3/AVSZ4 (OTZ returned). */
s32 RotAverage3(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2,
                s32 *sxy0, s32 *sxy1, s32 *sxy2, s32 *p, s32 *flag) {
    RotTransPers3(v0, v1, v2, sxy0, sxy1, sxy2, p, flag);
    return gte_cop2_avsz3();
}

s32 RotAverage4(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2, SVECTOR *v3,
                s32 *sxy0, s32 *sxy1, s32 *sxy2, s32 *sxy3,
                s32 *p, s32 *flag) {
    RotTransPers4(v0, v1, v2, v3, sxy0, sxy1, sxy2, sxy3, p, flag);
    return gte_cop2_avsz4();
}

/* RotNclip3 / RotAverageNclip3: RTPT, FLAG, NCLIP; a back face returns
 * before the SXY/OTZ stores (the retail Nclip shape). */
s32 RotNclip3(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2,
              s32 *sxy0, s32 *sxy1, s32 *sxy2, s32 *p, s32 *otz, s32 *flag) {
    s32 nc;
    gte_cop2_ldv(0, v0);
    gte_cop2_ldv(1, v1);
    gte_cop2_ldv(2, v2);
    gte_cop2_rtpt();
    GTE_OUT(flag, (s32)gte_cop2_flag());
    GTE_OUT(p, gte.ir0);
    GTE_OUT(sxy0, (s32)gte_cop2_sxy(0));
    GTE_OUT(sxy1, (s32)gte_cop2_sxy(1));
    GTE_OUT(sxy2, (s32)gte_cop2_sxy(2));
    GTE_OUT(otz, gte.sz3 >> 2);
    nc = gte_cop2_nclip();
    return nc;
}

s32 RotAverageNclip3(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2,
                     s32 *sxy0, s32 *sxy1, s32 *sxy2,
                     s32 *p, s32 *otz, s32 *flag) {
    s32 nc;
    u32 f;
    gte_cop2_ldv(0, v0);
    gte_cop2_ldv(1, v1);
    gte_cop2_ldv(2, v2);
    gte_cop2_rtpt();
    f = gte_cop2_flag();
    GTE_OUT(flag, (s32)f);
    nc = gte_cop2_nclip();
    if (nc <= 0) return nc;
    GTE_OUT(sxy0, (s32)gte_cop2_sxy(0));
    GTE_OUT(sxy1, (s32)gte_cop2_sxy(1));
    GTE_OUT(sxy2, (s32)gte_cop2_sxy(2));
    GTE_OUT(p, gte.ir0);
    GTE_OUT(otz, gte_cop2_avsz3());
    return nc;
}

/* RotAverageNclip4 -- retail listing: RTPT, *flag = FLAG, NCLIP; if MAC0
 * <= 0 return it with nothing else stored.  Otherwise SXY0..2, RTPS(v3),
 * SXY3, IR0, *flag |= FLAG, AVSZ4 -> *otz; returns the NCLIP value. */
s32 RotAverageNclip4(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2, SVECTOR *v3,
                     s32 *sxy0, s32 *sxy1, s32 *sxy2, s32 *sxy3,
                     s32 *p, s32 *otz, s32 *flag) {
    s32 nc;
    u32 f;
    gte_cop2_ldv(0, v0);
    gte_cop2_ldv(1, v1);
    gte_cop2_ldv(2, v2);
    gte_cop2_rtpt();
    f = gte_cop2_flag();
    GTE_OUT(flag, (s32)f);
    nc = gte_cop2_nclip();
    if (nc <= 0) return nc;
    GTE_OUT(sxy0, (s32)gte_cop2_sxy(0));
    GTE_OUT(sxy1, (s32)gte_cop2_sxy(1));
    GTE_OUT(sxy2, (s32)gte_cop2_sxy(2));
    gte_cop2_ldv(0, v3);
    gte_cop2_rtps();
    GTE_OUT(sxy3, (s32)gte_cop2_sxy(2));
    GTE_OUT(p, gte.ir0);
    GTE_OUT(flag, (s32)(f | gte_cop2_flag()));
    GTE_OUT(otz, gte_cop2_avsz4());
    return nc;
}

/* Retail MulMatrix0 (0x800A5BA0): rotation only, destination in a2.
 * MVMVA accumulates the full dot product before the 12-bit shift and IR
 * saturation. Translation belongs to the caller (e.g. GsMulCoord0/2/3).
 * Use a temporary because either input may also be the destination.
 *
 * Retail MulMatrix0 / MulMatrix / MulMatrix2 (0x800A5BA0 / 0x800A49A0 /
 * 0x800A4AAC) write m[2][2] with `swc2 $11, 16(dst)`: IR3 as a whole word,
 * so the 2-byte pad after m[2][2] (bytes 18..19) receives IR3's sign
 * extension (0xFFFF for a negative element, 0 otherwise).  Matrices the
 * game copies whole (title_obj_move's `coord_buf[i].mat = ma`) carry that
 * pad into RAM. */
MATRIX *MulMatrix0(MATRIX *m0, MATRIX *m1, MATRIX *m2) {
    int i, j, k;
    s64 sum;
    s32 ir3;
    MATRIX tmp;

    if (!m0 || !m1 || !m2) return m2;

    GTE_INNER((SetRotMatrix(m0), m0));
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            sum = 0;
            for (k = 0; k < 3; k++) {
                sum += (s64)m0->m[i][k] * m1->m[k][j];
            }
            tmp.m[i][j] = (s16)clamp16((s32)(sum >> GTE_SHIFT));
        }
    }

    memcpy(m2->m, tmp.m, sizeof(tmp.m));
    ir3 = tmp.m[2][2];
    memcpy((u8 *)m2->m + 16, &ir3, 4);
    return m2;
}

/* Retail 0x800A49A0 stores through a0 and returns a0. */
MATRIX *MulMatrix(MATRIX *m0, MATRIX *m1) {
    GTE_COST("MulMatrix");
    return (MATRIX *)GTE_INNER(MulMatrix0(m0, m1, m0));
}

/* Retail 0x800A4AAC stores through a1 and returns a1. In particular,
 * GsMulCoord2 must keep its parent (often GsWSMATRIX) intact. */
MATRIX *MulMatrix2(MATRIX *m0, MATRIX *m1) {
    GTE_COST("MulMatrix2");
    return (MATRIX *)GTE_INNER(MulMatrix0(m0, m1, m1));
}

/* Multiply rotation portion only */
MATRIX *MulRotMatrix0(MATRIX *m0, MATRIX *m1) {
    int i, j, k;
    MATRIX tmp;

    if (!m0 || !m1) return m1;

    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            s32 sum = 0;
            for (k = 0; k < 3; k++) {
                sum += ((s32)m0->m[i][k] * m1->m[k][j]) >> GTE_SHIFT;
            }
            tmp.m[i][j] = (s16)clamp16(sum);
        }
    }

    memcpy(m1->m, tmp.m, sizeof(tmp.m));
    return m1;
}

MATRIX *MulRotMatrix(MATRIX *m0) {
    return MulRotMatrix0(&gte.rot, m0);
}

/* Set and multiply in one step */
MATRIX *SetMulMatrix(MATRIX *m0, MATRIX *m1) {
    GTE_INNER((SetRotMatrix(m0), m0));
    return (MATRIX *)GTE_INNER(MulMatrix(m0, m1));
}

MATRIX *SetMulRotMatrix(MATRIX *m0) {
    GTE_INNER((SetRotMatrix(m0), m0));
    return MulRotMatrix(m0);
}

/* RotMatrix -- retail LIBGTE FGO_01 @0x800A4710 (asm/main/matched/RotMatrix.s).
 *
 * The angles index LIBGTE's rcossin_tbl (PS1 0x800E4C00: 4096 words, sin in
 * the low half, cos in the high half), which is retail data loaded from the
 * EXE into the PS1 RAM block -- the table is not reproducible from libm
 * (3,888 of its 4,096 entries differ from round(4096*sin/cos)).  A negative
 * angle looks up -a & 0xFFF and negates the sine.
 *
 * Result: M = Rx * Ry * Rz.  Each product is a 32-bit `multu` low word
 * followed by `sra 12`; where retail negates, it negates the PRODUCT before
 * the shift (-(a*b) >> 12, not -((a*b) >> 12)), and the three-factor terms
 * shift the first product before the second multiply.  Both orders matter
 * for the low bit and are kept.  Before 2026-10-30 this composed Rz*Ry*Rx
 * from float sinf/cosf: every title card's orientation was wrong (measured
 * against retail RAM at the same frame_counter). */
extern u32 rcossin_tbl[4096];

static void gte_cossin(s32 a, s32 *s, s32 *c) {
    u32 w;
    if (a >= 0) {
        w = rcossin_tbl[a & 0xFFF];
        *s = (s16)(w & 0xFFFF);
    } else {
        w = rcossin_tbl[(-a) & 0xFFF];
        *s = -(s32)(s16)(w & 0xFFFF);
    }
    *c = (s32)w >> 16;
}

/* RotMatrixX / RotMatrixY / RotMatrixZ -- LIBGTE FGO_04/05/06, the
 * hand-written assembly members TACO.EXN (0x8015D0B0 / 0x8015D250 /
 * 0x8015CF10) and DATE.EXN (0x8015A630 / 0x8015A970 / 0x8015A7D0) link
 * verbatim (asm/overlay/<OV>/RotMatrix[XYZ].s).  Multiply m in place by
 * the rotation about one axis and return m.  From the listings:
 *   s, c from rcossin_tbl[|a| & 0xFFF]; X and Z negate s for a < 0, Y
 *   negates it for a >= 0 (its sin is -sin(a));
 *   with rows (p, q) = (1, 2) for X, (0, 2) for Y, (0, 1) for Z:
 *     p' = (c*p - s*q) >> 12,  q' = (s*p + c*q) >> 12
 *   each product a 32-bit `multu` low word, the sum taken in 32 bits, then
 *   `sra 12` and `sh` (both from the ORIGINAL rows).
 * Before 2026-10-31 the PC link resolved TACO_/DATE_RotMatrix[XYZ] to
 * `return 0` stubs: every caller's matrix kept its identity rows. */
static MATRIX *gte_rot_axis(s32 a, MATRIX *m, s32 p, s32 q, s32 neg_on_positive) {
    u32 w;
    s32 s, c, k, rp[3], rq[3];
    if (a >= 0) {
        w = rcossin_tbl[a & 0xFFF];
        s = (s16)(w & 0xFFFF);
        if (neg_on_positive) s = -s;
    } else {
        w = rcossin_tbl[(u32)(-(u32)a) & 0xFFF];
        s = (s16)(w & 0xFFFF);
        if (!neg_on_positive) s = -s;
    }
    c = (s32)w >> 16;
    for (k = 0; k < 3; k++) {
        rp[k] = m->m[p][k];
        rq[k] = m->m[q][k];
    }
    for (k = 0; k < 3; k++) {
        m->m[p][k] = (s16)((s32)((u32)c * (u32)rp[k] - (u32)s * (u32)rq[k]) >> 12);
        m->m[q][k] = (s16)((s32)((u32)s * (u32)rp[k] + (u32)c * (u32)rq[k]) >> 12);
    }
    return m;
}

MATRIX *gte_rot_matrix_x(s32 a, MATRIX *m) { return gte_rot_axis(a, m, 1, 2, 0); }
MATRIX *gte_rot_matrix_y(s32 a, MATRIX *m) { return gte_rot_axis(a, m, 0, 2, 1); }
MATRIX *gte_rot_matrix_z(s32 a, MATRIX *m) { return gte_rot_axis(a, m, 0, 1, 0); }

static s32 gte_mul12(s32 a, s32 b) {
    return (s32)((u32)a * (u32)b) >> 12;
}

static s32 gte_negmul12(s32 a, s32 b) {
    return (s32)(0u - (u32)a * (u32)b) >> 12;
}

MATRIX *RotMatrix(SVECTOR *r, MATRIX *m) {
    s32 cx, cy, cz, sx, sy, sz, t;

    GTE_COST("RotMatrix");
    gte_cossin(r->vx, &sx, &cx);
    gte_cossin(r->vy, &sy, &cy);
    m->m[0][2] = (s16)sy;
    m->m[1][2] = (s16)gte_negmul12(cy, sx);
    m->m[2][2] = (s16)gte_mul12(cy, cx);
    gte_cossin(r->vz, &sz, &cz);
    m->m[0][0] = (s16)gte_mul12(cz, cy);
    m->m[0][1] = (s16)gte_negmul12(sz, cy);
    t = gte_mul12(cz, -sy);
    m->m[1][0] = (s16)(gte_mul12(sz, cx) - gte_mul12(t, sx));
    m->m[2][0] = (s16)(gte_mul12(sz, sx) + gte_mul12(t, cx));
    t = gte_mul12(sz, -sy);
    m->m[1][1] = (s16)(gte_mul12(cz, cx) + gte_mul12(t, sx));
    m->m[2][1] = (s16)(gte_mul12(cz, sx) - gte_mul12(t, cx));
    return m;
}

/* Create translation matrix */
MATRIX *TransMatrix(MATRIX *m, VECTOR *v) {
    GTE_COST("TransMatrix");
    if (!m || !v) return m;
    m->t[0] = v->vx;
    m->t[1] = v->vy;
    m->t[2] = v->vz;
    return m;
}

/* Create scale matrix */
MATRIX *ScaleMatrix(MATRIX *m, VECTOR *v) {
    GTE_COST("ScaleMatrix");
    if (!m || !v) return m;

    m->m[0][0] = (s16)((m->m[0][0] * v->vx) >> GTE_SHIFT);
    m->m[0][1] = (s16)((m->m[0][1] * v->vx) >> GTE_SHIFT);
    m->m[0][2] = (s16)((m->m[0][2] * v->vx) >> GTE_SHIFT);

    m->m[1][0] = (s16)((m->m[1][0] * v->vy) >> GTE_SHIFT);
    m->m[1][1] = (s16)((m->m[1][1] * v->vy) >> GTE_SHIFT);
    m->m[1][2] = (s16)((m->m[1][2] * v->vy) >> GTE_SHIFT);

    m->m[2][0] = (s16)((m->m[2][0] * v->vz) >> GTE_SHIFT);
    m->m[2][1] = (s16)((m->m[2][1] * v->vz) >> GTE_SHIFT);
    m->m[2][2] = (s16)((m->m[2][2] * v->vz) >> GTE_SHIFT);

    return m;
}

/* Scale matrix with long vector */
MATRIX *ScaleMatrixL(MATRIX *m, VECTOR *v) {
    return (MATRIX *)GTE_INNER(ScaleMatrix(m, v));  /* Same implementation for PC */
}

/* Transpose matrix */
MATRIX *TransposeMatrix(MATRIX *m0, MATRIX *m1) {
    MATRIX tmp;
    int i, j;

    if (!m0 || !m1) return m1;

    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            tmp.m[i][j] = m0->m[j][i];
        }
    }
    tmp.t[0] = m0->t[0];
    tmp.t[1] = m0->t[1];
    tmp.t[2] = m0->t[2];

    *m1 = tmp;
    return m1;
}

/* Composite matrix: m2 = m0 * m1, including translation */
MATRIX *CompMatrix(MATRIX *m0, MATRIX *m1, MATRIX *m2) {
    VECTOR input, output;
    s32 trans[3];
    int i;

    if (!m0 || !m1 || !m2) return m2;

    input.vx = m1->t[0];
    input.vy = m1->t[1];
    input.vz = m1->t[2];
    GTE_INNER(ApplyMatrixLV(m0, &input, &output));
    trans[0] = (s32)((u32)output.vx + (u32)m0->t[0]);
    trans[1] = (s32)((u32)output.vy + (u32)m0->t[1]);
    trans[2] = (s32)((u32)output.vz + (u32)m0->t[2]);
    MulMatrix0(m0, m1, m2);
    for (i = 0; i < 3; i++) m2->t[i] = trans[i];
    return m2;
}

MATRIX *CompMatrixLV(MATRIX *m0, MATRIX *m1, MATRIX *m2) {
    return CompMatrix(m0, m1, m2);
}

/* Check if matrix is identity */
int IsIdMatrix(MATRIX *m) {
    if (!m) return 0;
    return (m->m[0][0] == GTE_ONE && m->m[0][1] == 0 && m->m[0][2] == 0 &&
            m->m[1][0] == 0 && m->m[1][1] == GTE_ONE && m->m[1][2] == 0 &&
            m->m[2][0] == 0 && m->m[2][1] == 0 && m->m[2][2] == GTE_ONE);
}

/* Square of vector components */
VECTOR *Square12(VECTOR *v0, VECTOR *v1) {
    if (!v0 || !v1) return v1;
    v1->vx = ((s64)v0->vx * v0->vx) >> GTE_SHIFT;
    v1->vy = ((s64)v0->vy * v0->vy) >> GTE_SHIFT;
    v1->vz = ((s64)v0->vz * v0->vz) >> GTE_SHIFT;
    return v1;
}

VECTOR *Square0(VECTOR *v0, VECTOR *v1) {
    if (!v0 || !v1) return v1;
    v1->vx = v0->vx * v0->vx;
    v1->vy = v0->vy * v0->vy;
    v1->vz = v0->vz * v0->vz;
    return v1;
}

/* Square root (integer) */
/* SquareRoot0 / SquareRoot12 -- retail LIBGTE MSC01 @0x800A4E60/0x800A4EE4
 * (asm/main/matched/SquareRoot0.s).  The GTE's LZCR picks an even shift,
 * the top bits index a 192-entry s16 table (PS1 0x800E45D0, retail data
 * loaded from the EXE) and the entry is shifted back.  This is an
 * approximation: SquareRoot0(1000000) is 999, not 1000.  GsSetRefView2
 * divides by it, so the title's world-screen matrix is diag(-4100, 4096,
 * -4100) on retail; the sqrtf version gave -4096 (measured in RAM). */
extern s16 gte_sqrt_tbl[];

static s32 gte_lzcs(s32 a) {
    u32 v = (a < 0) ? ~(u32)a : (u32)a;
    s32 n = 0;
    while (n < 32 && !(v & 0x80000000u)) {
        v <<= 1;
        n++;
    }
    return n;
}

/* Returns 0 for a == 0 (and -1); else the table entry and its shift. */
static s32 gte_square_root(s32 a, s32 bias, s32 *shift) {
    s32 lz, even, top;
    lz = gte_lzcs(a);
    if (lz == 32) return 0;
    even = lz & ~1;
    *shift = (bias - even) >> 1;
    if (even - 24 >= 0)
        top = (s32)((u32)a << (even - 24));
    else
        top = a >> (24 - even);
    return gte_sqrt_tbl[top - 64];
}

s32 SquareRoot0(s32 a) {
    s32 shift = 0;
    GTE_COST("SquareRoot0");
    s32 t = gte_square_root(a, 31, &shift);
    return (s32)(((u32)t << (shift & 31)) >> 12);
}

s32 SquareRoot12(s32 a) {
    s32 shift = 0;
    s32 t = gte_square_root(a, 19, &shift);
    if (shift >= 0) return (s32)((u32)t << (shift & 31));
    return (s32)((u32)t >> ((-shift) & 31));
}

/* Inverse square root */
void InvSquareRoot(s32 a, s32 *b, s32 *c) {
    if (a <= 0) {
        if (b) *b = 0;
        if (c) *c = 0;
        return;
    }
    float inv = 1.0f / sqrtf((float)a / GTE_ONE);
    if (b) *b = (s32)(inv * GTE_ONE);
    if (c) *c = (s32)(inv * inv * GTE_ONE);
}

/* Vector normalization */
s32 VectorNormal(VECTOR *v0, VECTOR *v1) {
    s64 len_sq;
    s32 len;

    if (!v0 || !v1) return 0;

    len_sq = (s64)v0->vx * v0->vx + (s64)v0->vy * v0->vy + (s64)v0->vz * v0->vz;
    len = (s32)sqrtf((float)len_sq);

    if (len > 0) {
        v1->vx = (v0->vx * GTE_ONE) / len;
        v1->vy = (v0->vy * GTE_ONE) / len;
        v1->vz = (v0->vz * GTE_ONE) / len;
    } else {
        v1->vx = v1->vy = v1->vz = 0;
    }

    return len;
}

s32 VectorNormalS(VECTOR *v0, SVECTOR *v1) {
    VECTOR tmp;
    s32 len = VectorNormal(v0, &tmp);
    if (v1) {
        v1->vx = (s16)clamp16(tmp.vx);
        v1->vy = (s16)clamp16(tmp.vy);
        v1->vz = (s16)clamp16(tmp.vz);
    }
    return len;
}

/* Outer (cross) product */
void OuterProduct12(VECTOR *v0, VECTOR *v1, VECTOR *v2) {
    if (!v0 || !v1 || !v2) return;

    v2->vx = (((s64)v0->vy * v1->vz) - ((s64)v0->vz * v1->vy)) >> GTE_SHIFT;
    v2->vy = (((s64)v0->vz * v1->vx) - ((s64)v0->vx * v1->vz)) >> GTE_SHIFT;
    v2->vz = (((s64)v0->vx * v1->vy) - ((s64)v0->vy * v1->vx)) >> GTE_SHIFT;
}

void OuterProduct0(VECTOR *v0, VECTOR *v1, VECTOR *v2) {
    if (!v0 || !v1 || !v2) return;

    v2->vx = v0->vy * v1->vz - v0->vz * v1->vy;
    v2->vy = v0->vz * v1->vx - v0->vx * v1->vz;
    v2->vz = v0->vx * v1->vy - v0->vy * v1->vx;
}

/* Leading zero count */
s32 Lzc(s32 data) {
    s32 count = 0;
    u32 udata;

    if (data < 0) {
        udata = ~(u32)data;
    } else {
        udata = (u32)data;
    }

    if (udata == 0) return 32;

    while ((udata & 0x80000000) == 0) {
        count++;
        udata <<= 1;
    }

    return count;
}

/* rsin/rcos are the matched src/main/matched/{rsin,sin_1,rcos}.c, reading
 * the retail rsin_tbl in the PS1 RAM block.  The float sinf/cosf versions
 * that used to live here shadowed them (pc/Makefile HAL_EXCLUDES) and
 * returned different values: title_obj_move's card accumulators step by
 * (abs(rsin(frame_counter)) & 1) + 2, so the low bit alone moved every
 * title card 24 units deeper than retail by frame_counter 1105. */

/* Atan2 - returns angle in PS1 format (0-4095) */
s32 ratan2(s32 y, s32 x) {
    float rad = atan2f((float)y, (float)x);
    s32 angle = (s32)(rad * 4096.0f / (3.14159265358979f * 2.0f));
    return angle & 0xFFF;
}

/* Color/light functions */
void LocalLight(SVECTOR *v0, VECTOR *v1) {
    /* Apply light matrix to normal vector */
    if (!v0 || !v1) return;
    ApplyMatrix(&gte.light, v0, v1);
}

void LightColor(VECTOR *v0, VECTOR *v1) {
    /* Apply color matrix to light vector */
    if (!v0 || !v1) return;

    v1->vx = (gte.color.m[0][0] * v0->vx + gte.color.m[0][1] * v0->vy + gte.color.m[0][2] * v0->vz) >> GTE_SHIFT;
    v1->vy = (gte.color.m[1][0] * v0->vx + gte.color.m[1][1] * v0->vy + gte.color.m[1][2] * v0->vz) >> GTE_SHIFT;
    v1->vz = (gte.color.m[2][0] * v0->vx + gte.color.m[2][1] * v0->vy + gte.color.m[2][2] * v0->vz) >> GTE_SHIFT;

    /* Add background color */
    v1->vx += (gte.color.t[0] >> 4);
    v1->vy += (gte.color.t[1] >> 4);
    v1->vz += (gte.color.t[2] >> 4);
}

/* Normal color calculation */
void NormalColor(SVECTOR *v0, CVECTOR *v1) {
    VECTOR light, color;

    if (!v0 || !v1) return;

    /* Apply light matrix to normal */
    LocalLight(v0, &light);

    /* Apply color matrix */
    LightColor(&light, &color);

    /* Clamp to 0-255 */
    v1->r = (u8)clamp8u(color.vx >> 4);
    v1->g = (u8)clamp8u(color.vy >> 4);
    v1->b = (u8)clamp8u(color.vz >> 4);
    v1->cd = gte.rgbc.cd;
}

void NormalColor3(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2,
                  CVECTOR *c0, CVECTOR *c1, CVECTOR *c2) {
    NormalColor(v0, c0);
    NormalColor(v1, c1);
    NormalColor(v2, c2);
}

/* Depth cueing */
void DpqColor(CVECTOR *v0, s32 p, CVECTOR *v1) {
    if (!v0 || !v1) return;

    /* Interpolate between vertex color and far color based on depth */
    s32 inv_p = GTE_ONE - p;
    if (inv_p < 0) inv_p = 0;
    if (inv_p > GTE_ONE) inv_p = GTE_ONE;

    v1->r = (u8)clamp8u(((s32)v0->r * inv_p + (s32)(gte.light.t[0] >> 4) * p) >> GTE_SHIFT);
    v1->g = (u8)clamp8u(((s32)v0->g * inv_p + (s32)(gte.light.t[1] >> 4) * p) >> GTE_SHIFT);
    v1->b = (u8)clamp8u(((s32)v0->b * inv_p + (s32)(gte.light.t[2] >> 4) * p) >> GTE_SHIFT);
    v1->cd = v0->cd;
}

/* Interpolation */
void Intpl(VECTOR *v0, s32 p, CVECTOR *v1) {
    if (!v0 || !v1) return;

    s32 inv_p = GTE_ONE - p;

    v1->r = (u8)clamp8u((v0->vx * inv_p + (gte.light.t[0] >> 4) * p) >> GTE_SHIFT);
    v1->g = (u8)clamp8u((v0->vy * inv_p + (gte.light.t[1] >> 4) * p) >> GTE_SHIFT);
    v1->b = (u8)clamp8u((v0->vz * inv_p + (gte.light.t[2] >> 4) * p) >> GTE_SHIFT);
}

/* Load average (linear interpolation between two vectors) */
void LoadAverage12(VECTOR *v0, VECTOR *v1, s32 p0, s32 p1, VECTOR *v2) {
    if (!v0 || !v1 || !v2) return;

    s32 total = p0 + p1;
    if (total == 0) total = 1;

    v2->vx = ((s64)v0->vx * p0 + (s64)v1->vx * p1) / total;
    v2->vy = ((s64)v0->vy * p0 + (s64)v1->vy * p1) / total;
    v2->vz = ((s64)v0->vz * p0 + (s64)v1->vz * p1) / total;
}

void LoadAverageShort12(SVECTOR *v0, SVECTOR *v1, s32 p0, s32 p1, SVECTOR *v2) {
    VECTOR lv0 = {v0->vx, v0->vy, v0->vz};
    VECTOR lv1 = {v1->vx, v1->vy, v1->vz};
    VECTOR lv2;

    LoadAverage12(&lv0, &lv1, p0, p1, &lv2);

    v2->vx = (s16)clamp16(lv2.vx);
    v2->vy = (s16)clamp16(lv2.vy);
    v2->vz = (s16)clamp16(lv2.vz);
}

/* OTZ to/from projection conversions */
s32 otz2p(s32 otz, s32 projection) {
    if (otz <= 0) return 0;
    return (projection * GTE_ONE) / otz;
}

s32 p2otz(s32 p, s32 projection) {
    if (p <= 0) return 0;
    return (projection * GTE_ONE) / p;
}

/* Read light matrix */
void ReadLightMatrix(MATRIX *m) {
    if (!m) return;
    memcpy(m->m, gte.light.m, sizeof(m->m));
    m->t[0] = gte.light.t[0];
    m->t[1] = gte.light.t[1];
    m->t[2] = gte.light.t[2];
}

/* Read color matrix: retail (asm/main/matched/ReadColorMatrix.s) reads
 * LCM (cr16..20) and then the FAR colour (cr21..23) into m->t. */
void ReadColorMatrix(MATRIX *m) {
    if (!m) return;
    memcpy(m->m, gte.color.m, sizeof(m->m));
    m->t[0] = gte.light.t[0];
    m->t[1] = gte.light.t[1];
    m->t[2] = gte.light.t[2];
}

/* csin/ccos - cached sin/cos using lookup table (faster on PS1) */
s32 csin(s32 a) { return rsin(a); }
s32 ccos(s32 a) { return rcos(a); }

/* gte_dump_regs (hal_gte_cop2.h): the register file a retail save state
 * holds.  Only what the model keeps is filled; the rest stays 0. */
static u32 gte_pack16(s32 lo, s32 hi) {
    return ((u32)lo & 0xFFFF) | ((u32)hi << 16);
}

static void gte_dump_matrix(u32 *out, const MATRIX *m) {
    out[0] = gte_pack16(m->m[0][0], m->m[0][1]);
    out[1] = gte_pack16(m->m[0][2], m->m[1][0]);
    out[2] = gte_pack16(m->m[1][1], m->m[1][2]);
    out[3] = gte_pack16(m->m[2][0], m->m[2][1]);
    out[4] = (u32)m->m[2][2] & 0xFFFF;
}

void gte_dump_regs(u32 out[64]) {
    u32 *c = out + 32;
    int i;
    for (i = 0; i < 64; i++) out[i] = 0;
    out[0] = gte_pack16(gte.v[0].vx, gte.v[0].vy);
    out[1] = (u32)gte.v[0].vz & 0xFFFF;
    out[6] = g_rgbc;
    out[8] = (u32)gte.ir0;
    out[9] = (u32)gte.ir1;
    out[10] = (u32)gte.ir2;
    out[11] = (u32)gte.ir3;
    out[16] = (u32)gte.sz0;
    out[17] = (u32)gte.sz1;
    out[18] = (u32)gte.sz2;
    out[19] = (u32)gte.sz3;
    out[20] = g_rgb_fifo[0];
    out[21] = g_rgb_fifo[1];
    out[22] = g_rgb_fifo[2];
    out[24] = (u32)gte.mac0;
    out[25] = (u32)gte.mac1;
    out[26] = (u32)gte.mac2;
    out[27] = (u32)gte.mac3;
    gte_dump_matrix(c + 0, &gte.rot);
    c[5] = (u32)gte.trans.vx;
    c[6] = (u32)gte.trans.vy;
    c[7] = (u32)gte.trans.vz;
    gte_dump_matrix(c + 8, &gte.light);
    for (i = 0; i < 3; i++) c[13 + i] = (u32)gte.color.t[i];
    gte_dump_matrix(c + 16, &gte.color);
    for (i = 0; i < 3; i++) c[21 + i] = (u32)gte.light.t[i];
    c[24] = (u32)gte.ofx;
    c[25] = (u32)gte.ofy;
    c[26] = (u32)gte.h & 0xFFFF;
    c[27] = (u32)gte.dqa & 0xFFFF;
    c[28] = (u32)gte.dqb;
    c[29] = (u32)gte.zsf3 & 0xFFFF;
    c[30] = (u32)gte.zsf4 & 0xFFFF;
    c[31] = gte.flag;
}
