/* SPDX-License-Identifier: GPL-3.0-or-later */
/* win_heap.c -- the Windows build's malloc placement check.
 *
 * Game C keeps host addresses in 32-bit integers (pc/src/main.c, "the game
 * runs with its stack and heap below 2 GB"): an s32 global holding a buffer
 * address, `(s32)pages` passed for a pointer.  Linux keeps every malloc on
 * the brk heap after .bss.  On Windows the exe is linked without ASLR at
 * 0x400000 (pc/Makefile), and the CRT heap and VirtualAlloc then allocate
 * bottom-up from low addresses, so blocks land far below 2 GB -- but that
 * is the OS's policy, not a guarantee.  Every malloc/calloc/realloc of the
 * executable (game, HAL, the statically linked libstdc++ of the SPU core
 * and the GPU shim) goes through these wrappers (-Wl,--wrap=...), which
 * fail loudly instead of handing out an address an s32 would truncate.
 * free() needs no wrapper: the blocks are the CRT's own.
 */
#ifdef _WIN32
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <windows.h>
#include "execinfo.h"

void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t m);
void *__real_realloc(void *p, size_t n);

#define PC_WIN_LOW_LIMIT 0x80000000ull   /* below the PS1 RAM block */

static volatile LONG g_fatal;    /* the report below is running */

static void *checked(void *p, size_t n, const char *what)
{
    /* while the fatal report runs, its own allocations (the backtrace's
     * symbol table) pass unchecked: the process is about to abort */
    if (g_fatal) return p;
    if (p && (uintptr_t)p + n > PC_WIN_LOW_LIMIT) {
        g_fatal = 1;
        char msg[256];
        snprintf(msg, sizeof msg,
                 "[HEAP] FATAL: %s(%llu) returned %p, above 2 GB: game code "
                 "keeps host addresses in 32-bit integers\n",
                 what, (unsigned long long)n, p);
        fputs(msg, stderr);
        fflush(stderr);
        {
            /* who asked (win_dl.c names the exe's global functions) */
            void *bt[32];
            int k = backtrace(bt, 32);
            backtrace_symbols_fd(bt, k, 2);
        }
        abort();
    }
    return p;
}

void *__wrap_malloc(size_t n) { return checked(__real_malloc(n), n, "malloc"); }
void *__wrap_calloc(size_t n, size_t m) { return checked(__real_calloc(n, m), n * m, "calloc"); }
void *__wrap_realloc(void *p, size_t n) { return checked(__real_realloc(p, n), n, "realloc"); }
#endif
