/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * ptr_validate.c — pc_ptr_is_mapped: check whether a memory range is
 * currently mapped, using mincore(2) (VirtualQuery on Windows). Used by HAL
 * guards (e.g. LoadImage) to skip operations on truncated PS1 pointers that
 * would otherwise SEGV when the host kernel sees an unmapped page.
 *
 * The size is `unsigned long`, as pc/include/hal.h and every caller's
 * extern declare it.  (It was size_t here: the same type on LP64 Linux, but
 * on LLP64 Windows a caller passing a 32-bit unsigned long to a 64-bit
 * size_t parameter leaves the register's upper half undefined.)
 */
#include <stddef.h>
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>

int pc_ptr_is_mapped(const void *p, unsigned long bytes) {
    uintptr_t address, last;
    MEMORY_BASIC_INFORMATION mbi;

    if (!p) return 0;
    address = (uintptr_t)p;
    if (bytes == 0) bytes = 1;
    if ((uintptr_t)bytes - 1 > UINTPTR_MAX - address) return 0;
    last = address + bytes - 1;
    /* committed pages, any protection (mincore's "mapped") */
    while (address <= last) {
        uintptr_t end;
        if (!VirtualQuery((const void *)address, &mbi, sizeof mbi)) return 0;
        if (mbi.State != MEM_COMMIT) return 0;
        end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (end <= address) return 0;
        address = end;
    }
    return 1;
}
#else
#include <unistd.h>
#include <sys/mman.h>

int pc_ptr_is_mapped(const void *p, unsigned long bytes) {
    long ps;
    uintptr_t address, start, last;
    size_t pages, batch;
    unsigned char vec[64];

    if (!p) return 0;
    address = (uintptr_t)p;
    /* Keep the existing zero-length point query, without range overflow. */
    if (bytes == 0) bytes = 1;
    if (bytes - 1 > UINTPTR_MAX - address) return 0;
    ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) ps = 4096;
    start = address - address % (uintptr_t)ps;
    last = address + bytes - 1;
    last -= last % (uintptr_t)ps;
    for (;;) {
        pages = (size_t)((last - start) / (uintptr_t)ps) + 1;
        batch = pages > sizeof(vec) ? sizeof(vec) : pages;
        if (mincore((void *)start, batch * (size_t)ps, vec) != 0) {
            return 0;
        }
        if (batch == pages) return 1;
        start += batch * (size_t)ps;
    }
    /* This proves mapping only, not allocation bounds or permissions. */
}
#endif

/* hal_ps1_ptr: a PS1 address that reached the HAL through an `s32`.
 *
 * The game stores addresses in s32 globals and parameters (load_image_rect's
 * 5th argument, cdq_cur_addr, the CD read destinations).  On the PS1 that is
 * free; on x86-64 the implicit s32 -> pointer conversion SIGN-EXTENDS
 * 0x801A0000 to 0xFFFFFFFF801A0000, which is kernel space.  The PS1 RAM
 * windows (pc/src/main.c) are mapped at their zero-extended addresses, so a
 * pointer whose upper half is all ones and whose lower half is a KSEG0 RAM
 * address is folded back to the mapped window.  Any other pointer is returned
 * unchanged. */
void *hal_ps1_ptr(const void *p) {
    uintptr_t v = (uintptr_t)p;
    if ((v >> 32) == 0xFFFFFFFFu && (v & 0xFFFFFFFFu) >= 0x80000000u && (v & 0xFFFFFFFFu) < 0x80800000u)
        return (void *)(v & 0xFFFFFFFFu);
    return (void *)v;
}
