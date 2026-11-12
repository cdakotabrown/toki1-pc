/*
 * Function: SsUtGetDetVVol
 * Address: 0x8008F648
 * Size: 84 bytes (21 instructions)
 * Region: Library (0x80086000+), IDO 5.2
 * Status: BYTE-PERFECT 21/21, Body 21, no Fixup (2026-09-24) -- pipeline
 *   ido52-gas (copy-and-bump / fill-load-delay / fill-macro-lui /
 *   branch-zero-imm / jr-store-slot) + Hook 41b.  Lever: the two copies and
 *   the `return 0` sit in an `if (1) { }` scope after `r` is formed; that
 *   block boundary stops uopt copy-propagating `r` into its first use, so r
 *   lands in v1 as retail.  NOTE: this is a block-boundary spelling (every
 *   byte compiler-emitted, no PostProc beyond the thunk strip); no more
 *   natural spelling found (b1 early-return, ret-local, *r++, struct row all
 *   Body 22-24).  Earlier history below is kept as hypotheses/record.
 *
 * Leaf 3 of the eight-function blob recorded under func_8008F478 (true start
 * 0x8008F46C, true size 1072, fifteen `jr ra`).  Leaf table:
 *   0x8008F46C/276  0x8008F580/200  0x8008F648/84   0x8008F69C/112
 *   0x8008F70C/132  0x8008F790/156  0x8008F82C/56   0x8008F864/56
 * The last two are byte-perfect in src/main/matched/.
 *
 * SPU voice getter: read the two raw 16-bit words the setter at 0x8008F69C
 * wrote into the 24 x 16-byte shadow table reached through the pointer at
 * libsnd_spu_regs_ptr.
 *
 * ---- WHY THIS IS NOT BYTE-PERFECT (measured 2026-08-30) ----
 *
 * (1) THE x16 ADDRESS SCALE.  Retail computes &tbl[vc] for the 16-byte-stride
 *     table as TWO shifts, `sll rX,vc,3` then `sll rY,rX,1`.  Our IDO -O2
 *     folds the row multiply and the element scale into a single `sll 4`, so
 *     every access to that table is one word short.  This is NOT a recomp
 *     defect: the IRIX IDO 5.2 binaries under qemu-irix folds it identically
 *     (checked on `u16 *p; p = base + (vc << 3);` -- both emit `sll 4`).
 *     Twelve source spellings were screened and all fold: 2-D array
 *     `u16 t[24][8]`, pointer-to-row `u16 (*)[8]`, 16-byte struct array,
 *     `&base[vc*8]`, `base + (vc<<3)`, `(u8 *)base + vc*16`,
 *     `(u8 *)base + ((vc<<3)<<1)`, split statements `n = vc<<3; n <<= 1;`,
 *     u32/const/volatile-qualified bases, and -O1/-O0 (which DO emit the two
 *     shifts, but at the cost of homing every parameter).  The one spelling
 *     that splits them -- a `u8` intermediate index -- pays for it with an
 *     `andi 0xff`.  `IDO52_SKIP_PASSES` has no token for the folding pass and
 *     no `-Wo` flag reaches it.
 *
 * (2) THE `li v0,-1` DELAY-SLOT COPIES.  Retail fills the range-check branch
 *     delay slots by COPYING the `li v0,-1` from the branch target and
 *     advancing the destination one word (as1 branch-target-copy).  Our as1
 *     has a real instruction available for those slots (a hoisted `%hi` lui,
 *     or a register-shuffle `move`) and fills with that instead, losing one
 *     word per affected branch.  Making the referenced global `volatile`
 *     blocks the lui hoist but does not restore the target copy.
 *
 * Everything else lines up mnemonic for mnemonic; the remaining diffs are
 * register fields (paintable) and the s16-parameter sign extension, which
 * retail performs in place in a0/a1/a2 and our recomp routes through t6/t7.
 * No PostProc is used to compensate for (1) or (2) -- an honest short body is
 * recorded instead.
 *
 * 2026-09-19: Hook 41b (IDO52_UGEN_FORCE_TARGET_SAFE=all) makes the s16
 * extension in place in a0 as retail: 1 -> 4/20 exact, Fixup 1, Body 19.
 * cc -S shows ugen emits `mul 8; sll 1` -- retail's `sll 3; sll 1` pair is
 * exactly ugen's output and it is OUR as1 that folds it to `sll 4` (none of
 * NO_PASS1_PROP / KEEP_ANDI / NO_COPY_PROP stops it).  ido52-gas keeps the
 * pair but expands ugen's `blt a0,0` to `slti at; bnez` where retail has
 * `bltz` (GNU as does not special-case a zero immediate; 24-27 words).
 * Remaining: uopt colours r into a1 and moves out0 to a3 (retail: r in v1).
 *
 * 2026-09-22 ido52-gas screen (file left on as1; best gas probe 3/21,
 * Body 24, worse than 4/20).  Retail IS the gas-path shape: both range
 * branches are copy-and-bump (`beqz ; li v0,-1` and `bltz ; li v0,-1`,
 * original `li` kept at 0x8008F690), `sll 3 ; sll 1` unfolded, and
 * `move v0,zero` fills the first `lhu` delay.  Three things stand between:
 *  (a) TOOL: gccwrap's ido_expand_branch_macros spells ugen's `blt $4,0`
 *      as `slti at ; bnez` -- GNU as/ASPSX emit `bltz` for a ZERO
 *      immediate (+1 word).
 *  (b) TOOL: `lhu t0 ; sh t0 ; j $31` -- ASPSX sinks the store into the
 *      `jr` slot with no load-delay nop; GNU as pads the nop first (+1).
 *  (c) C/uopt: `r` is copy-propagated into its first use in the same
 *      block, so the address becomes its own web (a1, forcing out0 to a3)
 *      plus `move v1,a1` (+2).  Every natural spelling keeps it (13 tried:
 *      struct rows, &r[i], r+=, *r++, K&R/-cckr, goto-bad, && guard);
 *      VETO_WEB / NOSPLIT / FORCE_WEB_COLOR / NO_CSE / COPYPROP_SITE=*
 *      inert.  `r = libsnd_spu_regs_ptr; r += vc*8;` gets retail's lw-before-sll
 *      order.  A block boundary after the `r` assignment (`if (1) {...}`
 *      probe) removes the copy, and FORCE_WEB_COLOR=6:v1 then puts r in v1
 *      as retail -- but that is a probe, not source; the real spelling
 *      that separates the def from the first use is still unknown.
 * 2026-09-22 (later): tool gaps (a) and (b) are now modelled as default-off
 * gccwrap flags -- `--ido-branch-zero-imm` (ugen `blt rs,0` -> `bltz`) and
 * `--ido-jr-store-slot` (store whose data was just loaded sinks into the
 * `j $31` slot, no nop).  On ido52-gas with --ido-copy-and-bump
 * --ido-fill-load-delay --ido-fill-macro-lui --ido-branch-zero-imm
 * --ido-jr-store-slot, only (c) is left: natural C is 2/21, Body 23.
 * MEASURED: `r = libsnd_spu_regs_ptr; r += vc * 8;` followed by the two copies
 * inside `if (1) { ... }` (or `while (1) { ...; return 0; }`) is
 * BYTE-PERFECT 21/21, Body 21, no Fixup, with NO hook beyond 41b -- the
 * block boundary alone puts r in v1.  Not adopted: that wrapper is a
 * block-boundary probe, not believable source (do{}while(0), a label, a
 * goto join, `&&` and an if/else split do NOT reproduce it).  The spelling
 * that separates r's def from its first use is still the open question.
 * @SymbolStart: 0x8008F648
 * @SymbolSize: 84
 * @Compiler: ido52-gas
 * @CompileFlags: --ido-copy-and-bump --ido-fill-load-delay --ido-fill-macro-lui --ido-branch-zero-imm --ido-jr-store-slot
 * @CompileEnv: IDO52_UGEN_FORCE_TARGET_SAFE=all
 * @PostProc: strip_leading_instructions --count auto
 * @Measured: 2026-09-19 4/21 exact (20.0%), compiled 19 vs target 21 words, Fixup 1 -- SEMI
 * 2026-09-17 HOOK 36 BLIND SWEEP: NO GAIN (honest negative -- do not re-run
 *   unless the body changes).  7 temp-register requests; every one of the
 *   first 7 x every register its own TRACE free list offers there was
 *   probed (62 probes, greedy, 3 rounds), guarded on an unchanged word
 *   count and an unchanged Fixup.  4 of those occurrences CHANGED the
 *   object, so the hook FIRES here -- it simply has no better choice to
 *   make.  Best stays 1/20.
 */
/* SsUtGetDetVVol -- read two voice-register halves (PC override of
 * src/main/matched/SsUtGetDetVVol.c).  Statement for statement the matched
 * function; the ONE divergence is addressing (mv_state.h doctrine):
 * libsnd_spu_regs_ptr is a 4-byte PS1 pointer global whose native 8-byte load folds
 * the neighbouring retail ramp table into the high half.  Read as its
 * PS1-width u32 and zero-extended here. */
#include "types.h"

extern u32 libsnd_spu_regs_ptr;   /* PS1-width pointer handle (see header) */

s32 SsUtGetDetVVol(s16 vc, u16 *out0, u16 *out1)
{
    u16 *r;

    if (vc < 24) {
        if (vc >= 0) {
            r = (u16 *)(unsigned long)libsnd_spu_regs_ptr;
            r += vc * 8;
            if (1) {
                *out0 = r[0];
                *out1 = r[1];
                return 0;
            }
        }
    }
    return -1;
}
