/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Real gpu.c OT submission regressions. No renderer model or game assets.
 * Each invocation is a private child process; fixed PS1 mappings never affect
 * the Python runner. Build with section GC, not unresolved-symbol suppression.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#ifndef GPU_SOURCE
#define GPU_SOURCE "../hal/gpu.c"
#endif
#include GPU_SOURCE

u8 graph_env[128];

/* No system clock here: libgpu's CPU phases (pc/hal/gpu_cpu.c) take no
 * time. Their timing is pinned by test_gpu_reset's sdk-cpu case. */
void gpu_cpu_submit_entry(u32 to_alarm, u32 to_mask)
{
    (void)to_alarm;
    (void)to_mask;
}
void gpu_cpu_masked(u32 masked, u32 to_return)
{
    (void)masked;
    (void)to_return;
}

/* Required debug storage, not replacement submission/rendering functions. */
int g_frame_prim_count;
int g_frame_sprt_count;
int g_frame_tile_count;
int g_frame_poly_count;
int g_frame_dr_mode_count;

/* DrawOTag contains backend branches even with MDFN_GPU=0 at runtime.
 * Match mdfn_gpu_shim.h exactly and fail if any stub is actually called.
 * Real native rendering and texture sampling remain linked and unchanged.
 */

#ifndef GPU_REAL_BACKEND
static void unexpected_backend(void)
{
    fprintf(stderr, "unexpected Mednafen backend call\n");
    exit(80);
}
void mdfn_set_drawing_area(int x0, int y0, int x1, int y1)
{
    (void)x0; (void)y0; (void)x1; (void)y1;
    unexpected_backend();
}
void mdfn_set_draw_offset(int x, int y)
{
    (void)x; (void)y;
    unexpected_backend();
}
void mdfn_set_texpage(uint16_t packed)
{
    (void)packed;
    unexpected_backend();
}
void mdfn_set_clut(uint16_t packed)
{
    (void)packed;
    unexpected_backend();
}
void mdfn_set_texwindow(uint8_t tww, uint8_t twh, uint8_t twx, uint8_t twy)
{
    (void)tww;
    (void)twh;
    (void)twx;
    (void)twy;
    unexpected_backend();
}
void mdfn_set_mask(int set_or, int eval_and)
{
    (void)set_or;
    (void)eval_and;
    unexpected_backend();
}
void mdfn_clear_cache(void)
{
    unexpected_backend();
}
void mdfn_invalidate_tex_cache(void)
{
    unexpected_backend();
}
void mdfn_draw_triangle(const MdfnTri *triangle)
{
    (void)triangle;
    unexpected_backend();
}
void mdfn_draw_sprite(const MdfnSprite *sprite)
{
    (void)sprite;
    unexpected_backend();
}
void mdfn_draw_line(const MdfnLine *line)
{
    (void)line;
    unexpected_backend();
}

#endif /* GPU_REAL_BACKEND */

#define RAW_MASK 0x00ffffffU
#define TERMINATOR ((void *)(uintptr_t)RAW_MASK)

static u32 raw_token(const void *pointer)
{
    return (u32)((uintptr_t)pointer & RAW_MASK);
}

/* Emulate a matched PS1 producer's header write, not pc_ot_setaddr. */
static void raw_link(u32 *tag, u32 token)
{
    *tag = (*tag & 0xff000000U) | (token & RAW_MASK);
}

static int expect_pointer(u32 *tag, const void *expected, const char *stage)
{
    void *actual;
    actual = pc_ot_getaddr(tag);
    if (actual == expected) return 0;
    fprintf(stderr, "%s: got %p, expected %p\n", stage, actual, expected);
    return 1;
}

/* Getter raw fallback may return a 24-bit token or its canonical KSEG0
 * address. DrawOTag must resolve it before dereference (tested separately).
 * Neither representation may retain an invalidated full native association.
 */
static int expect_raw(u32 *tag, u32 token, const void *obsolete,
                      const char *stage)
{
    void *actual;
    uintptr_t value;
    actual = pc_ot_getaddr(tag);
    value = (uintptr_t)actual;
    if (actual != obsolete &&
        (value == token || value == ((uintptr_t)0x80000000U | token)))
        return 0;
    fprintf(stderr, "%s: got %p, raw=%06x obsolete=%p\n",
            stage, actual, token, obsolete);
    return 1;
}

static int check_links(const char *name)
{
    u32 tag[4] = {0};
    u32 native_a[4] = {0};
    u32 native_b[4] = {0};
    u32 original, replacement;
    int result;

    if ((uintptr_t)native_a <= UINT32_MAX) {
        fprintf(stderr, "fixture requires a full 64-bit native pointer\n");
        return 81;
    }
    tag[0] = 0x5a000000U;
    pc_ot_setaddr(tag, native_a);
    original = raw_token(native_a);
    replacement = original == 0x100200U ? 0x100400U : 0x100200U;
    if (tag[0] != (0x5a000000U | original)) return 2;
    if (expect_pointer(tag, native_a, "initial cache")) return 3;

    if (!strcmp(name, "native")) return 0;
    if (!strcmp(name, "len")) {
        tag[0] = (tag[0] & RAW_MASK) | 0x03000000U;
        return expect_pointer(tag, native_a, "length-only mutation");
    }
    if (!strcmp(name, "terminator") ||
        !strcmp(name, "terminator-restore")) {
        raw_link(tag, RAW_MASK);
        result = expect_pointer(tag, TERMINATOR, "raw terminator");
        if (result || !strcmp(name, "terminator")) return result;
        raw_link(tag, original);
        return expect_raw(tag, original, native_a, "restore after terminator");
    }

    raw_link(tag, replacement);
    result = expect_raw(tag, replacement, native_a, "raw next mutation");
    if (result || !strcmp(name, "raw")) return result;
    if (!strcmp(name, "restore")) {
        /* The mutation is observed before restoring the old token. Unseen
         * mutate-and-restore between reads cannot be detected by this API. */
        raw_link(tag, original);
        return expect_raw(tag, original, native_a, "restore after raw next");
    }
    if (!strcmp(name, "retarget")) {
        pc_ot_setaddr(tag, native_b);
        if (tag[0] != (0x5a000000U | raw_token(native_b))) return 4;
        return expect_pointer(tag, native_b, "explicit native retarget");
    }
    return 82;
}

static int check_draw(int raw_insertion, int cyclic)
{
#ifdef MAP_FIXED_NOREPLACE
    void *requested, *mapping;
    u32 *head, *old_next, *tile;
    int x, y, result;
    u16 expected;
    /* Literal 16-byte TILE: len=3, RGB=(248,0,0), code=60h,
     * x=7 y=9 width=2 height=2. Its raw link is patched below. */
    static const unsigned char packet[16] = {
        0xff, 0xff, 0xff, 0x03, 0xf8, 0x00, 0x00, 0x60,
        0x07, 0x00, 0x09, 0x00, 0x02, 0x00, 0x02, 0x00
    };

    requested = (void *)(uintptr_t)0x80100000U;
    mapping = mmap(requested, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (mapping == MAP_FAILED) {
        fprintf(stderr, "PS1 mmap failed: %s\n", strerror(errno));
        return 83;
    }
    if (mapping != requested) {
        fprintf(stderr, "kernel did not honor MAP_FIXED_NOREPLACE\n");
        munmap(mapping, 4096);
        return 84;
    }
    head = (u32 *)mapping;
    old_next = (u32 *)((unsigned char *)mapping + 0x40);
    tile = (u32 *)((unsigned char *)mapping + 0x80);
    pc_ot_setaddr(head, old_next);
    pc_ot_setaddr(old_next, TERMINATOR);
    memcpy(tile, packet, sizeof(packet));
    if (cyclic) {
        /* GsSortClear's static packet AddPrim'd twice into an OT nobody
         * cleared in between: head -> tile -> tile -> ... (TACO 3 -> 10) */
        raw_link(head, raw_token(tile));
        raw_link(tile, raw_token(tile));
    } else if (raw_insertion) {
        raw_link(tile, raw_token(old_next));
        raw_link(head, raw_token(tile));
    } else {
        /* Positive native-cache control: same nodes and pixels, but use
         * explicit native registration for the insertion. */
        pc_ot_setaddr(tile, old_next);
        pc_ot_setaddr(head, tile);
    }
    for (y = 0; y < VRAM_HEIGHT; y++) {
        for (x = 0; x < VRAM_WIDTH; x++) g_vram[y][x] = 0x1234;
    }
    if (mdfn_gpu_enabled()) {
        munmap(mapping, 4096);
        return 85;
    }
    g_frame_prim_count = 0;
    DrawOTag(head);
    result = 0;
    if (cyclic && g_frame_prim_count != 1) {
        fprintf(stderr, "cyclic OT: %d primitives drawn, expected the cycle's "
                "one\n", g_frame_prim_count);
        result = 1;
        goto done;
    }
    for (y = 0; y < VRAM_HEIGHT; y++) {
        for (x = 0; x < VRAM_WIDTH; x++) {
            expected = x >= 7 && x < 9 && y >= 9 && y < 11 ?
                       0x001f : 0x1234;
            if (g_vram[y][x] != expected) {
                fprintf(stderr, "DrawOTag pixel (%d,%d)=%04x expected=%04x; "
                        "sentinel (7,9)=%04x\n", x, y, g_vram[y][x],
                        expected, g_vram[9][7]);
                result = 1;
                goto done;
            }
        }
    }
done:
    if (munmap(mapping, 4096)) return 86;
    return result;
#else
    (void)raw_insertion;
    (void)cyclic;
    fprintf(stderr, "MAP_FIXED_NOREPLACE is required; no MAP_FIXED fallback\n");
    return 87;
#endif
}

int main(int argc, char **argv)
{
    if (argc != 2) return 88;
    if (!strcmp(argv[1], "draw-raw")) return check_draw(1, 0);
    if (!strcmp(argv[1], "draw-cached")) return check_draw(0, 0);
    if (!strcmp(argv[1], "draw-cycle")) return check_draw(1, 1);
    return check_links(argv[1]);
}
