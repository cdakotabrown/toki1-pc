/* prim_arg_fold.c -- linker wrappers that fold PS1 pointer ARGUMENTS for the
 * matched libgpu primitive layer.
 *
 * The matched setters/linker are called with pointers that matched code
 * materialises from `s32` cells (OT slot arrays, prim banks): on the console
 * that `lw` yields the address directly, on the host it SIGN-EXTENDS it
 * (0x800F1150 -> 0xFFFFFFFF800F1150) and the store faults in kernel space.
 * That is the src/main/pc-port/ mv_state.h class again: the pointer width is
 * a property of the host, not of the game.
 *
 * The fix here is deliberately NOT a body copy: ld --wrap redirects callers
 * to these __wrap_ shims, which fold the arguments with hal_ps1_ptr (the
 * sanctioned primitive -- it folds exactly the sign-extended PS1 range and
 * passes host pointers through untouched) and tail-call the REAL matched
 * function through __real_.  The PS1 code still runs, statement for
 * statement, byte for byte.
 *
 * Measured first-fail classes these cover (scene matrix 2026-10-19):
 * SetSprt+0x5, SetPolyG4+0x4, SetPolyGT4+0x4, SetTile1+0x4, AddPrim+0x6.
 * The whole one-arg prim-setter family is wrapped pre-emptively -- they are
 * the same 8-byte-load shape and each matrix run costs 25 minutes.
 *
 * New victim of the s32-pointer class?  Add its name to WRAP_SYMS in
 * pc/Makefile and a shim (WRAP1/WRAP2) here.
 */
#include "types.h"
#include "hal.h"

#define WRAP1(name)                                                  \
    extern void __real_##name(u32 *p);                               \
    void __wrap_##name(u32 *p)                                       \
    {                                                                \
        __real_##name((u32 *)hal_ps1_ptr(p));                        \
    }

WRAP1(SetLineF2)
WRAP1(SetLineF3)
WRAP1(SetLineF4)
WRAP1(SetLineG2)
WRAP1(SetLineG3)
WRAP1(SetLineG4)
WRAP1(SetPolyF3)
WRAP1(SetPolyF4)
WRAP1(SetPolyFT3)
WRAP1(SetPolyFT4)
WRAP1(SetPolyG3)
WRAP1(SetPolyG4)
WRAP1(SetPolyGT3)
WRAP1(SetPolyGT4)
WRAP1(SetSprt)
WRAP1(SetSprt8)
WRAP1(SetSprt16)
WRAP1(SetTile)
WRAP1(SetTile1)
WRAP1(SetTile8)
WRAP1(SetTile16)

extern void __real_AddPrim(u32 *ofs, u32 *p);

/* ---- port-only OT accounting (OT_TRACE=1) ----------------------------
 * Every primitive that reaches an ordering table goes through AddPrim or
 * AddPrims, so these two counters answer "does this scene build a draw
 * list at all?".  Scene 18 (title) walked 257 OT nodes and produced one
 * primitive while scene 64 produced 94, so the counters locate the gap.
 * Zero cost when OT_TRACE is unset. */
extern char *getenv(const char *);
extern void debug_log(const char *fmt, ...);
extern int atexit(void (*)(void));

static int g_ot_trace = -1;
static unsigned long g_ot_addprim, g_ot_addprims, g_ot_addprims_nodes;

static void ot_trace_dump(void)
{
    /* The 3D path's failure mode is visible in these five numbers: with the
     * GTE builders stubbed, GsOUT_PACKET_P fell to 0 after the first object
     * and AddPrim stopped growing.  tmd_* proves the model itself loaded
     * (0x41 = TMD v0x41, nobj = 13 for the title). */
    extern unsigned long g_ot_setaddr_calls;
    if (g_ot_trace != 1) return;
    extern void gs4_stats(unsigned long *);
    unsigned long gs4[6];
    gs4_stats(gs4);
    debug_log("[OT] AddPrim=%lu setaddr=%lu GsOUT_PACKET_P=%08x sub_a=%d "
              "sub_b=%d tmd_id=%08x tmd_nobj=%u",
              g_ot_addprim, g_ot_setaddr_calls,
              *(volatile u32 *)0x80131CACu,
              *(volatile u8 *)0x800EF872u, *(volatile u8 *)0x800EF873u,
              *(volatile u32 *)0x801F0000u, *(volatile u32 *)0x801F0008u);
    debug_log("[GS4] calls=%lu negative=%lu zero_prim=%lu field_nonzero=%lu "
              "prim_runs=%lu unsupported=%lu",
              gs4[0], gs4[1], gs4[2], gs4[3], gs4[4], gs4[5]);
    {
        /* TMD primitive-type census: the switch arm that fires names the GTE
         * builder the port must implement.  One line per observed code. */
        extern void gs4_type_stats(unsigned long *);
        static unsigned long types[256];
        s32 i;
        gs4_type_stats(types);
        for (i = 0; i < 256; i++) {
            if (types[i]) debug_log("[GS4T] code=%02x runs_prim=%lu", i, types[i]);
        }
    }
    {
        /* Builder census: path x masked code x light arm -> runs, prims.
         * One line per non-zero cell; the builder name follows from them
         * (GsTMD{fast,div}<code><NL|LFG|L>, unlit codes N*). */
        extern void gs4_census_stats(unsigned long *, unsigned long *);
        static unsigned long runs[2 * 256 * 4], prims[2 * 256 * 4];
        static const char *const cls[4] = { "NL", "LFG", "L", "unlit" };
        s32 i;
        gs4_census_stats(runs, prims);
        for (i = 0; i < 2 * 256 * 4; i++) {
            if (runs[i])
                debug_log("[TMDC] path=%s code=%02x light=%s runs=%lu prims=%lu",
                          (i / 1024) ? "div" : "fast", (i / 4) & 0xFF,
                          cls[i & 3], runs[i], prims[i]);
        }
    }
}

extern void ot_trace_tick(void);

static int ot_trace_on(void)
{
    if (g_ot_trace < 0) {
        const char *e = getenv("OT_TRACE");
        g_ot_trace = (e && *e && *e != '0') ? 1 : 0;
        if (g_ot_trace) atexit(ot_trace_dump);
    }
    return g_ot_trace;
}

/* Called once per frame by the HAL so the counters cannot be masked by a
 * shutdown-order quirk: prints every 120 calls too. */
void ot_trace_tick(void)
{
    static unsigned long ticks;
    if (!ot_trace_on()) return;
    if ((++ticks % 120) == 0) ot_trace_dump();
}

void __wrap_AddPrim(u32 *ofs, u32 *p)
{
    ot_trace_on();
    g_ot_addprim++;
    __real_AddPrim((u32 *)hal_ps1_ptr(ofs), (u32 *)hal_ps1_ptr(p));
}

extern void __real_AddPrims(u32 *ofs, u32 *p0, u32 *p1);

void __wrap_AddPrims(u32 *ofs, u32 *p0, u32 *p1)
{
    ot_trace_on();
    g_ot_addprims++;
    __real_AddPrims((u32 *)hal_ps1_ptr(ofs), (u32 *)hal_ps1_ptr(p0),
                    (u32 *)hal_ps1_ptr(p1));
}

/* The two flag setters take the same primitive handle plus a scalar flag.
 * Scene 19 first faults in SetSemiTrans with 0xffffffff80105980. */
#define WRAP_FLAG(name)                                              \
    extern void __real_##name(u8 *p, s32 flag);                       \
    void __wrap_##name(u8 *p, s32 flag)                               \
    {                                                               \
        __real_##name((u8 *)hal_ps1_ptr(p), flag);                    \
    }

WRAP_FLAG(SetSemiTrans)
WRAP_FLAG(SetShadeTex)
