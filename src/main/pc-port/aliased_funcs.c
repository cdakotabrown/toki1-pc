/* aliased_funcs.c — trampolines to rename-only pc-port aliases.
 *
 * Many matched C files live under real Konami/Sony symbol names (e.g.
 * "pre_syogatu_main.c" implements the function at 0x800618B8). PS1 callers
 * reference the function by its hex address `pre_syogatu_main`, and the
 * empty stub in pc/src/missing_funcs.c shadows the real implementation
 * at link time. This file defines `func_XXXXXXXX` naked tail-call
 * trampolines that jmp to the real named implementation, the same
 * pattern as lowercase_aliases.c.
 *
 * 93 matched + 3 semi-matched aliases discovered 2026-04-18.
 * SD_BranchCTRL (dma_execute) deliberately excluded — touches PS1 I/O
 * registers at 0x1F801080+ which segfaults on PC.
 */

/* semi-matched aliases (batch 27) */
/* pc-port-defined aliases (batch 28) */

/* matched aliases (batch 26) */
/* 2026-10-19: func_800457FC is the 8-byte prologue-hoist fragment of the
 * CD queue worker (semi-matched/func_800457FC.c is its ASM-ONLY marker: a
 * call to 0x800457FC runs the hoist head AND the host body at 0x8004580C).
 * The generated `return 0` stub swallowed the worker -- the PC frame hook
 * (pc/src/main.c) calls it once per frame, so the matched cdq_command_process
 * never ran, CdSync(1) never got polled past the movie's stream state, and
 * boot_data_load waited forever on get_CD_State2() & 1.  Forward it to the
 * host like any rename-only alias. */
__attribute__((naked)) void func_800457FC(void) { __asm__("jmp cdq_command_process"); }
__attribute__((naked)) void func_8007A64C(void) { __asm__("jmp FUN_8007A64C"); }

/* HAL-backed aliases (batch 33) — HAL_EXCLUDES removes the matched/ _spu_close.c,
 * _spu_write.c, SsSeqCalledTbyT.c from the build; the symbols come from pc/hal. */
/* Overlay-referenced aliases (batch 35) — called from overlay_ETC/TACO at
 * paths gated on uninitialized locals (GCC -O2 elides them today, but wire
 * them up in case future changes reach the calls). */
/* batch 37: swap pc-port stub for matched impl. icon_init at 0x8006523C
 * populates sprite-table slots 64/80 with CLUT/TIM pointers and uploads
 * three palette rows to VRAM (288,497..499) — required for title bg. */
/* o.bin high-confidence renames (batch 36) */
