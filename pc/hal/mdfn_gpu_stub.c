/* SPDX-License-Identifier: GPL-3.0-or-later */
/* No-op Mednafen GPU backend.
 *
 * The parity backend (a vendored Mednafen PSX software rasterizer, GPL-2.0+)
 * is not part of the tree: it ships as an opt-in you drop in yourself, because
 * it is a *verification* path, not a shipped one. Build with
 * `make -f pc/Makefile MDFN_GPU_BACKEND=1` and a `pc/3rd/mednafen-gpu/`
 * checkout (see that directory's README, or THIRD-PARTY.md) to get the real
 * rasterizer.
 *
 * Without it these stubs satisfy the link so that `pc/hal/gpu.c` -- which is
 * written against the backend's interface -- compiles and runs unchanged. The
 * HAL only reaches them when the backend reports itself enabled, and
 * `mdfn_gpu_enabled()` is compiled to a constant 0 in this configuration, so
 * nothing here is ever executed. If that invariant breaks, the abort below
 * says so instead of silently drawing nothing.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../3rd/mednafen-gpu/shim/mdfn_gpu_shim.h"

static void mdfn_unreachable(const char *fn)
{
    /* A backend call with the backend compiled out is a build bug, not a
     * runtime condition: fail loudly rather than render nothing. */
    fprintf(stderr, "fatal: %s called with MDFN_GPU_BACKEND=0\n", fn);
    abort();
}

void mdfn_gpu_init(uint16_t *vram_backing_store)
{
    (void)vram_backing_store;
    mdfn_unreachable("mdfn_gpu_init");
}

void mdfn_gpu_shutdown(void) { mdfn_unreachable("mdfn_gpu_shutdown"); }

void mdfn_set_drawing_area(int x0, int y0, int x1, int y1)
{
    (void)x0; (void)y0; (void)x1; (void)y1;
    mdfn_unreachable("mdfn_set_drawing_area");
}

void mdfn_set_draw_offset(int x, int y)
{
    (void)x; (void)y;
    mdfn_unreachable("mdfn_set_draw_offset");
}

void mdfn_set_texpage(uint16_t packed)
{
    (void)packed;
    mdfn_unreachable("mdfn_set_texpage");
}

void mdfn_set_clut(uint16_t packed)
{
    (void)packed;
    mdfn_unreachable("mdfn_set_clut");
}

void mdfn_set_mask(int set_or, int eval_and)
{
    (void)set_or; (void)eval_and;
    mdfn_unreachable("mdfn_set_mask");
}

void mdfn_set_texwindow(uint8_t tww, uint8_t twh, uint8_t twx, uint8_t twy)
{
    (void)tww; (void)twh; (void)twx; (void)twy;
    mdfn_unreachable("mdfn_set_texwindow");
}

void mdfn_clear_cache(void) { mdfn_unreachable("mdfn_clear_cache"); }

void mdfn_invalidate_tex_cache(void)
{
    mdfn_unreachable("mdfn_invalidate_tex_cache");
}

void mdfn_draw_triangle(const MdfnTri *t)
{
    (void)t;
    mdfn_unreachable("mdfn_draw_triangle");
}

void mdfn_draw_sprite(const MdfnSprite *s)
{
    (void)s;
    mdfn_unreachable("mdfn_draw_sprite");
}

void mdfn_draw_line(const MdfnLine *l)
{
    (void)l;
    mdfn_unreachable("mdfn_draw_line");
}

uint16_t *mdfn_vram(void)
{
    mdfn_unreachable("mdfn_vram");
    return NULL;
}