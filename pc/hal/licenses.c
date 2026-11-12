/* SPDX-License-Identifier: GPL-3.0-or-later */
/* licenses.c -- the licence texts that travel inside the executable
 * (pc/licenses/*.txt and the project's own MIT LICENSE, embedded with
 * .incbin): shown by `tokimeki-pc --licenses` and the settings menu's
 * "About / licences" page (pc/hal/setup_screen.c).  This is the only copy
 * a player gets: the download holds the program and README.txt only
 * (pc/tools/package_windows.py, package_linux.py; 2026-11-07). */
#include <stddef.h>
#include <string.h>

#ifdef _WIN32
#define PC_LIC_SECTION ".section .rdata,\"dr\"\n"
#define PC_LIC_SYM "pc_license_text"
#else
#define PC_LIC_SECTION ".section .rodata\n"
#define PC_LIC_SYM "pc_license_text"
#endif

/* The project's own licence: LICENSE of the checkout that holds the
 * decompiled C (toki1-decomp; pc/Makefile passes its path when that is a
 * sibling checkout, docs/REPO-SPLIT-PLAN.md). */
#ifndef PC_PROJECT_LICENSE
#define PC_PROJECT_LICENSE "LICENSE"
#endif

#define PC_LIC_RULE "\\n\\n==============================================================================\\n"

__asm__(PC_LIC_SECTION
        ".globl " PC_LIC_SYM "\n"
        PC_LIC_SYM ":\n"
        ".incbin \"pc/licenses/THIRD-PARTY.txt\"\n"
        ".ascii \"" PC_LIC_RULE "\"\n"
        ".incbin \"pc/licenses/SDL2.txt\"\n"
        ".ascii \"" PC_LIC_RULE "\"\n"
        ".incbin \"pc/licenses/TinySoundFont.txt\"\n"
        ".ascii \"" PC_LIC_RULE "\"\n"
        ".incbin \"pc/licenses/stb.txt\"\n"
        ".ascii \"" PC_LIC_RULE "\"\n"
        ".incbin \"pc/licenses/dr_libs.txt\"\n"
        ".ascii \"" PC_LIC_RULE "\"\n"
        ".incbin \"pc/licenses/MinGW-runtime.txt\"\n"
        ".ascii \"" PC_LIC_RULE "\"\n"
        ".incbin \"pc/licenses/GPL-3.0.txt\"\n"
        ".ascii \"" PC_LIC_RULE "\"\n"
        ".ascii \"The project's own licence (the sources this program is built from)\\n\\n\"\n"
        ".incbin \"" PC_PROJECT_LICENSE "\"\n"
        ".byte 0\n"
        ".text\n");

extern const char pc_license_text[];

const char *pc_licenses(void) { return pc_license_text; }
