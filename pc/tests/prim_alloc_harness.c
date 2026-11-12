/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Actual PC allocator + actual native OT traversal, without game assets.
 * Reuse the established real-driver fixture and its fail-if-called backend
 * stubs; no copied renderer or unresolved-symbol suppression. */
#define main gpu_ot_fixture_main
#include "gpu_ot_harness.c"
#undef main

u8 screen_w[64] __attribute__((aligned(8)));
#ifndef PRIM_ALLOC_SOURCE
#define PRIM_ALLOC_SOURCE "../../src/main/pc-port/prim_alloc.c"
#endif
#include PRIM_ALLOC_SOURCE

int main(int argc, char **argv)
{
    void *mapping;
    u32 *first, *second, *head;
    u32 base, expected;
    int x, y, result, i;
    static const s32 requests[] = {36, 12, 12, 36, 12, 12};
    static const s32 cursors[] = {0, 36, 48, 60, 96, 108, 120};

    if (argc != 2) return 90;
#ifndef MAP_FIXED_NOREPLACE
    fprintf(stderr, "MAP_FIXED_NOREPLACE required; no destructive fallback\n");
    return 91;
#else
    mapping = mmap((void *)0x80010000UL, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (mapping == MAP_FAILED || mapping != (void *)0x80010000UL) return 92;
    base = (u32)(uintptr_t)mapping;
    memset(screen_w, 0, sizeof(screen_w));
    *(u32 *)(screen_w + 36) = base;
    *(u32 *)(screen_w + 48) = base + 1024;
    /* Adjacent size is not the high half of a native pointer. */
    *(u32 *)(screen_w + 40) = 0x10c00;
    result = 0;
    if (!strcmp(argv[1], "zero")) {
        *(s32 *)(screen_w + 32) = 3;
        first = (u32 *)prim_alloc(0, 0);
        result = first != (u32 *)(uintptr_t)(base + 12) ||
                 *(s32 *)(screen_w + 32) != 3;
    } else if (!strcmp(argv[1], "cursor")) {
        *(s32 *)(screen_w + 32) = 3;
        first = (u32 *)prim_alloc(13, 0);
        result = first != (u32 *)(uintptr_t)(base + 12) ||
                 *(s32 *)(screen_w + 32) != 19;
    } else if (!strcmp(argv[1], "round")) {
        first = (u32 *)prim_alloc(1, 0);
        second = (u32 *)prim_alloc(4, 0);
        result = first != mapping ||
                 second != (u32 *)(uintptr_t)(base + 16);
        first = (u32 *)prim_alloc(5, 0);
        result |= first != (u32 *)(uintptr_t)(base + 32) ||
                  *(s32 *)(screen_w + 32) != 16;
    } else if (!strcmp(argv[1], "banks")) {
        first = (u32 *)prim_alloc(12, 0);
        second = (u32 *)prim_alloc(8, 1);
        result = first != mapping ||
                 second != (u32 *)(uintptr_t)(base + 1024) ||
                 *(s32 *)(screen_w + 32) != 12 ||
                 *(s32 *)(screen_w + 44) != 8 ||
                 *(u32 *)(screen_w + 40) != 0x10c00;
    } else if (!strcmp(argv[1], "shade")) {
        for (i = 0; i < 6; i++) {
            first = (u32 *)prim_alloc(requests[i], 0);
            if (first != (u32 *)(uintptr_t)(base + cursors[i] * 4) ||
                *(s32 *)(screen_w + 32) != cursors[i + 1]) result = 1;
        }
    } else if (!strcmp(argv[1], "render")) {
        first = (u32 *)prim_alloc(12, 0);
        second = (u32 *)prim_alloc(12, 0);
        if (first == second) {
            fprintf(stderr, "consecutive allocations alias; raw insertion would self-link\n");
            munmap(mapping, 4096);
            return 1;
        }
        head = (u32 *)(uintptr_t)(base + 512);
        *head = 0x00ffffff;
        pc_ot_setaddr(head, TERMINATOR);
        first[0] = second[0] = 0x02000000;
        first[1] = 0x680000ff;
        first[2] = (9u << 16) | 7u;
        second[1] = 0x6800ff00;
        second[2] = (9u << 16) | 10u;
        raw_link(first, *head & 0xffffff);
        raw_link(head, raw_token(first));
        raw_link(second, *head & 0xffffff);
        raw_link(head, raw_token(second));
        for (y = 0; y < VRAM_HEIGHT; y++)
            for (x = 0; x < VRAM_WIDTH; x++) g_vram[y][x] = 0x1234;
        DrawOTag(head);
        for (y = 0; y < VRAM_HEIGHT; y++) {
            for (x = 0; x < VRAM_WIDTH; x++) {
                expected = y == 9 && x == 7 ? 0x001f :
                           y == 9 && x == 10 ? 0x03e0 : 0x1234;
                if (g_vram[y][x] != expected) result = 1;
            }
        }
    } else result = 93;
    if (result) fprintf(stderr, "allocator contract failed: %s cursor=%d\n",
                        argv[1], *(s32 *)(screen_w + 32));
    if (munmap(mapping, 4096)) return 94;
    return result;
#endif
}
