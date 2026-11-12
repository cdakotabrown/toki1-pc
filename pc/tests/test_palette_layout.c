/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Standalone regression: retail CLUT records keep their 8-byte stride.
 * Uses a high 32-bit address to exercise zero extension as well as layout. */
#include <sys/mman.h>
#include <string.h>
#include "types.h"

u8 screen_w[8192];
u8 palette_queue_n;
static int calls, failed;
static u32 *expected;
extern void palette_load_vram(s32);

void load_image_rect(u16 x, u16 y, u16 w, s32 h, u32 *data)
{
    if (x != 0 || y != 487 || w != 256 || h != 1 || data != expected)
        failed = 1;
    calls++;
}

int test_palette_layout(void)
{
    u32 *clut;
    int mode, i, edge, enabled;
    clut = mmap((void *)0x83000000UL, 4096, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (clut == MAP_FAILED) return 1;
    for (mode = 0; mode < 5; mode++) {
        memset(screen_w, 0, sizeof(screen_w));
        for (i = 0; i < 1024; i++) clut[i] = 0x81238123;
        /* Two adjacent entries; their packed metadata must not become the
         * upper half of a native pointer, or shift the second entry. */
        for (i = 0; i < 2; i++) {
            *(u32 *)(screen_w + 4904 + i * 8) = (u32)(unsigned long)clut;
            screen_w[4908 + i * 8] = 7;
            screen_w[4910 + i * 8] = (u8)mode;
            screen_w[4911 + i * 8] = 1;
        }
        screen_w[5160] = palette_queue_n = 2;
        expected = clut + 128;
        calls = failed = 0;
        palette_load_vram(0);
        if (calls != 2 || failed || palette_queue_n != 0) return 2;
        edge = mode == 2 ? 120 : mode == 4 ? 112 : 0;
        for (i = 0; i < 128; i++) {
            enabled = mode == 1 || mode == 3 ||
                      ((mode == 2 || mode == 4) && i >= edge);
            if (clut[128 + i] != (enabled ? 0x81238123u : 0x01230123u))
                return 3;
        }
        if (clut[127] != 0x81238123 || clut[256] != 0x81238123) return 4;
    }
    munmap(clut, 4096);
    return 0;
}
