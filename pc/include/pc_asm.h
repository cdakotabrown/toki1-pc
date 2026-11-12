/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pc_asm.h -- symbol-type directives for top-level __asm__ blocks.
 *
 * ELF (Linux) marks a hand-written symbol `.type name, @function` /
 * `@object`; the COFF assembler of the Windows build rejects that syntax
 * and spells a function `.def name; .scl 2; .type 32; .endef` (COFF has no
 * object type).  Use as string pieces:
 *     ".globl f\n" PC_ASM_TYPE_FN("f") "f:\n ..."
 * docs/pc-port/WINDOWS-BUILD.md. */
#ifndef PC_ASM_H
#define PC_ASM_H

#ifdef _WIN32
#define PC_ASM_TYPE_FN(name)  ".def " name "; .scl 2; .type 32; .endef\n"
#define PC_ASM_TYPE_OBJ(name) ""
#define PC_ASM_SIZE(name)     ""
#else
#define PC_ASM_TYPE_FN(name)  ".type " name ", @function\n"
#define PC_ASM_TYPE_OBJ(name) ".type " name ", @object\n"
/* after a function's last instruction: its ELF size */
#define PC_ASM_SIZE(name)     ".size " name ", .-" name "\n"
#endif

#endif
