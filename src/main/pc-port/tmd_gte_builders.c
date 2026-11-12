/*
 * tmd_gte_builders.c -- the GTE half of the port's 3D path.
 *
 * The `GsTMDfast*` TMD packet builders are terminal asm_only Sony LIBGTE
 * TMDF_* members (config/c_unmatchable.txt): COP2 sequences that C89 cannot
 * emit.  The port supplies them here.  Each body replays its retail listing
 * (asm/main/matched/GsTMDfast<X>.s) command for command through the GTE
 * model's COP2 interface (pc/include/hal_gte_cop2.h): the same vertex loads,
 * RTPT/RTPS, the FLAG bit-31 and NCLIP tests, AVSZ3/AVSZ4, the same packet
 * words written in the same order -- including the words retail stores into
 * the packet BEFORE it decides to drop the primitive -- and the same cursor
 * rule: a dropped primitive does not advance the packet pointer.
 *
 * Implemented (unlit, "N" = TMD no-light flag; dispatched by GsSortObject4):
 *
 *   code  prim  packet  builder            retail entry
 *   0x21   16    5 w    GsTMDfastNF3       0x800A64B0 (TMDF_F3)
 *   0x29   16    6 w    GsTMDfastNF4       0x800A69C0 (TMDF_F4)
 *   0x31   24    7 w    GsTMDfastNG3       0x800A6F90 (TMDF_G3)
 *   0x25   28    8 w    GsTMDfastTNF3      0x800A7540 (TMDF_FT3)
 *   0x35   36   10 w    GsTMDfastTNG3      0x800A7AF0 (TMDF_GT3)
 *   0x39   28    9 w    GsTMDfastNG4       0x800A82E0 (TMDF_G4)
 *   0x2D   32   10 w    GsTMDfastTNF4      0x800A89A0 (TMDF_FT4)
 *   0x3D   44   13 w    GsTMDfastTNG4      0x800A9040 (TMDF_GT4)
 *
 * Prim layouts (Sony TMD format, no-light primitives; +0 is the olen/ilen/
 * flag/mode header, whose low half GsLinkObject4 overwrites with the run
 * count and whose top byte is the mode/code):
 *
 *   NF3   +4 rgb|mode            +8 v0|v1  +C v2
 *   NF4   +4 rgb|mode            +8 v0|v1  +C v2|v3
 *   NG3   +4 rgb0|mode +8 rgb1 +C rgb2              +10 v0|v1 +14 v2
 *   NG4   +4 rgb0|mode +8 rgb1 +C rgb2 +10 rgb3     +14 v0|v1 +18 v2|v3
 *   TNF3  +4 uv0|cba +8 uv1|tsb +C uv2  +10 rgb     +14 v0|v1 +18 v2
 *   TNF4  +4 uv0|cba +8 uv1|tsb +C uv2 +10 uv3 +14 rgb  +18 v0|v1 +1C v2|v3
 *   TNG3  +4 uv0|cba +8 uv1|tsb +C uv2 +10 rgb0 +14 rgb1 +18 rgb2
 *                                                   +1C v0|v1 +20 v2
 *   TNG4  +4..+10 uv0..uv3  +14..+20 rgb0..rgb3     +24 v0|v1 +28 v2|v3
 *
 * Vertex n is the SVECTOR at vtx + n * 8.  Untextured packets take the
 * colour word as-is with GsTON << 25 (semi-transparency, GPU bit 1) OR'd in;
 * textured packets build the command as ((code | GsTON << 1) >> 1) << 25 so
 * the TMD's raw-texture bit 0 never reaches the GPU command.  The second
 * and later colour words of a Gouraud packet are copied raw (pad byte too).
 *
 * Lit: F3L / TF3L / TG3L, the only lit arms the game was measured to reach
 * (TACO); see the section at the end.  The unreached lit builders (LFG, NL,
 * F4/G3/G4/TF4/TG4 L) stay cursor pass-throughs in pc/src/missing_funcs.c.
 */
#include "types.h"
#include "hal.h"
#include "hal_gte_cop2.h"

/* Semi-transparency, stored by GsSortObject/2/4 from the object attribute
 * (bit 30). */
extern s32 GsTON;

/* ---- TMD_TRACE --------------------------------------------------------
 * Prints the first dozen OT links (bucket, OTZ) and then stays silent.
 * Zero cost when unset. */
extern char *getenv(const char *);
extern void debug_log(const char *fmt, ...);

static s32 tmd_trace_on(void)
{
    static s32 on = -1;
    if (on < 0) {
        const char *e = getenv("TMD_TRACE");
        on = (e && *e && *e != '0');
    }
    return on;
}

/* ---- shared pieces ---------------------------------------------------- */

#define TMD_PTR(a) ((u8 *)hal_ps1_ptr((const void *)(unsigned long)(a)))

static u32 tmd_word(const u8 *p, u32 off)
{
    return *(const u32 *)(p + off);
}

static const SVECTOR *tmd_vertex(const u8 *base, u32 index)
{
    return (const SVECTOR *)(base + (index & 0xFFFFu) * 8u);
}

/* lw (pair); lhu (single): V0 = pair.lo, V1 = pair.hi, V2 = single. */
static void tmd_load3(const u8 *base, const u8 *prim, u32 pair_off,
                      u32 third_off)
{
    u32 pair = tmd_word(prim, pair_off);
    gte_cop2_ldv(0, tmd_vertex(base, pair));
    gte_cop2_ldv(1, tmd_vertex(base, pair >> 16));
    gte_cop2_ldv(2, tmd_vertex(base, *(const u16 *)(prim + third_off)));
}

static void tmd_load_v3(const u8 *base, const u8 *prim, u32 off)
{
    gte_cop2_ldv(0, tmd_vertex(base, *(const u16 *)(prim + off)));
}

/* Untextured: colour word | GsTON << 25.  Textured: GPU command from the
 * mode byte (sign-extended `lb`, then a logical shift) over the 24-bit
 * colour. */
static u32 tmd_flat_colour(const u8 *prim)
{
    return tmd_word(prim, 4) | ((u32)GsTON << 25);
}

static u32 tmd_tex_command(const u8 *prim, u32 rgb_off)
{
    u32 code = (u32)(s32)(s8)prim[3] | ((u32)GsTON << 1);
    return (tmd_word(prim, rgb_off) & 0x00FFFFFFu) | ((code >> 1) << 25);
}

static s32 tmd_flag_error(void)
{
    return (gte_cop2_flag() & 0x80000000u) != 0;
}

/*
 * Link `packet` (a `len`+1 word node) as the new head of the OT bucket that
 * `otz` selects.  Retail, repeated in every builder:
 *   org    = *(u32 *)(ot + 4)      GsOT.org     -- bucket array
 *   offset = *(u32 *)(ot + 8)      GsOT.offset
 *   slot   = org + ((((u32)(otz - offset)) >> shift) & 0xFFFF) * 4
 *   packet[0] = (len << 24) | (*slot & 0x00FFFFFF)
 *   *slot     = (*slot & 0xFF000000) | (packet & 0x00FFFFFF)
 * The OT word keeps its top byte; only the 24-bit address is replaced.  The
 * port's DrawOTag resolves that back with 0x80000000 | addr (pc/hal/gpu.c).
 */
static void tmd_ot_link(s32 ot, s32 shift, s32 otz, u32 *packet, u32 len)
{
    u32 *otp = (u32 *)TMD_PTR((u32)ot);
    u32 *org = (u32 *)(unsigned long)otp[1];
    u32 offset = otp[2];
    u32 *slot;
    u32 old;
    u32 bucket = (((u32)otz - offset) >> (shift & 31)) & 0xFFFFu;

    slot = (u32 *)(unsigned long)((u32)(unsigned long)org + bucket * 4u);
    old = *slot;
    packet[0] = (len << 24) | (old & 0x00FFFFFFu);
    *slot = (old & 0xFF000000u) | ((u32)(unsigned long)packet & 0x00FFFFFFu);
    if (tmd_trace_on()) {
        static u32 shown;
        if (shown < 12) {
            shown++;
            debug_log("[TMDL] pkt=%08x len=%u otz=%d shift=%d org=%08x "
                      "off=%08x bucket=%u old=%08x tag=%08x",
                      (u32)(unsigned long)packet, len, otz, shift,
                      (u32)(unsigned long)org, offset, bucket, old,
                      packet[0]);
        }
    }
}

/* The cursor is a 32-bit PS1 address on both sides of the call. */
#define TMD_BEGIN(ap, base, out, prim, vtx, packet)                    \
    u8 *ap = TMD_PTR(prim);                                             \
    u8 *base = TMD_PTR(vtx);                                            \
    u32 *out = (u32 *)TMD_PTR((u32)(packet))

#define TMD_END(out) return (s32)(u32)(unsigned long)(out)

/* ---- code 0x21: 3-vertex flat (GsTMDfastNF3) --------------------------
 * RTPT; FLAG; NCLIP; AVSZ3.  Packet: tag(4) colour sxy0 sxy1 sxy2. */
s32 GsTMDfastNF3(u8 *prim, u8 *vtx, s32 packet, u16 count, s32 shift,
                 s32 ot, s32 arg3)
{
    TMD_BEGIN(ap, base, out, prim, vtx, packet);
    u32 colour;
    s32 otz;

    (void)arg3;
    for (; count != 0; count--, ap += 16) {
        tmd_load3(base, ap, 8, 0xC);
        gte_cop2_rtpt();
        colour = tmd_flat_colour(ap);
        if (tmd_flag_error() || gte_cop2_nclip() <= 0) continue;
        otz = gte_cop2_avsz3();
        tmd_ot_link(ot, shift, otz, out, 4);
        out[1] = colour;
        out[2] = gte_cop2_sxy(0);
        out[3] = gte_cop2_sxy(1);
        out[4] = gte_cop2_sxy(2);
        out += 5;
    }
    TMD_END(out);
}

/* ---- code 0x29: 4-vertex flat (GsTMDfastNF4) --------------------------
 * RTPT; FLAG; NCLIP; RTPS(v3); FLAG; AVSZ4.
 * Packet: tag(5) colour sxy0 sxy1 sxy2 sxy3. */
s32 GsTMDfastNF4(u8 *prim, u8 *vtx, s32 packet, u16 count, s32 shift,
                 s32 ot, s32 arg3)
{
    TMD_BEGIN(ap, base, out, prim, vtx, packet);
    u32 colour;
    s32 otz;

    (void)arg3;
    for (; count != 0; count--, ap += 16) {
        tmd_load3(base, ap, 8, 0xC);
        gte_cop2_rtpt();
        colour = tmd_flat_colour(ap);
        if (tmd_flag_error() || gte_cop2_nclip() <= 0) continue;
        tmd_load_v3(base, ap, 0xE);
        out[1] = colour;
        out[2] = gte_cop2_sxy(0);
        out[3] = gte_cop2_sxy(1);
        out[4] = gte_cop2_sxy(2);
        gte_cop2_rtps();
        if (tmd_flag_error()) continue;
        otz = gte_cop2_avsz4();
        out[5] = gte_cop2_sxy(2);
        tmd_ot_link(ot, shift, otz, out, 5);
        out += 6;
    }
    TMD_END(out);
}

/* ---- code 0x31: 3-vertex Gouraud (GsTMDfastNG3) -----------------------
 * Packet: tag(6) rgb0|cmd sxy0 rgb1 sxy1 rgb2 sxy2. */
s32 GsTMDfastNG3(u8 *prim, u8 *vtx, s32 packet, u16 count, s32 shift,
                 s32 ot)
{
    TMD_BEGIN(ap, base, out, prim, vtx, packet);
    u32 colour;
    s32 otz;

    for (; count != 0; count--, ap += 24) {
        tmd_load3(base, ap, 0x10, 0x14);
        gte_cop2_rtpt();
        colour = tmd_flat_colour(ap);
        if (tmd_flag_error() || gte_cop2_nclip() <= 0) continue;
        otz = gte_cop2_avsz3();
        out[1] = colour;
        out[3] = tmd_word(ap, 8);
        out[5] = tmd_word(ap, 0xC);
        out[2] = gte_cop2_sxy(0);
        out[4] = gte_cop2_sxy(1);
        out[6] = gte_cop2_sxy(2);
        tmd_ot_link(ot, shift, otz, out, 6);
        out += 7;
    }
    TMD_END(out);
}

/* ---- code 0x25: 3-vertex flat textured (GsTMDfastTNF3) ----------------
 * The UVs are stored before the FLAG/NCLIP tests (as retail does).
 * Packet: tag(7) cmd|rgb sxy0 uv0 sxy1 uv1 sxy2 uv2. */
s32 GsTMDfastTNF3(u8 *prim, u8 *vtx, s32 packet, u16 count, s32 shift,
                  s32 ot)
{
    TMD_BEGIN(ap, base, out, prim, vtx, packet);
    u32 command;
    s32 otz;

    for (; count != 0; count--, ap += 28) {
        tmd_load3(base, ap, 0x14, 0x18);
        gte_cop2_rtpt();
        command = tmd_tex_command(ap, 0x10);
        out[3] = tmd_word(ap, 4);
        out[5] = tmd_word(ap, 8);
        out[7] = tmd_word(ap, 0xC);
        if (tmd_flag_error() || gte_cop2_nclip() <= 0) continue;
        otz = gte_cop2_avsz3();
        tmd_ot_link(ot, shift, otz, out, 7);
        out[1] = command;
        out[2] = gte_cop2_sxy(0);
        out[4] = gte_cop2_sxy(1);
        out[6] = gte_cop2_sxy(2);
        out += 8;
    }
    TMD_END(out);
}

/* ---- code 0x35: 3-vertex Gouraud textured (GsTMDfastTNG3) -------------
 * Retail 0x800A7AF0: the one GT3 member that is not a 2.5 LIBGTE twin
 * (3.3 TMDF_GT3 +0x0).  Until 2026-10-30 the port's version was a
 * four-argument cursor return; GsSortObject4 passes six.
 * Packet: tag(9) cmd|rgb0 sxy0 uv0 rgb1 sxy1 uv1 rgb2 sxy2 uv2. */
s32 GsTMDfastTNG3(u8 *prim, u8 *vtx, s32 packet, u16 count, s32 shift,
                  s32 ot)
{
    TMD_BEGIN(ap, base, out, prim, vtx, packet);
    u32 c0, c1, c2;
    s32 otz;

    for (; count != 0; count--, ap += 36) {
        tmd_load3(base, ap, 0x1C, 0x20);
        gte_cop2_rtpt();
        c0 = tmd_tex_command(ap, 0x10);
        c1 = tmd_word(ap, 0x14);
        c2 = tmd_word(ap, 0x18);
        out[3] = tmd_word(ap, 4);
        out[6] = tmd_word(ap, 8);
        out[9] = tmd_word(ap, 0xC);
        if (tmd_flag_error() || gte_cop2_nclip() <= 0) continue;
        otz = gte_cop2_avsz3();
        out[2] = gte_cop2_sxy(0);
        out[5] = gte_cop2_sxy(1);
        out[8] = gte_cop2_sxy(2);
        out[1] = c0;
        out[4] = c1;
        out[7] = c2;
        tmd_ot_link(ot, shift, otz, out, 9);
        out += 10;
    }
    TMD_END(out);
}

/* ---- code 0x39: 4-vertex Gouraud (GsTMDfastNG4) -----------------------
 * Packet: tag(8) rgb0|cmd sxy0 rgb1 sxy1 rgb2 sxy2 rgb3 sxy3. */
s32 GsTMDfastNG4(u8 *prim, u8 *vtx, s32 packet, u16 count, s32 shift,
                 s32 ot)
{
    TMD_BEGIN(ap, base, out, prim, vtx, packet);
    s32 otz;

    for (; count != 0; count--, ap += 28) {
        tmd_load3(base, ap, 0x14, 0x18);
        gte_cop2_rtpt();
        if (tmd_flag_error() || gte_cop2_nclip() <= 0) continue;
        tmd_load_v3(base, ap, 0x1A);
        out[2] = gte_cop2_sxy(0);
        out[4] = gte_cop2_sxy(1);
        out[6] = gte_cop2_sxy(2);
        gte_cop2_rtps();
        out[1] = tmd_flat_colour(ap);
        out[3] = tmd_word(ap, 8);
        out[5] = tmd_word(ap, 0xC);
        if (tmd_flag_error()) continue;
        otz = gte_cop2_avsz4();
        out[7] = tmd_word(ap, 0x10);
        out[8] = gte_cop2_sxy(2);
        tmd_ot_link(ot, shift, otz, out, 8);
        out += 9;
    }
    TMD_END(out);
}

/* ---- code 0x2D: 4-vertex flat textured (GsTMDfastTNF4) ----------------
 * Packet: tag(9) cmd|rgb sxy0 uv0 sxy1 uv1 sxy2 uv2 sxy3 uv3. */
s32 GsTMDfastTNF4(u8 *prim, u8 *vtx, s32 packet, u16 count, s32 shift,
                  s32 ot, s32 arg3)
{
    TMD_BEGIN(ap, base, out, prim, vtx, packet);
    s32 otz;

    (void)arg3;
    for (; count != 0; count--, ap += 32) {
        tmd_load3(base, ap, 0x18, 0x1C);
        gte_cop2_rtpt();
        out[1] = tmd_tex_command(ap, 0x14);
        if (tmd_flag_error() || gte_cop2_nclip() <= 0) continue;
        tmd_load_v3(base, ap, 0x1E);
        out[2] = gte_cop2_sxy(0);
        out[4] = gte_cop2_sxy(1);
        out[6] = gte_cop2_sxy(2);
        gte_cop2_rtps();
        out[3] = tmd_word(ap, 4);
        out[5] = tmd_word(ap, 8);
        out[7] = tmd_word(ap, 0xC);
        out[9] = tmd_word(ap, 0x10);
        if (tmd_flag_error()) continue;
        otz = gte_cop2_avsz4();
        out[8] = gte_cop2_sxy(2);
        tmd_ot_link(ot, shift, otz, out, 9);
        out += 10;
    }
    TMD_END(out);
}

/* ---- code 0x3D: 4-vertex Gouraud textured (GsTMDfastTNG4) -------------
 * Packet: tag(12) cmd|rgb0 sxy0 uv0 rgb1 sxy1 uv1 rgb2 sxy2 uv2 rgb3 sxy3
 * uv3. */
s32 GsTMDfastTNG4(u8 *prim, u8 *vtx, s32 packet, u16 count, s32 shift,
                  s32 ot)
{
    TMD_BEGIN(ap, base, out, prim, vtx, packet);
    u32 c0, c1, c2, c3;
    s32 otz;

    for (; count != 0; count--, ap += 44) {
        tmd_load3(base, ap, 0x24, 0x28);
        gte_cop2_rtpt();
        c0 = tmd_tex_command(ap, 0x14);
        c1 = tmd_word(ap, 0x18);
        c2 = tmd_word(ap, 0x1C);
        c3 = tmd_word(ap, 0x20);
        if (tmd_flag_error() || gte_cop2_nclip() <= 0) continue;
        tmd_load_v3(base, ap, 0x2A);
        out[2] = gte_cop2_sxy(0);
        out[5] = gte_cop2_sxy(1);
        out[8] = gte_cop2_sxy(2);
        gte_cop2_rtps();
        if (tmd_flag_error()) continue;
        otz = gte_cop2_avsz4();
        out[3] = tmd_word(ap, 4);
        out[6] = tmd_word(ap, 8);
        out[9] = tmd_word(ap, 0xC);
        out[12] = tmd_word(ap, 0x10);
        out[11] = gte_cop2_sxy(2);
        out[1] = c0;
        out[4] = c1;
        out[7] = c2;
        out[10] = c3;
        tmd_ot_link(ot, shift, otz, out, 12);
        out += 13;
    }
    TMD_END(out);
}

/* ======================================================================
 * Lit builders, the "L" arm of TmdF3 / TmdTF3 / TmdTG3 (GsLIOFF = 0, not
 * the fog/LFG condition).  Reached: TACO (scene 144) gameplay draws every
 * one of them (pc/tools/tmd_census.py); no other lit arm and no GsTMDdiv*
 * run was reached (GTE-BUILDERS.md census).
 *
 * Signature (from TmdF3/TmdTF3/TmdTG3, retail stack layout): prim, vtx,
 * normal table, packet, count, shift, ot.  Lit prim layouts: each vertex
 * word is normal index (low half) | vertex index (high half).
 *
 *   code prim packet builder        retail entry
 *   0x20  16   5 w   GsTMDfastF3L   0x800A672C (TMDF_F3)
 *   0x24  24   8 w   GsTMDfastTF3L  0x800A7810 (TMDF_FT3)
 *   0x34  28  10 w   GsTMDfastTG3L  0x800A7DC4 (TMDF_GT3 +0x2D4)
 *
 *   F3   +4 rgb|mode  +8 n0|v0  +C v1|v2
 *   TF3  +4 uv0|cba +8 uv1|tsb +C uv2  +10 n0|v0  +14 v1|v2
 *   TG3  +4 uv0|cba +8 uv1|tsb +C uv2  +10 n0|v0  +14 n1|v1  +18 n2|v2
 *
 * The lighting is the GTE's: RGBC is loaded (flat: the TMD colour word |
 * GsTON << 25; textured: 0x808080 under the command byte (mode | GsTON <<
 * 1) << 24, the texture being modulated), then NCCS on the face normal or
 * NCCT on the three vertex normals, with the LLM / LCM / BK the libgs
 * setters loaded.  Measured against retail: 785 TF3/TG3 packets in two
 * TACO frames reproduced word for word (BK = ambient << 4, LCM = _LC,
 * LLM = GsLIGHTWSMATRIX x Lw).
 * ====================================================================== */

static void tmd_ldv_index(s32 slot, const u8 *table, u32 index)
{
    gte_cop2_ldv(slot, tmd_vertex(table, index));
}

static u32 tmd_lit_tex_rgbc(const u8 *prim)
{
    u32 code = (u32)(s32)(s8)prim[3] | ((u32)GsTON << 1);
    return 0x00808080u | (code << 24);
}

/* ---- code 0x20: flat lit (GsTMDfastF3L) -------------------------------
 * RTPT; RGBC; FLAG; NCLIP; AVSZ3; V0 = normal; NCCS.
 * Packet: tag(4) RGB2 sxy0 sxy1 sxy2.  Nothing is stored before the tests. */
s32 GsTMDfastF3L(u8 *prim, u8 *vtx, u8 *normal, s32 packet, u16 count,
                 s32 shift, s32 ot)
{
    TMD_BEGIN(ap, base, out, prim, vtx, packet);
    u8 *nbase = TMD_PTR(normal);
    u32 a, b;
    s32 otz;

    for (; count != 0; count--, ap += 16) {
        a = tmd_word(ap, 8);
        b = tmd_word(ap, 0xC);
        tmd_ldv_index(0, base, a >> 16);
        tmd_ldv_index(1, base, b);
        tmd_ldv_index(2, base, b >> 16);
        gte_cop2_rtpt();
        gte_cop2_set_rgbc(tmd_flat_colour(ap));
        if (tmd_flag_error() || gte_cop2_nclip() <= 0) continue;
        otz = gte_cop2_avsz3();
        tmd_ldv_index(0, nbase, a);
        gte_cop2_nccs();
        tmd_ot_link(ot, shift, otz, out, 4);
        out[2] = gte_cop2_sxy(0);
        out[3] = gte_cop2_sxy(1);
        out[4] = gte_cop2_sxy(2);
        out[1] = gte_cop2_rgb(2);
        out += 5;
    }
    TMD_END(out);
}

/* ---- code 0x24: flat textured lit (GsTMDfastTF3L) ---------------------
 * The UVs are stored before the FLAG/NCLIP tests (as retail does).
 * Packet: tag(7) RGB2 sxy0 uv0 sxy1 uv1 sxy2 uv2. */
s32 GsTMDfastTF3L(u8 *prim, u8 *vtx, u8 *normal, s32 packet, u16 count,
                  s32 shift, s32 ot)
{
    TMD_BEGIN(ap, base, out, prim, vtx, packet);
    u8 *nbase = TMD_PTR(normal);
    u32 a, b;
    s32 otz;

    for (; count != 0; count--, ap += 24) {
        a = tmd_word(ap, 0x10);
        b = tmd_word(ap, 0x14);
        tmd_ldv_index(0, base, a >> 16);
        tmd_ldv_index(1, base, b);
        tmd_ldv_index(2, base, b >> 16);
        gte_cop2_rtpt();
        gte_cop2_set_rgbc(tmd_lit_tex_rgbc(ap));
        out[3] = tmd_word(ap, 4);
        out[5] = tmd_word(ap, 8);
        out[7] = tmd_word(ap, 0xC);
        if (tmd_flag_error() || gte_cop2_nclip() <= 0) continue;
        otz = gte_cop2_avsz3();
        tmd_ldv_index(0, nbase, a);
        gte_cop2_nccs();
        tmd_ot_link(ot, shift, otz, out, 7);
        out[2] = gte_cop2_sxy(0);
        out[4] = gte_cop2_sxy(1);
        out[6] = gte_cop2_sxy(2);
        out[1] = gte_cop2_rgb(2);
        out += 8;
    }
    TMD_END(out);
}

/* ---- code 0x34: Gouraud textured lit (GsTMDfastTG3L) ------------------
 * RTPT; RGBC; FLAG; NCLIP; AVSZ3; V0..V2 = normals; NCCT.
 * Packet: tag(9) RGB0 sxy0 uv0 RGB1 sxy1 uv1 RGB2 sxy2 uv2. */
s32 GsTMDfastTG3L(u8 *prim, u8 *vtx, u8 *normal, s32 packet, u16 count,
                  s32 shift, s32 ot)
{
    TMD_BEGIN(ap, base, out, prim, vtx, packet);
    u8 *nbase = TMD_PTR(normal);
    u32 a, b, c;
    s32 otz;

    for (; count != 0; count--, ap += 28) {
        a = tmd_word(ap, 0x10);
        b = tmd_word(ap, 0x14);
        c = tmd_word(ap, 0x18);
        tmd_ldv_index(0, base, a >> 16);
        tmd_ldv_index(1, base, b >> 16);
        tmd_ldv_index(2, base, c >> 16);
        gte_cop2_rtpt();
        gte_cop2_set_rgbc(tmd_lit_tex_rgbc(ap));
        if (tmd_flag_error() || gte_cop2_nclip() <= 0) continue;
        otz = gte_cop2_avsz3();
        tmd_ldv_index(0, nbase, a);
        tmd_ldv_index(1, nbase, b);
        tmd_ldv_index(2, nbase, c);
        gte_cop2_ncct();
        tmd_ot_link(ot, shift, otz, out, 9);
        out[3] = tmd_word(ap, 4);
        out[6] = tmd_word(ap, 8);
        out[9] = tmd_word(ap, 0xC);
        out[2] = gte_cop2_sxy(0);
        out[5] = gte_cop2_sxy(1);
        out[8] = gte_cop2_sxy(2);
        out[1] = gte_cop2_rgb(0);
        out[4] = gte_cop2_rgb(1);
        out[7] = gte_cop2_rgb(2);
        out += 10;
    }
    TMD_END(out);
}

/* ---- code 0x34, fog arm: GsTMDfastTG3LFG (retail 0x800A7F78) ----------
 * TmdTG3 takes it when GsLIGNR == 0 && GsLIGHT_MODE == 1 (or GsLIGNR == 1
 * && GsLMODE == 1).  The listing is GsTMDfastTG3L's with NCDT
 * (c2 0x0F80416) in place of NCCT: the three vertex colours are depth
 * cued toward the far colour by RTPT's IR0 (DQA / DQB).  Same packet.
 * Reached: TAIIKU's ball rolling (TAMA2.TMD, 3 Jun 95 sports festival;
 * playthrough spring_events.txt).  The PC used to supply a `void` stub
 * here, so TmdTG3's tail call handed GsSortObject4 whatever %rax held as
 * the packet cursor. */
s32 GsTMDfastTG3LFG(u8 *prim, u8 *vtx, u8 *normal, s32 packet, u16 count,
                    s32 shift, s32 ot)
{
    TMD_BEGIN(ap, base, out, prim, vtx, packet);
    u8 *nbase = TMD_PTR(normal);
    u32 a, b, c;
    s32 otz;

    for (; count != 0; count--, ap += 28) {
        a = tmd_word(ap, 0x10);
        b = tmd_word(ap, 0x14);
        c = tmd_word(ap, 0x18);
        tmd_ldv_index(0, base, a >> 16);
        tmd_ldv_index(1, base, b >> 16);
        tmd_ldv_index(2, base, c >> 16);
        gte_cop2_rtpt();
        gte_cop2_set_rgbc(tmd_lit_tex_rgbc(ap));
        if (tmd_flag_error() || gte_cop2_nclip() <= 0) continue;
        otz = gte_cop2_avsz3();
        tmd_ldv_index(0, nbase, a);
        tmd_ldv_index(1, nbase, b);
        tmd_ldv_index(2, nbase, c);
        gte_cop2_ncdt();
        tmd_ot_link(ot, shift, otz, out, 9);
        out[3] = tmd_word(ap, 4);
        out[6] = tmd_word(ap, 8);
        out[9] = tmd_word(ap, 0xC);
        out[2] = gte_cop2_sxy(0);
        out[5] = gte_cop2_sxy(1);
        out[8] = gte_cop2_sxy(2);
        out[1] = gte_cop2_rgb(0);
        out[4] = gte_cop2_rgb(1);
        out[7] = gte_cop2_rgb(2);
        out += 10;
    }
    TMD_END(out);
}
