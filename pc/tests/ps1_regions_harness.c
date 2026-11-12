/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ps1_regions_harness.c -- pc/src/ps1_regions.c against a heap that has
 * grown over the PS1 register page (pc/tools/test_ps1_regions.py).
 *
 * usage: ps1_regions_harness MODE
 *   heap-first-new  grow the brk heap across 0x1F801000, mark the heap page,
 *                   then pc_map_fixed_regions(): must refuse (exit 0 when it
 *                   refused and the heap page kept its data)
 *   heap-first-old  the same heap, then the old main.c mapping (MAP_FIXED):
 *                   prints whether the heap page's data survived -- the
 *                   negative control, it does not
 *   map-first       pc_map_fixed_regions() first, then try to grow the brk
 *                   heap across the page: the kernel must refuse
 * Run under `setarch -R` so the brk heap starts right after .bss, below the
 * register page.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern int pc_map_fixed_regions(void);
extern unsigned long pc_heap_room(void);

#define MMIO 0x1F801000ul

static int grow_over_mmio(void)
{
    uintptr_t cur = (uintptr_t)sbrk(0);
    if (cur >= MMIO) {
        printf("SKIP brk starts above the page (%#lx)\n", (unsigned long)cur);
        return -1;
    }
    if (sbrk((intptr_t)(MMIO + 0x2000 - cur)) == (void *)-1) {
        printf("brk growth refused: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!strcmp(mode, "heap-first-new") || !strcmp(mode, "heap-first-old")) {
        volatile uint32_t *page = (volatile uint32_t *)MMIO;
        int r = grow_over_mmio();
        if (r) return r < 0 ? 77 : 2;
        page[0] = 0xC0FFEE11u;                    /* a malloc'd block's data */
        if (!strcmp(mode, "heap-first-old")) {
            void *m = mmap((void *)MMIO, 0x1000, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            printf("old mapping %s; heap data %s\n", m == MAP_FAILED ? "failed" : "succeeded",
                   page[0] == 0xC0FFEE11u ? "intact" : "LOST");
            return 0;
        }
        r = pc_map_fixed_regions();
        printf("map %s; heap data %s\n", r ? "refused" : "succeeded",
               page[0] == 0xC0FFEE11u ? "intact" : "LOST");
        return r != 0 && page[0] == 0xC0FFEE11u ? 0 : 1;
    }
    if (!strcmp(mode, "map-first")) {
        int r;
        if (pc_map_fixed_regions()) return 2;
        printf("room %lu MB\n", pc_heap_room() >> 20);
        r = grow_over_mmio();
        if (r < 0) return 77;
        printf("heap over the page %s\n", r ? "refused" : "ALLOWED");
        return r ? 0 : 1;
    }
    return 3;
}
