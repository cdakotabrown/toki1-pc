/*
 * SpuSetReverbModeParam -- SPU reverb mode setup (PC override of
 * src/main/matched/SpuSetReverbModeParam.c).
 *
 * Statement for statement the matched function; the ONE divergence is the
 * `D_800E39A8` CdVol pointer (src/main/pc-port/mv_state.h doctrine -- the
 * pointer width is a property of the host, not of the game).  The matched C
 * models the retail global as `static CdVol *D_800E39A8` (a file-local NULL;
 * the retail word at 0x800E39A8 holds 0x1F801C00, the SPU register block), so
 * the native build writes p->x/p->y to absolute 0x184/0x186 and SIGSEGV
 * (measured: SpuSetReverbModeParam+0x135 from SD_Init).  Here the global is
 * read as its PS1-width u32 from the mapped RAM window and zero-extended --
 * the pointer the console's `lw` produces.
 *
 * The matched header follows.
 *
 * Function: SpuSetReverbModeParam
 * Address: 0x80098800
 * Size: 1408 bytes (352 instructions)
 * Region: Library (0x80086000+)
 *
 * Status: BYTE-PERFECT 352/352 (2026-09-25; see the note at
 *   the end of this header).  NOT registered in level3_functions.json.
 * (history) Status: SEMI (2026-09-24 re-derivation).  15/352 exact, Body 362 vs
 *   target 352, Fixup 5 -- NOT registered.  The previous body scored 24/352
 *   (Body 351, Fixup 7) but was a volatile-everywhere transcription with a
 *   WRONG store: it wrote ((s16 *)buf)[15] = q2 + [19], retail stores index
 *   18 (`sh t7,128(sp)`, buf at sp+92).  This body is re-derived word by
 *   word from retail, natural C, no volatile, and is semantically faithful.
 *
 * PIPELINE: IDO 5.2 codegen assembled by ASPSX (@Compiler: ido52-gas).
 *   The tell is copy-and-bump everywhere: `beqz t6,0x80098988 ; lw t3,76(sp)`
 *   branches past the ORIGINAL `lw t3,76(sp)` at 0x80098984 with a copy in
 *   the slot (also 0x80098a9c/0x80098b90/0x80098bfc/0x80098cf0, all $sp
 *   loads -> --ido-copy-dead-ft), `b ; li v0,1` / unreachable `move v0,zero`
 *   at word 9, and unfilled load-delay nops.  `div` by 127 is a plain
 *   `li at,127 ; div ; mflo` (no strength reduction, no break check) -- the
 *   old "div-by-127 magic multu" blocker was a misread of the retail bytes.
 *
 * SEMANTICS (recovered): cmd_ptr[0] = flag word (bit0 type, bit3 volume,
 *   bit4 pan, bit1/bit2 x/y), 0 = "all"; cmd_ptr[1] = type (bit 8 = also
 *   call SpuClearReverbWorkArea); cmd_ptr[3] volume, cmd_ptr[4] pan, halves of
 *   cmd_ptr[2] = x/y for D_800E39A8+388/390 and libspu_rev_depth_left/4A.  Frame 160:
 *   buf[17] at sp+92, flags 88, type 84, use_flag 80, is_null 76, (72),
 *   local_68 68, local_64 64, local_60 60, local_56 56, local_52 52,
 *   table_ptr 48, cmd_ptr at its own home 160, ra 44.
 *
 * RESIDUALS (measured 2026-09-24), in order of size:
 *   1. ALLOCATION.  Retail colours cmd_ptr t0, flags t1, type v1,
 *      table_ptr a3, is_null v0 (only until its store) and saves/restores
 *      them around every call into their homes (the naive caller-spill
 *      form).  Ours spills cmd_ptr (web bitpos 2) and gives is_null (11) t0.
 *      `IDO52_FORCE_WEB_NOSPILL=2 IDO52_FORCE_WEB_COLOR_NOSPLIT=2` makes the
 *      frame exactly 160 (21/352, Body 354, Fixup 2) but colours cmd_ptr
 *      s0; `FORCE_WEB_COLOR=2:t0` and `11:v0` are refused by the legality
 *      gate (forbid masks 7f8.. / 7e..): retail's pieces are range-split
 *      webs, so the forces must target the SPLIT bitposes (GC_COLOR_TRACE
 *      without NOSPLIT) -- the DivideF3 recipe, not attempted to the end.
 *   2. SWITCH LAYOUT.  Retail lays each `switch (libspu_rev_mode)` out as
 *      `b test ; case bodies ; test: li at,7 ; beq ; li at,8 ; beq ; default`
 *      (case bodies ABOVE the compare chain); ours puts the compare chain
 *      first.
 *   3. (v << 12) / 127 is one web in retail (v0, reused by five adds); ours
 *      adds a `move v0,t4`.
 * Not hooked: the NOSPILL pair above improves the score without closing
 *   anything, so it is recorded here rather than shipped.
 * (prev) @Measured: 2026-09-24 15/352 exact (4.3%), compiled 362 vs target 352 words, Fixup 5 -- OVERRUN
 * (at)CompileFlags: -Wc,-pic0
 * (at)PostProc: strip_leading_instructions --count auto
 * varref: no split (IDO52_BLOCK_SPLIT_TRACE=1, 2026-09-24)
 * (prev) @Measured: 2026-09-25 23/352 exact (6.5%), compiled 350 vs target 352 words, Fixup 8 -- SEMI
 *   Pipeline switched ido52-gas -> NATIVE as1 (-Wc,-pic0, convert_or_to_addu,
 *   IDO52_AS1_XBB_SKIP_CAND=all + NO_FALLTHRU_FILL=all; each load-bearing:
 *   FALLTHRU alone 23 F14 Body 321).  Shipped gas config was `@Compiler:
 *   ido52-gas` + `--ido-copy-and-bump --ido-fill-load-delay --ido-fill-macro-lui
 *   --ido-copy-dead-ft --ido-fill-at-next --ido-jr-store-slot --ido-b-store-slot`:
 *   15/352 F5, Body 362 (OVERRUN).  Native: honest 15 vs 10, Body 350 vs 352.
 *   Residual is C-side (frame 152 vs 160, a0 homed vs retail `move t0,a0`,
 *   the zero-init store run order); DIV_MACRO/FORCE_TARGET_SAFE inert.
 * 2026-09-25: IDENTIFIED, not worked (time went to SpuMallocWithStartAddr).
 *   This is libspu SpuSetReverbModeParam (Psy-Q LIBSPU member S_SRMP in both
 *   2.5 and 4.5; neither object is verbatim: 2.5 is 1264 bytes, 4.5 1248,
 *   retail 1408).  Sibling SpuMallocWithStartAddr is IsIdMatrix and closed
 *   byte-perfect today on the native-as1 recipe (static table pointer,
 *   FORCE_WEB_COLOR off s0, NO_COPY_PROP / KEEP_DEAD_RELOAD for the kept
 *   copies, XBB_SKIP_CAND + NO_FALLTHRU_FILL, declaration order for the
 *   spill homes) -- start from that header.  Body unchanged.
 * (prev) @Measured: 2026-09-25 23/352 exact (6.5%), compiled 350 vs target 352 words, Fixup 8 -- SEMI
 */
/*
 * ---- PREVIOUS HEADER (superseded) ----

 * Function: SpuSetReverbModeParam
 * Address: 0x80098800
 * Size: 1408 bytes (352 instructions)
 * Region: Library (0x80086000+)
 * Status: SEMI - 352-insn function blocked by (1) div-by-127 non-power-of-2
 *   constant divisions (lines ~128/129/174 -> multu/divu magic the build can't
 *   reproduce from C) and (2) the -O1 build inlining read_*_unaligned helper
 *   bodies for the cmd_ptr[] accesses, which the hand-tuned custom_script no longer
 *   strips. Needs deeper work, not a header fix. De-registered (ASM fallback).
 *
 * CD audio command processing. 160-byte frame, only ra saved.
 * (at)PostProc: convert_or_to_addu
 * (at)PostProc: strip_leading_instructions --count auto
 * (at)Measured: 2026-09-19 24/352 exact (6.8%), compiled 351 vs target 352 words, Fixup 7 -- SEMI
 * ⭐ 2026-09-17 HOOK 36 BLIND SWEEP: +4 words, 20 -> 24/352 (6.8%).
 *   Body 351 words (unchanged), Fixup 7 (unchanged).
 *   Greedy sweep of every temp-register REQUEST x every register the
 *   hook's own TRACE free list offers at that request, re-swept after
 *   each accepted force.  Do NOT re-derive the forces by reasoning: the
 *   occ->instruction map DRIFTS under forcing (UOPT-HOOKS-REFERENCE
 *   Hook 36, trap 2).  Re-run the sweep instead if the body changes.
 * (at)CompileEnv: IDO52_UGEN_TEMP_REG=7:t8,10:t7,29:t2
 
 */
/* hook61 (2026-09-25): ido52-gas; ugen KEEP_JUMP_NEXT=all honest 10->11 but
 * Body 362->363 vs 352 (further) -- rejected. */
/*
 * ---- 2026-09-25: 23/352 -> 352/352 BYTE-PERFECT -------------
 * libspu SpuSetReverbModeParam, IDO 5.2 -O2, native as1 (pic0).  Sony's
 * names: D_80131640.. = _spu_rev_attr (+4 mode, +8/+A depth L/R, +C delay,
 * +10 feedback), _spu_rev_startaddr = _spu_rev_startaddr[10], _spu_rev_param = the
 * 10 x 68-byte reverb parameter table, _spu_fd = _spu_fd,
 * _spu_rev_offsetaddr = _spu_rev_offsetaddr, _spu_ioctl = GetTPage,
 * _SpuIsInAllocateArea_ = _SpuIsInAllocateArea_, SpuClearReverbWorkArea =
 * _addque (SPU_REV_MODE_CLEAR_WA = 0x100).
 * Source-shape fixes, each measured:
 *  - D_800E39A8 is a FILE-PRIVATE STATIC (Sony 2.5 S_SRMP relocates it
 *    section-relative): 352 vs 311 as an extern.  Its three %hi/%lo pairs
 *    are skipped by the name audits -- hand-checked: every one reads
 *    `lui v0,0x800e ; lw v0,14760(v0)` = 0x800E39A8.
 *  - _spu_rev_param is an array of a 68-byte struct, indexed [mode]: the
 *    `[i * 17]` spelling burned a temp (192 -> 326 in one step).
 *  - is_null: the 1/0 value is a separate local (tn, retail's reserved
 *    slot at sp+72) and is_null itself is VOLATILE (memory-resident,
 *    reloaded per test, first test reads tn in v0); non-volatile: 32/352.
 *  - the three `switch (mode)` are written bodies-first with gotos
 *    (`goto swN; cases; swN: switch -> goto case; default`): retail lays
 *    out `b test ; case bodies ; test ; default` and IDO's cfe always puts
 *    a real switch's compare chain first.  NO_JUMP_MERGE=0,1 keeps uopt
 *    from splicing the case bodies back into the chain.
 *  - the x/y block: `if (!(is_null || flags & 2)) p = D; else { p = D;
 *    p->x = ... }` -- retail loads the pointer on BOTH paths and the y
 *    store reuses it; IDO does no PRE of the load, so both defs are source.
 *  - `buf[m] + (0, q)`: puts q in rs of the four addu (a bare-leaf operand
 *    goes second; the comma makes q a subtree).  Semantically null.
 *  - `q = (v << 12) / 127` in place (mflo v0) needs FORCE_TARGET_SAFE_DIV.
 * Hooks (every token and every list element ablated: all load-bearing):
 *  FORCE_WEB_COLOR 2:t0 + NOSPLIT 2 = cmd_ptr in t0 saved to its own home
 *  around calls (natural: the formal is spilled whole); FORCE_TARGET_SAFE
 *  (`type &= ~0x100` in place); NO_PASS1_PROP=5 keeps `lw t1,0(t0)` after
 *  `move t0,a0`; XBB_SKIP_CAND + NO_FALLTHRU_FILL (list) + FTF_BACKWARD
 *  give retail's fall-through vs copy-and-bump slot fills; TIE_TAKE = the
 *  spill-store placement around the calls (greedy fit).
 * PostProc (allowed classes, count-neutral, no opcode change):
 *  - 3 x swap_instructions: `sh t5,120(sp)` (the buf[14] store) sits right
 *    after its addu in retail; ours sinks it below the other three addus.
 *    Not a tie (prio 0 vs 2): every TIE_TAKE/REJECT in that block and a
 *    struct/typing sweep were inert.  Pure reorder; the sh reads only t5
 *    (defined at 0x258) and sp, the addus read neither memory nor t5.
 *  - remap_stack_slots 20:44,28:48: ra and the table_ptr spill slot.  All
 *    other offsets match; pic1 (+16 below the locals) or dropping the
 *    reserved scalar slots gives the right locals but moves the frame --
 *    retail keeps its register-scalar slots BELOW the save area.
 * What did NOT work: -cckr (inert), -Wo,-noprecolor (inert once the
 *  colours are forced), hook-only is_null piece colouring (Hook 43/53/60:
 *  f_split's successor-only BFS cannot put both arms in the v0 piece).
 * @Measured: 2026-09-25 352/352 exact (100.0%), compiled C vs target 352 words, Fixup 0 -- BYTE-PERFECT
 * @CompileFlags: -Wc,-pic0
 * @PostProc: strip_leading_instructions --count auto
 */
/* @CompileEnv: IDO52_AS1_XBB_SKIP_CAND=all IDO52_AS1_NO_FALLTHRU_FILL=4,7,8,11,12,14,15,16,20,21,22,24 IDO52_FORCE_WEB_COLOR_NOSPLIT=2 IDO52_FORCE_WEB_COLOR=2:t0 IDO52_UGEN_FORCE_TARGET_SAFE=all IDO52_NO_JUMP_MERGE=0,1 IDO52_AS1_FTF_BACKWARD=all IDO52_AS1_NO_PASS1_PROP=5 IDO52_UGEN_FORCE_TARGET_SAFE_DIV=1 IDO52_AS1_TIE_TAKE=38,40,111,116,176,298,327,351 */
/* @PostProc: convert_or_to_addu */
/* @PostProc: swap_instructions 0x264 0x268 */
/* @PostProc: swap_instructions 0x260 0x264 */
/* @PostProc: swap_instructions 0x25c 0x260 */
/* @PostProc: remap_stack_slots --map 20:44,28:48 */
#include "types.h"

typedef struct {
    u8 pad[388];
    s16 x;
    s16 y;
} CdVol;

extern s32 _SpuIsInAllocateArea_();
extern void _memcpy();
extern void _spu_ioctl();
extern void SpuClearReverbWorkArea();

extern s32 _spu_rev_startaddr[];
typedef struct {
    s32 w[17];
} RevParam;

extern RevParam _spu_rev_param[];
extern s32 _spu_fd;
extern s32 _spu_rev_offsetaddr;
extern u32 D_800E39A8;   /* PS1-width pointer handle (see header) */

extern s32 libspu_rev_mode;
extern s16 libspu_rev_depth_left;
extern s16 libspu_rev_depth_right;
extern s32 libspu_rev_delay;
extern s32 libspu_rev_feedback;

s32 SpuSetReverbModeParam(s32 *cmd_ptr)
{
    s32 buf[17];
    s32 flags;
    s32 type;
    s32 use_flag;
    volatile s32 is_null;
    s32 tn;
    s32 local_68;
    s32 local_64;
    s32 local_60;
    s32 local_56;
    s32 local_52;
    s32 *table_ptr;
    CdVol *p;
    s32 v;
    s32 q;

    flags = cmd_ptr[0];
    use_flag = 0;
    if (flags == 0) {
        tn = 1;
    } else {
        tn = 0;
    }
    is_null = tn;
    local_60 = 0;
    local_56 = 0;
    local_52 = 0;
    buf[0] = 0;

    if (tn || (flags & 1)) {
        type = cmd_ptr[1];
        if (type & 0x100) {
            type &= ~0x100;
            use_flag = 1;
        }
        if (type >= 10 || type < 0) {
            return -1;
        }
        table_ptr = &_spu_rev_startaddr[type];
        if (_SpuIsInAllocateArea_(*table_ptr) != 0) {
            return -1;
        }
        libspu_rev_mode = type;
        local_60 = 1;
        _spu_rev_offsetaddr = *table_ptr;
        _memcpy(buf, &_spu_rev_param[type], 68);
        goto sw1;
    sw1_7:
        libspu_rev_feedback = 255;
        libspu_rev_delay = 255;
        goto sw1_end;
    sw1_8:
        libspu_rev_feedback = 0;
        libspu_rev_delay = 255;
        goto sw1_end;
    sw1:
        switch (libspu_rev_mode) {
        case 7: goto sw1_7;
        case 8: goto sw1_8;
        }
        libspu_rev_feedback = 0;
        libspu_rev_delay = 0;
    sw1_end:;
    }

    if (is_null || (flags & 8)) {
        goto sw2;
    sw2_78:
        local_56 = 1;
        if (local_60 == 0) {
            _memcpy(buf, &_spu_rev_param[libspu_rev_mode], 68);
            buf[0] = 0x0C011C00;
        }
        libspu_rev_delay = cmd_ptr[3];
        v = libspu_rev_delay;
        q = (v << 13) / 127;
        ((s16 *)buf)[12] = q - ((u16 *)buf)[2];
        q = (v << 12) / 127;
        ((s16 *)buf)[13] = q - ((u16 *)buf)[3];
        ((s16 *)buf)[14] = ((u16 *)buf)[15] + (0, q);
        ((s16 *)buf)[18] = ((u16 *)buf)[19] + (0, q);
        ((s16 *)buf)[28] = ((u16 *)buf)[30] + (0, q);
        ((s16 *)buf)[29] = ((u16 *)buf)[31] + (0, q);
        goto sw2_end;
    sw2:
        switch (libspu_rev_mode) {
        case 7: goto sw2_78;
        case 8: goto sw2_78;
        }
        libspu_rev_delay = 0;
    sw2_end:;
    }

    if (is_null || (flags & 0x10)) {
        goto sw3;
    sw3_78:
        local_52 = 1;
        if (local_60 == 0) {
            if (local_56 == 0) {
                _memcpy(buf, &_spu_rev_param[libspu_rev_mode], 68);
                buf[0] = 0x80;
            } else {
                buf[0] |= 0x80;
            }
        }
        libspu_rev_feedback = cmd_ptr[4];
        ((s16 *)buf)[9] = (libspu_rev_feedback * 0x8100) / 127;
        goto sw3_end;
    sw3:
        switch (libspu_rev_mode) {
        case 7: goto sw3_78;
        case 8: goto sw3_78;
        }
        libspu_rev_feedback = 0;
    sw3_end:;
    }

    if (local_60) {
        _spu_ioctl(_spu_fd, 106, &local_64);
        if (local_64) {
            local_68 = 0;
            _spu_ioctl(_spu_fd, 96, &local_68);
        }
    }

    if (local_60 == 0) {
        if (!(is_null || (flags & 2))) {
            p = (CdVol *)(unsigned long)D_800E39A8;
        } else {
            p = (CdVol *)(unsigned long)D_800E39A8;
            p->x = ((s16 *)cmd_ptr)[4];
            libspu_rev_depth_left = ((s16 *)cmd_ptr)[4];
        }
        if (is_null || (flags & 4)) {
            p->y = ((s16 *)cmd_ptr)[5];
            libspu_rev_depth_right = ((s16 *)cmd_ptr)[5];
        }
    } else {
        p = (CdVol *)(unsigned long)D_800E39A8;
        p->x = 0;
        p->y = 0;
        libspu_rev_depth_left = 0;
        libspu_rev_depth_right = 0;
    }

    if (local_60 || local_56 || local_52) {
        _spu_ioctl(_spu_fd, 99, buf);
    }
    if (use_flag) {
        SpuClearReverbWorkArea(libspu_rev_mode);
    }
    if (local_60) {
        _spu_ioctl(_spu_fd, 97, &_spu_rev_offsetaddr);
    }
    if (local_60 && local_64) {
        _spu_ioctl(_spu_fd, 96, &local_64);
    }
    return 0;
}
