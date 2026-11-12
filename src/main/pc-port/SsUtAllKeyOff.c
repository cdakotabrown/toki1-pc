/*
 * SsUtAllKeyOff -- key off every SPU voice (PC override of
 * src/main/matched/SsUtAllKeyOff.c).
 *
 * Statement for statement the matched function; the ONE divergence is
 * addressing (src/main/pc-port/mv_state.h doctrine -- the pointer width is
 * a property of the host, not of the game): libsnd_spu_regs_ptr is a 4-byte PS1
 * pointer global (0x1F801C00, the SPU voice register block) whose native
 * 8-byte load folds the neighbouring retail ramp table into the high half,
 * so the per-voice register stores run away from the mapped window.  Read
 * as its PS1-width u32 and zero-extended here.  This was the scene matrix's
 * dominant first-fail class (150 of 211 scenes, SsUtAllKeyOff+0x81).
 *
 * The matched header follows.
 *
 * Function: SsUtAllKeyOff
 * Address: 0x8008F89C
 * Size: 300 bytes (75 instructions)
 * Region: Library (0x80086000+) -- Konami sound driver, sibling of SsSepOpen
 * Compiler: IDO 5.2 -O2 (confirmed: t6..t9 expression temps in ugen's
 *   $14/$15/$24/$25/$8.. order, `blez`/`bnez` not `j`, no `mult`, the
 *   `addu`-form move is the region's convert_or_to_addu; as1 -O2 confirmed by
 *   the lw-above-sll/sra load-delay hoist and the branch-slot fills).
 * Status: BYTE-PERFECT 75/75, Body 75 == target 75, Fixup 0 (2026-09-22)
 *
 * ---- 2026-09-22: CLOSED -- new as1 Hook 50 IDO52_AS1_NO_SHIFT_COMBINE ----
 * THE WALL below is func_41673c (the pass-II `sll` handler): the reaching
 * def of the outer sll's source is an sll, sum 3+16 < 32, so it combines.
 * Hook 50 vetoes it at its own no-change exit (NO_SHIFT_COMBINE_TRACE: one
 * candidate, `sll=0018cc00 def=0010c0c0 sum=19`).  Then:
 *  - `u16 *spuData` / `extern u16 *libsnd_spu_regs_ptr`: retail stores 0x80FF with
 *    `ori s7,zero,0x80ff`; an s16 pointee makes it `addiu s7,zero,-32513`
 *    (that was the Fixup word; s16 = 74/75).
 *  - TIE_SRC_ORDER=bb:2 (loop block in source order) + re-fitted ties
 *    TAKE=23,31,39,175 REJECT=6,62,63,182,185 (greedy climb + trace;
 *    62/63 put `lui s3` between `sw s6` and `sw s7`).
 * Every token proven by removal: -TARGET_SAFE 70/75, -NOINREG 43/75,
 * -NO_SHIFT_COMBINE 39/75 (74 words), -SRC_ORDER 63/75, each TAKE 70-73,
 * each REJECT 69-73.
 *
 * @SymbolStart: 0x8008F89C
 * @SymbolSize: 300
 * @CompileFlags: -Wo,-do_opt_saved_regs
 * @CompileEnv: IDO52_UGEN_FORCE_TARGET_SAFE=all IDO52_CONST_NOINREG_VALUES=24:1:1 IDO52_AS1_NO_SHIFT_COMBINE=all IDO52_AS1_TIE_SRC_ORDER=bb:2 IDO52_AS1_TIE_TAKE=23,31,39,175 IDO52_AS1_TIE_REJECT=6,62,63,182,185
 * @PostProc: strip_leading_instructions --count auto
 * @PostProc: convert_or_to_addu
 * @Measured: 2026-09-22 75/75 exact, compiled 75 vs target 75 words, Fixup 0 -- BYTE-PERFECT
 *
 * BEFORE this change (measured, same harness): 12/75 exact with Fixup 3
 * (honest 9/75), Body 73 -- TWO words short.  The old header's Hook 36 force
 * (`IDO52_UGEN_TEMP_REG=3:t7`) was fitted to a wrong body and is dropped.
 *
 * THE TWO MISSING WORDS, IDENTIFIED:
 *  1. The load-delay `nop` at 0x8008F9B4 between the sunk `lw s1..s8`
 *     restores and the common `lw ra`: retail SHRINK-WRAPS -- s0/ra saved at
 *     sp+24/28 in the prologue, s1..s8 saved at sp+60..32 only past the
 *     `blez` guard and restored only on the loop-exit path.  CLOSED by
 *     `-Wo,-do_opt_saved_regs` (the SsSepOpen / libgs-emitter recipe).
 *  2. `sll t8,s0,3` at 0x8008F930: ugen emits `mul $24,$16,8; sll $25,$24,16;
 *     sra $8,$25,16; mul $9,$8,2` (visible in `cc -S`, temp numbering
 *     $15/$24/$25/$8/$9/$10 == retail's t7/t8/t9/t0/t1/t2 exactly) and as1
 *     pass II FOLDS the two shifts into `sll t9,s0,0x13`, leaving the first
 *     as a dead `move t8,s0` that copy-prop erases (`IDO52_AS1_NO_COPY_PROP=all`
 *     shows the move).  Retail's as1 did not fold.  STILL OPEN -- see the wall.
 *
 * WHAT ELSE CLOSED (all measured against the retail bytes):
 *  - Frame 72 (was 64 / 80 under the flag): no `entry` / `idx8` locals.  Under
 *    -do_opt_saved_regs EITHER named loop-scratch local costs +8; the stores
 *    must index the array directly.  `spuData` (assigned before the loop,
 *    live across the call) costs nothing.
 *  - Retail's stride is the as1 expansion of a single immediate
 *    `mul $15,$16,24` (`sll t7,s0,2; subu t7,t7,s0; sll t7,t7,3` -- ONE temp).
 *    The old `(i*4-i)*8` gave three temps; `i * 24` / `&arr[i]` join the
 *    `= 24` store's held constant web (`multu s0,s4; mflo`).  Retail holds
 *    `li s4,24` for the store AND multiplies by immediate, so the two 24s are
 *    different descriptors and the multiplier's is NOT held.  `u16 b` splits
 *    the descriptors (type=8 vs type=6 in IDO52_FORM_TRACE) but uopt still
 *    holds both and demotes 16384 to an in-loop `li`; the windowed
 *    `IDO52_CONST_NOINREG_VALUES=24:1:1` (query 1 = the int-typed multiplier)
 *    then reproduces retail.  With an s16 field the descriptor is shared and
 *    the same window is silently inert (blocked site re-queries).
 *  - `IDO52_UGEN_FORCE_TARGET_SAFE=all`: the latch narrowing is in place
 *    (`sll s0,s0,16; sra s0,s0,16`), and the bottom `lb` lands in t2.
 *  - `IDO52_AS1_TIE_REJECT=6`: the prologue `blez` slot.  Retail has
 *    `move s0,zero` above the branch and `sw a0,72(sp)` in the slot; ugen
 *    emits `sw $4; move $16,$0; lb; ble` and tie #6 (both prio 0) picks the
 *    move.  This prologue shape is UNIQUE in the main binary (retail-wide
 *    scan), no source spelling of the parameter (s32/s16/u8/ptr/void, i as the
 *    parameter, guard read into a local, decl-initialiser) moves it.
 *  - bb=1 (sunk saves + preheader): `TAKE=23,31,39,91 REJECT=70` from a
 *    greedy tie climb (tieclimb, 98 ties) moved the `lui s2`/`lui s3`/`addiu
 *    s3` slots most of the way (`lui s3` is one slot late at word 16).
 *    `TIE_SRC_ORDER=bb:1` is worse; `bb:0`/`all` break the head hoist.
 *
 * THE WALL (as1 pass-II shift fold, no veto hook):
 *  The fold's precondition is that the intermediate has ONE use in as1's
 *  input: with a second use (`n = i*8; D_x = n; ...(s16)n`) the pair survives
 *  (`sll a1,s0,3; sll t8,a1,16`).  The byte-perfect specimen
 *  OPTION/func_80133768 shows how retail's stream had that second use: an
 *  in-place accumulate on a multi-def variable makes ugen emit
 *  `mul $14,$10,16; move $10,$14; sll $15,$10,16; move ...`, as1 folds nothing
 *  (the move is a use), then copy-prop deletes the moves.  Here the product
 *  would need to be a uopt web that survives to ugen; every spelling tried
 *  is folded back into the temp chain by uopt BEFORE ugen:
 *    idx8 = i; idx8 = idx8 * 8 | idx8 *= 8 | idx8 = idx8 << 3 |
 *    n = i; n = n * 8; idx8 = n | idx8 = i*8; idx8 = idx8 | n = i*8; idx8 = n;
 *    n = idx8 | arg0 = i * 8 (parameter as scratch) | arg0 = i << 3 |
 *    (s16)(u16)(i*8) | q = &spuData[(s16)(i*8)] | 16-byte Spu struct index
 *  and the uopt-side vetoes are inert or fatal: IDO52_SKIP_COPYPROPAGATE=1
 *  changes nothing (the copy is folded after that pass -- Hook 32 headline),
 *  IDO52_SKIP_PASSES=copypropagate aborts uopt (signal 6).
 *  as1-side: the fold is NOT func_416ca8 (IDO52_AS1_KEEP_ANDI_TRACE prints no
 *  candidates on this TU); KEEP_ANDI=all, FORCE_NARROW_FOLD=all,
 *  NO_PASS1_PROP=1/all, NO_COPY_PROP=all (keeps a dead move, not the sll),
 *  NO_SCHEDULE=1, NO_BOPT=1, NO_BRANCH_OPT=1, XBB_SKIP_CAND=0/1/0,1/1,2 (as1
 *  offers no XBB candidate here at all) all leave `sll t9,s0,0x13`.
 *  Source-order ties on the loop block (`TIE_SRC_ORDER=bb:3`) print Body 75
 *  but it is a COUNT COMPENSATOR: the fold stays and a load-delay nop appears
 *  after the un-hoisted `lb t2`.  Do not ship it.
 *  Needed primitive: a veto at as1's pass-II shift-combine (the sibling of
 *  Hook 29, different sub-transform of func_419268), or a uopt lever that
 *  keeps a single-use expression variable as a web.  The two Fixup words
 *  (sh offsets at words 36/44) and the store/shift interleave are the
 *  scheduler's consequence of the folded 3-chain and should follow.
 *
 * Also refuted (do not re-run): 24U / sizeof(Ent) / (u32)i index split the
 * web but stay held; ((i<<2)-i)<<3 gives three temps (subu is not in Hook
 * 41b's in-place list); a `base = _svm_voice` pointer local (any position)
 * costs +8 frame and moves nothing; IDO52_FORM_SITE=const:24:13:0 and
 * plain NOINREG=24 kill the store's `li s4,24` too.
 *
 * 2026-09-22: `-cckr` added to the flags is inert on this body
 * (29/75, Body 74, Fixup 2 either way).  The as1 pass-II shift fold is
 * still the wall; no C-side lever found this pass.
 *
 * Semantics (re-derived from the retail bytes): for each of spuVmMaxVoice
 * channels, reset the 24-byte channel record at _svm_voice[i] (fields
 * +2=24, +0=255, +23=0, +4/+6=0, +10=255, +12/+14=0, +16=255), reset the
 * 16-byte SPU voice record at (*libsnd_spu_regs_ptr)[i*8] (0,0,4096,512,0x80FF,
 * 16384), publish the channel index in D_8013160A and call
 * SpuVmKeyOffNow(1).  arg0 is homed (`sw a0,72(sp)`) and never read.
 */
#include "types.h"

typedef struct {
    s16 a;
    u16 b;
    s16 c;
    s16 d;
    s16 e;
    s16 f;
    s16 g;
    s16 h;
    s16 i;
    s16 j;
    s16 k;
    u8 l;
    u8 m;
} Ent;

extern Ent _svm_voice[];
extern u32 libsnd_spu_regs_ptr;   /* PS1-width pointer handle (see header) */
extern s8 spuVmMaxVoice;
extern s16 D_8013160A;

extern void SpuVmKeyOffNow(s32 arg0);

void SsUtAllKeyOff(s32 arg0) {
    s16 i;
    u16 *spuData;

    i = 0;
    if (spuVmMaxVoice > 0) {
        spuData = (u16 *)(unsigned long)libsnd_spu_regs_ptr;
        do {
            _svm_voice[i].b = 24;
            _svm_voice[i].a = 255;
            _svm_voice[i].m = 0;
            _svm_voice[i].c = 0;
            _svm_voice[i].d = 0;
            _svm_voice[i].f = 255;
            _svm_voice[i].g = 0;
            _svm_voice[i].h = 0;
            _svm_voice[i].i = 255;
            spuData[(s16)(i * 8) + 0] = 0;
            spuData[(s16)(i * 8) + 1] = 0;
            spuData[(s16)(i * 8) + 3] = 512;
            spuData[(s16)(i * 8) + 2] = 4096;
            spuData[(s16)(i * 8) + 4] = 0x80FF;
            spuData[(s16)(i * 8) + 5] = 16384;

            D_8013160A = i;
            SpuVmKeyOffNow(1);

            i = (s16)(i + 1);
        } while (i < spuVmMaxVoice);
    }
}
