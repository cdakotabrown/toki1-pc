/*
 * GsSortObject4 -- PC pointer-width adapter for matched/GsSortObject4.c.
 * Statement-for-statement matched body; only host pointer width diverges.
 * PS1 object and TMD pointer fields are u32 handles at console offsets;
 * dereferences zero-extend them rather than reading fused native pointers.
 * GsOUT_PACKET_P and the lighting dispatch cell remain four-byte globals.
 * See mv_state.h. No rendering/dispatch logic or call order is changed.
 */
#include "types.h"
#ifdef PC_PORT
#include "sony_bios_alias.h"   /* printf -> the BIOS A(0x3F) trampoline */
#endif

/* ---- port-only GS4 accounting (GS4_STATS) -----------------------------
 * The title (scene 18) walked 257 OT nodes and produced one primitive.  These
 * counters say whether GsSortObject4 was asked to draw and what it emitted;
 * the printout lives in the HAL (OT_TRACE), so this file stays I/O-free and
 * still links into the unit-test harness.  Zero cost when unset. */
extern char *getenv(const char *);

static int g_gs4_trace = -1;
static unsigned long g_gs4_calls, g_gs4_negative, g_gs4_zero_prim,
                     g_gs4_field_nonzero, g_gs4_prim_runs, g_gs4_unsupported;

int gs4_trace_on(void)
{
    if (g_gs4_trace < 0) {
        const char *e = getenv("GS4_TRACE");
        g_gs4_trace = (e && *e && *e != '0') ? 1 : 0;
    }
    return g_gs4_trace;
}

void gs4_trace_call(s32 attribute, s32 stage, s32 value)
{
    (void)attribute;
    switch (stage) {
    case 0: g_gs4_calls++; break;
    case 1: g_gs4_negative++; break;
    case 2: if (value == 0) g_gs4_zero_prim++; break;
    case 3: g_gs4_field_nonzero++; break;
    case 4: g_gs4_prim_runs++; break;
    case 5: g_gs4_unsupported++; break;
    }
}

void gs4_stats(unsigned long *out)
{
    out[0] = g_gs4_calls;
    out[1] = g_gs4_negative;
    out[2] = g_gs4_zero_prim;
    out[3] = g_gs4_field_nonzero;
    out[4] = g_gs4_prim_runs;
    out[5] = g_gs4_unsupported;
}

/* Per-TMD-primitive-type histogram.  Which switch arm fires decides which
 * GTE builder the port actually has to implement, so the type census is the
 * work list.  Indexed by the raw code byte; only counted when GS4_TRACE is on
 * (the dump lives in the HAL, so this file stays I/O-free). */
static unsigned long g_gs4_type_hist[256];

void gs4_type_hit(u32 code)
{
    g_gs4_type_hist[code & 0xFFu]++;
}

void gs4_type_stats(unsigned long *out)
{
    s32 i;
    for (i = 0; i < 256; i++) out[i] = g_gs4_type_hist[i];
}

extern s32 GsLMODE;
extern s32 GsLIGNR;
extern s32 GsLIOFF;
extern s32 GsLIGHT_MODE;

/* Builder census (GS4_TRACE): which GsSortObject4 path (fast / div), which
 * TMD code and which lighting arm the TmdXX dispatchers will take.  The
 * light class repeats TmdF3's own test (GsLIOFF -> NL; GsLIGNR/GsLIGHT_MODE/
 * GsLMODE -> LFG; else L) on the values this call just stored, so the census
 * names the builder without touching the dispatchers.  Unlit codes (bit 0)
 * are filed under class 3. */
static unsigned long g_gs4_census_runs[2][256][4];
static unsigned long g_gs4_census_prims[2][256][4];

static void gs4_census(s32 div, s32 code, u32 count)
{
    s32 cls;
    if (code & 1) {
        cls = 3;
    } else if (GsLIOFF == 1) {
        cls = 0;
    } else if ((GsLIGNR == 0 && GsLIGHT_MODE == 1) ||
               (GsLIGNR == 1 && GsLMODE == 1)) {
        cls = 1;
    } else {
        cls = 2;
    }
    g_gs4_census_runs[div ? 1 : 0][code & 0xFF][cls]++;
    g_gs4_census_prims[div ? 1 : 0][code & 0xFF][cls] += count;
}

/* [div][code][class] flattened, 2*256*4 entries each; class 0 = NL,
 * 1 = LFG, 2 = L, 3 = unlit (N*). */
void gs4_census_stats(unsigned long *out_runs, unsigned long *out_prims)
{
    s32 d, c, k;
    for (d = 0; d < 2; d++)
        for (c = 0; c < 256; c++)
            for (k = 0; k < 4; k++) {
                out_runs[(d * 256 + c) * 4 + k] = g_gs4_census_runs[d][c][k];
                out_prims[(d * 256 + c) * 4 + k] = g_gs4_census_prims[d][c][k];
            }
}

extern s32 GsNDIV;
extern s32 GsTON;
extern s32 GsOUT_PACKET_P;
extern s32 HWD0;
extern s32 VWD0;

/* Pattern 1 functions: func(prim, base, param, global, count, arg2, arg1, arg3) */
extern s32 TmdF3(u8*, u8*, u8*, s32, u16, s32, s32, s32);
extern s32 TmdTF3(u8*, u8*, u8*, s32, u16, s32, s32, s32);
extern s32 TmdF4(u8*, u8*, u8*, s32, u16, s32, s32, s32);
extern s32 TmdTF4();
extern s32 TmdG3(u8*, u8*, u8*, s32, u16, s32, s32, s32);
extern s32 TmdTG3(u8*, u8*, u8*, s32, u16, s32, s32, s32);
extern s32 TmdG4();
extern s32 TmdTG4();

/* Pattern 2 (7-arg): func(prim, base, global, count, arg2, arg1, arg3) */
extern s32 GsTMDfastNF3(u8*, u8*, s32, u16, s32, s32, s32);
extern s32 GsTMDfastNF4(u8*, u8*, s32, u16, s32, s32, s32);
extern s32 GsTMDdivNF3(u8*, u8*, s32, u16, s32, s32, s32);
extern s32 GsTMDdivNF4(u8*, u8*, s32, u16, s32, s32, s32);

/* Pattern 2A (6-arg, switch 1 only): func(prim, base, global, count, arg2, arg1) */
extern s32 GsTMDfastNG3(u8*, u8*, s32, u16, s32, s32);
extern s32 GsTMDfastTNF3(u8*, u8*, s32, u16, s32, s32);
extern s32 GsTMDfastTNG3(u8*, u8*, s32, u16, s32, s32);
extern s32 GsTMDfastNG4(u8*, u8*, s32, u16, s32, s32);
extern s32 GsTMDfastTNF4(u8*, u8*, s32, u16, s32, s32);
extern s32 GsTMDfastTNG4(u8*, u8*, s32, u16, s32, s32);

/* Pattern 2B (7-arg, switch 2): func(prim, base, global, count, arg2, arg1, arg3) */
extern s32 GsTMDdivTNF3(u8*, u8*, s32, u16, s32, s32, s32);
extern s32 GsTMDdivNG3(u8*, u8*, s32, u16, s32, s32, s32);
extern s32 GsTMDdivTNG3(u8*, u8*, s32, u16, s32, s32, s32);
extern s32 GsTMDdivNG4(u8*, u8*, s32, u16, s32, s32, s32);
extern s32 GsTMDdivTNF4(u8*, u8*, s32, u16, s32, s32, s32);
extern s32 GsTMDdivTNG4(u8*, u8*, s32, u16, s32, s32, s32);

extern s32 printf(const char *format, ...);

typedef struct {
    u32 attribute;
    u32 coord2;
    u32 tmd;
    u32 id;
} GsDOBJ2;

void GsSortObject4(GsDOBJ2 *arg0, s32 arg1, s32 arg2, s32 *arg3) {
    u32 val;
    u32 *header;
    u8 *curPrim;
    u32 primCount;
    u8 *base;
    u8 *param;
    s32 maskedType;
    u16 count;
    s32 field;
    /* Port-only diagnostics (GS4_TRACE=1): the title's draw list came out
     * empty with 257 OT nodes, so count every early exit and every packet
     * run this adapter actually emits.  No effect when GS4_TRACE is unset. */
    extern int gs4_trace_on(void);
    extern void gs4_trace_call(s32 attribute, s32 stage, s32 value);
    s32 trace = gs4_trace_on();

    GsLMODE = (arg0->attribute >> 3) & 3;
    GsLIGNR = (arg0->attribute >> 5) & 1;
    GsLIOFF = (arg0->attribute >> 6) & 1;
    field = (arg0->attribute >> 9) & 7;
    GsNDIV = field;
    GsTON = (arg0->attribute >> 30) & 1;

    if ((s32)arg0->attribute < 0) {
        if (trace) gs4_trace_call((s32)arg0->attribute, 1, 0);
        return;
    }

    header = (u32 *)(unsigned long)arg0->tmd;
    curPrim = (u8 *)(unsigned long)header[4];
    primCount = (u32)header[5];
    base = (u8 *)(unsigned long)header[0];
    param = (u8 *)(unsigned long)header[2];
    if (trace) gs4_trace_call((s32)arg0->attribute, 2, (s32)primCount);

    if (field != 0) {
        if (trace) gs4_trace_call((s32)arg0->attribute, 3, field);
    }
    if (trace) gs4_trace_call((s32)arg0->attribute, 0, 0);

    if (field == 0) {
        if (primCount == 0) {
            return;
        }
        do {
            maskedType = curPrim[3] & 0xFD;
            if (trace) {
                gs4_type_hit(curPrim[3]);
                gs4_census(field != 0, maskedType, *(u16 *)curPrim);
            }
            switch (maskedType) {
                case 0x20:
                    count = *(u16 *)curPrim;
                    GsOUT_PACKET_P = TmdF3(curPrim, base, param, GsOUT_PACKET_P, count, arg2, arg1, arg3);
                    count = *(u16 *)curPrim;
                    primCount -= count;
                    curPrim += count * 16;
                    break;
                case 0x24:
                    count = *(u16 *)curPrim;
                    GsOUT_PACKET_P = TmdTF3(curPrim, base, param, GsOUT_PACKET_P, count, arg2, arg1, arg3);
                    count = *(u16 *)curPrim;
                    primCount -= count;
                    curPrim += count * 24;
                    break;
                case 0x30:
                    count = *(u16 *)curPrim;
                    GsOUT_PACKET_P = TmdG3(curPrim, base, param, GsOUT_PACKET_P, count, arg2, arg1, arg3);
                    count = *(u16 *)curPrim;
                    primCount -= count;
                    curPrim += count * 20;
                    break;
                case 0x34:
                    count = *(u16 *)curPrim;
                    GsOUT_PACKET_P = TmdTG3(curPrim, base, param, GsOUT_PACKET_P, count, arg2, arg1, arg3);
                    count = *(u16 *)curPrim;
                    primCount -= count;
                    curPrim += count * 28;
                    break;
                case 0x28:
                    count = *(u16 *)curPrim;
                    GsOUT_PACKET_P = TmdF4(curPrim, base, param, GsOUT_PACKET_P, count, arg2, arg1, arg3);
                    count = *(u16 *)curPrim;
                    primCount -= count;
                    curPrim += count * 20;
                    break;
                case 0x21:
                    count = *(u16 *)curPrim;
                    GsOUT_PACKET_P = GsTMDfastNF3(curPrim, base, GsOUT_PACKET_P, count, arg2, arg1, arg3);
                    count = *(u16 *)curPrim;
                    primCount -= count;
                    curPrim += count * 16;
                    break;
                case 0x29:
                    count = *(u16 *)curPrim;
                    GsOUT_PACKET_P = GsTMDfastNF4(curPrim, base, GsOUT_PACKET_P, count, arg2, arg1, arg3);
                    count = *(u16 *)curPrim;
                    primCount -= count;
                    curPrim += count * 16;
                    break;
                case 0x2C:
                    count = *(u16 *)curPrim;
                    GsOUT_PACKET_P = TmdTF4(curPrim, base, param, GsOUT_PACKET_P, count, arg2, arg1);
                    count = *(u16 *)curPrim;
                    primCount -= count;
                    curPrim += count * 32;
                    break;
                case 0x38:
                    count = *(u16 *)curPrim;
                    GsOUT_PACKET_P = TmdG4(curPrim, base, param, GsOUT_PACKET_P, count, arg2, arg1);
                    count = *(u16 *)curPrim;
                    primCount -= count;
                    curPrim += count * 24;
                    break;
                case 0x3C:
                    count = *(u16 *)curPrim;
                    GsOUT_PACKET_P = TmdTG4(curPrim, base, param, GsOUT_PACKET_P, count, arg2, arg1);
                    count = *(u16 *)curPrim;
                    primCount -= count;
                    curPrim += count * 36;
                    break;
                case 0x31:
                    count = *(u16 *)curPrim;
                    GsOUT_PACKET_P = GsTMDfastNG3(curPrim, base, GsOUT_PACKET_P, count, arg2, arg1);
                    count = *(u16 *)curPrim;
                    primCount -= count;
                    curPrim += count * 24;
                    break;
                case 0x25:
                    count = *(u16 *)curPrim;
                    GsOUT_PACKET_P = GsTMDfastTNF3(curPrim, base, GsOUT_PACKET_P, count, arg2, arg1);
                    count = *(u16 *)curPrim;
                    primCount -= count;
                    curPrim += count * 28;
                    break;
                case 0x35:
                    count = *(u16 *)curPrim;
                    GsOUT_PACKET_P = GsTMDfastTNG3(curPrim, base, GsOUT_PACKET_P, count, arg2, arg1);
                    count = *(u16 *)curPrim;
                    primCount -= count;
                    curPrim += count * 36;
                    break;
                case 0x39:
                    count = *(u16 *)curPrim;
                    GsOUT_PACKET_P = GsTMDfastNG4(curPrim, base, GsOUT_PACKET_P, count, arg2, arg1);
                    count = *(u16 *)curPrim;
                    primCount -= count;
                    curPrim += count * 28;
                    break;
                case 0x2D:
                    count = *(u16 *)curPrim;
                    GsOUT_PACKET_P = GsTMDfastTNF4(curPrim, base, GsOUT_PACKET_P, count, arg2, arg1);
                    count = *(u16 *)curPrim;
                    primCount -= count;
                    curPrim += count * 32;
                    break;
                case 0x3D:
                    count = *(u16 *)curPrim;
                    GsOUT_PACKET_P = GsTMDfastTNG4(curPrim, base, GsOUT_PACKET_P, count, arg2, arg1);
                    count = *(u16 *)curPrim;
                    primCount -= count;
                    curPrim += count * 44;
                    break;
                default:
                    printf("non supported code %x %x\n", maskedType, curPrim);
                    break;
            }
        } while (primCount != 0);
    } else {
        {
            s32 t1 = HWD0;
            s32 t2 = VWD0;
            arg3[0] = field;
            arg3[1] = t1;
            arg3[2] = t2;
        }

        if (primCount == 0) {
            return;
        }
        do {
            maskedType = curPrim[3] & 0xFD;
            if (trace) {
                gs4_type_hit(curPrim[3]);
                gs4_census(field != 0, maskedType, *(u16 *)curPrim);
            }
            switch (maskedType) {
                case 0x20:
                    count = *(u16 *)curPrim;
                    GsOUT_PACKET_P = TmdF3(curPrim, base, param, GsOUT_PACKET_P, count, arg2, arg1, arg3);
                    count = *(u16 *)curPrim;
                    primCount -= count;
                    curPrim += count * 16;
                    break;
                case 0x24:
                    count = *(u16 *)curPrim;
                    GsOUT_PACKET_P = TmdTF3(curPrim, base, param, GsOUT_PACKET_P, count, arg2, arg1, arg3);
                    count = *(u16 *)curPrim;
                    primCount -= count;
                    curPrim += count * 24;
                    break;
                case 0x30:
                    count = *(u16 *)curPrim;
                    GsOUT_PACKET_P = TmdG3(curPrim, base, param, GsOUT_PACKET_P, count, arg2, arg1, arg3);
                    count = *(u16 *)curPrim;
                    primCount -= count;
                    curPrim += count * 20;
                    break;
                case 0x34:
                    count = *(u16 *)curPrim;
                    GsOUT_PACKET_P = TmdTG3(curPrim, base, param, GsOUT_PACKET_P, count, arg2, arg1, arg3);
                    count = *(u16 *)curPrim;
                    primCount -= count;
                    curPrim += count * 28;
                    break;
                case 0x28:
                    count = *(u16 *)curPrim;
                    GsOUT_PACKET_P = TmdF4(curPrim, base, param, GsOUT_PACKET_P, count, arg2, arg1, arg3);
                    count = *(u16 *)curPrim;
                    primCount -= count;
                    curPrim += count * 20;
                    break;
                case 0x21:
                    count = *(u16 *)curPrim;
                    GsOUT_PACKET_P = GsTMDdivNF3(curPrim, base, GsOUT_PACKET_P, count, arg2, arg1, arg3);
                    count = *(u16 *)curPrim;
                    primCount -= count;
                    curPrim += count * 16;
                    break;
                case 0x29:
                    count = *(u16 *)curPrim;
                    GsOUT_PACKET_P = GsTMDdivNF4(curPrim, base, GsOUT_PACKET_P, count, arg2, arg1, arg3);
                    count = *(u16 *)curPrim;
                    primCount -= count;
                    curPrim += count * 16;
                    break;
                case 0x2C:
                    count = *(u16 *)curPrim;
                    GsOUT_PACKET_P = TmdTF4(curPrim, base, param, GsOUT_PACKET_P, count, arg2, arg1, arg3);
                    count = *(u16 *)curPrim;
                    primCount -= count;
                    curPrim += count * 32;
                    break;
                case 0x38:
                    count = *(u16 *)curPrim;
                    GsOUT_PACKET_P = TmdG4(curPrim, base, param, GsOUT_PACKET_P, count, arg2, arg1, arg3);
                    count = *(u16 *)curPrim;
                    primCount -= count;
                    curPrim += count * 24;
                    break;
                case 0x3C:
                    count = *(u16 *)curPrim;
                    GsOUT_PACKET_P = TmdTG4(curPrim, base, param, GsOUT_PACKET_P, count, arg2, arg1, arg3);
                    count = *(u16 *)curPrim;
                    primCount -= count;
                    curPrim += count * 36;
                    break;
                case 0x31:
                    count = *(u16 *)curPrim;
                    GsOUT_PACKET_P = GsTMDdivNG3(curPrim, base, GsOUT_PACKET_P, count, arg2, arg1, arg3);
                    count = *(u16 *)curPrim;
                    primCount -= count;
                    curPrim += count * 24;
                    break;
                case 0x25:
                    count = *(u16 *)curPrim;
                    GsOUT_PACKET_P = GsTMDdivTNF3(curPrim, base, GsOUT_PACKET_P, count, arg2, arg1, arg3);
                    count = *(u16 *)curPrim;
                    primCount -= count;
                    curPrim += count * 28;
                    break;
                case 0x35:
                    count = *(u16 *)curPrim;
                    GsOUT_PACKET_P = GsTMDdivTNG3(curPrim, base, GsOUT_PACKET_P, count, arg2, arg1, arg3);
                    count = *(u16 *)curPrim;
                    primCount -= count;
                    curPrim += count * 36;
                    break;
                case 0x39:
                    count = *(u16 *)curPrim;
                    GsOUT_PACKET_P = GsTMDdivNG4(curPrim, base, GsOUT_PACKET_P, count, arg2, arg1, arg3);
                    count = *(u16 *)curPrim;
                    primCount -= count;
                    curPrim += count * 28;
                    break;
                case 0x2D:
                    count = *(u16 *)curPrim;
                    GsOUT_PACKET_P = GsTMDdivTNF4(curPrim, base, GsOUT_PACKET_P, count, arg2, arg1, arg3);
                    count = *(u16 *)curPrim;
                    primCount -= count;
                    curPrim += count * 32;
                    break;
                case 0x3D:
                    count = *(u16 *)curPrim;
                    GsOUT_PACKET_P = GsTMDdivTNG4(curPrim, base, GsOUT_PACKET_P, count, arg2, arg1, arg3);
                    count = *(u16 *)curPrim;
                    primCount -= count;
                    curPrim += count * 44;
                    break;
                default:
                    printf("non supported code %x %x\n", maskedType, curPrim);
                    break;
            }
        } while (primCount != 0);
    }
}
