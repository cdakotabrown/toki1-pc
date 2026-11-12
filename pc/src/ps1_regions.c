/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * ps1_regions.c -- the fixed PS1 address ranges the port maps, mapped
 * before the host heap can grow over them.
 *
 * Game code dereferences fixed PS1 addresses: the 2 MB of RAM at
 * 0x80000000 (every game global, pc/ld/psx_globals.syms) and the
 * hardware-register page at 0x1F801000 (SPU voice registers, DMA, MDEC
 * reached through pointer cells such as libpress_dma0_chcr_reg_ptr).  main.c used to map
 * them with MAP_FIXED from real_main(), after hal_init had malloc'd.  The
 * port keeps malloc on the brk heap (main.c: host pointers must survive a
 * round trip through an s32), the brk start is randomised over 1 GB on
 * x86-64, and M_TOP_PAD grows the heap in 64 MB steps: a heap that started
 * below 0x1F801000 grew over it, and MAP_FIXED then silently replaced a
 * heap page with the register page -- malloc'd memory and the SPU/DMA
 * register mirror aliased (seen in a plain power-on run).
 *
 * Now:
 *  - pc_map_fixed_regions() is the first thing main() does.  Every range
 *    is mapped MAP_FIXED_NOREPLACE: an occupied range is a hard error
 *    naming the range and the mapping in the way, never a replacement.
 *    Once mapped, the kernel's brk refuses to grow the heap into it.
 *  - pc_heap_room() says how far the brk heap can grow before the next
 *    fixed range.  When it is below PC_HEAP_ROOM_MIN, main() re-executes
 *    the process once with address randomisation off (the brk heap then
 *    starts right after .bss, ~480 MB below the register page), the
 *    mechanism the debug toolkit already uses.
 * Test: pc/tools/test_ps1_regions.py (pc/tests/ps1_regions_harness.c).
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#endif

typedef struct {
    uintptr_t addr;
    size_t size;
    const char *name;
} Ps1Region;

/* The PS1 scratchpad (0x1F800000) is not mapped. */
static const Ps1Region k_regions[] = {
    { 0x1F801000u, 0x1000u,   "PSX MMIO (hardware registers)" },
    { 0x80000000u, 0x200000u, "PS1 RAM" },
};
#define N_REGIONS (sizeof k_regions / sizeof k_regions[0])

#ifdef _WIN32
/* Windows: the same ranges
 * from VirtualAlloc at fixed addresses.  An allocation base must be 64 KB
 * aligned, so the register page is reserved as the 64 KB at 0x1F800000 and
 * only the page at 0x1F801000 is committed: the rest (the PS1 scratchpad at
 * 0x1F800000 among it) stays inaccessible, as unmapped memory is on Linux.
 * There is no brk heap to collide with: the CRT heap is the OS's, and
 * pc/hal/win/win_heap.c checks where its blocks land. */
#include <windows.h>

static void report_holder(uintptr_t addr)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((const void *)addr, &mbi, sizeof mbi))
        fprintf(stderr, "[INIT]   in the way: base %p size 0x%llx state 0x%lx\n",
                mbi.AllocationBase, (unsigned long long)mbi.RegionSize,
                (unsigned long)mbi.State);
}

int pc_map_fixed_regions(void)
{
    size_t i;
    for (i = 0; i < N_REGIONS; i++) {
        const Ps1Region *r = &k_regions[i];
        uintptr_t base = r->addr & ~(uintptr_t)0xFFFF;
        size_t span = ((r->addr + r->size + 0xFFFF) & ~(uintptr_t)0xFFFF) - base;
        void *res = VirtualAlloc((void *)base, span, MEM_RESERVE, PAGE_NOACCESS);
        void *p = res ? VirtualAlloc((void *)r->addr, r->size, MEM_COMMIT, PAGE_READWRITE) : NULL;
        if (!p || (uintptr_t)p != r->addr) {
            DWORD e = GetLastError();
            fprintf(stderr, "[INIT] FATAL: cannot map %s at 0x%08lx-0x%08lx: error %lu\n",
                    r->name, (unsigned long)r->addr, (unsigned long)(r->addr + r->size),
                    (unsigned long)e);
            report_holder(r->addr);
            return -1;
        }
    }
    return 0;
}

/* no brk heap: nothing can grow into the ranges */
unsigned long pc_heap_room(void)
{
    return (unsigned long)-1;
}
#else
/* The mapping in /proc/self/maps that covers `addr`, for the error. */
static void report_holder(uintptr_t addr, size_t size)
{
    char line[512];
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        if (sscanf(line, "%lx-%lx", &lo, &hi) == 2 && lo < addr + size && hi > addr)
            fprintf(stderr, "[INIT]   in the way: %s", line);
    }
    fclose(f);
}

/* 0 when every range is mapped; -1 (after naming the range and what holds
 * it on stderr) when one is already occupied or cannot be mapped. */
int pc_map_fixed_regions(void)
{
    size_t i;
    for (i = 0; i < N_REGIONS; i++) {
        const Ps1Region *r = &k_regions[i];
        void *p = mmap((void *)r->addr, r->size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == MAP_FAILED || (uintptr_t)p != r->addr) {
            int e = errno;
            if (p != MAP_FAILED) munmap(p, r->size);   /* pre-4.17 kernel: a hint */
            fprintf(stderr, "[INIT] FATAL: cannot map %s at 0x%08lx-0x%08lx: %s\n",
                    r->name, (unsigned long)r->addr, (unsigned long)(r->addr + r->size),
                    p == MAP_FAILED ? strerror(e) : "kernel placed it elsewhere");
            report_holder(r->addr, r->size);
            return -1;
        }
    }
    return 0;
}

/* Bytes the brk heap can grow from its current end before the next fixed
 * range above it (the ranges below it cannot be reached). */
unsigned long pc_heap_room(void)
{
    uintptr_t brk_end = (uintptr_t)sbrk(0);
    uintptr_t limit = UINTPTR_MAX;
    size_t i;
    for (i = 0; i < N_REGIONS; i++)
        if (k_regions[i].addr >= brk_end && k_regions[i].addr < limit)
            limit = k_regions[i].addr;
    for (i = 0; i < N_REGIONS; i++)       /* the heap already inside one */
        if (k_regions[i].addr < brk_end && k_regions[i].addr + k_regions[i].size > brk_end)
            return 0;
    return limit == UINTPTR_MAX ? (unsigned long)-1 : (unsigned long)(limit - brk_end);
}

#endif /* _WIN32 */

/* The game's heap use is ~64 MB (scene 18, 1,500 frames; M_TOP_PAD is
 * 64 MB); a quarter gigabyte leaves room for growth.  With randomisation
 * off the room below the register page is ~480 MB. */
const unsigned long PC_HEAP_ROOM_MIN = 256ul << 20;
