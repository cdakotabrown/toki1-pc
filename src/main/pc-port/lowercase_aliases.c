/* lowercase_aliases.c — lowercase-hex name aliases for PC-port linking.
 *
 * Several matched source files contain typo'd lowercase hex in function-
 * name references (e.g. `parameter_show_init` instead of `parameter_show_init`). C is
 * case-sensitive, so on PC these lowercase calls resolved to empty stubs
 * in missing_funcs.c while the real uppercase-named implementations sat
 * unused. This file defines the lowercase names as naked tail-call
 * trampolines that `jmp` to the real uppercase implementations. All
 * arguments are forwarded transparently via the x86_64 SysV ABI registers.
 *
 * Affects 55 functions, discovered during the 2026-04-18 thunk audit.
 */

/* Keep the lowercase ABI alias for callers that still spell the address. */
/* Added 2026-04-19: batch stub-decomp pass (339→semi-matched) */
__attribute__((naked)) void func_800b4830(void) { __asm__("jmp func_800B4830"); }
__attribute__((naked)) void func_800b4860(void) { __asm__("jmp func_800B4860"); }
__attribute__((naked)) void func_800b4880(void) { __asm__("jmp func_800B4880"); }
__attribute__((naked)) void func_800b4890(void) { __asm__("jmp func_800B4890"); }
__attribute__((naked)) void func_800b48a0(void) { __asm__("jmp func_800B48A0"); }
__attribute__((naked)) void func_800b48d0(void) { __asm__("jmp func_800B48D0"); }
__attribute__((naked)) void func_800b5650(void) { __asm__("jmp func_800B5650"); }
__attribute__((naked)) void func_800b5920(void) { __asm__("jmp func_800B5920"); }
__attribute__((naked)) void func_800b5940(void) { __asm__("jmp func_800B5940"); }
