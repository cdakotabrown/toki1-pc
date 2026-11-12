/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pc_crt.h -- the presenter's CRT filter: presets and the SDL-free math.
 *
 * Presentation only.  The filter is applied by pc/hal/present.c while it
 * puts the finished picture (the 320x240 scanout, or the N x picture of
 * pc/hal/gpu_hires.c) into the window; the PS1 VRAM, the rasterizer and
 * every emulated value are never read back or changed, and screenshots
 * stay the plain picture.
 *
 * How it is drawn (SDL_Renderer only, no shaders, so it runs on every
 * SDL2 backend including the software renderer):
 *   0. GAMMA   -- the picture's pixels through a 256-entry table before
 *                 gpu.c uploads them (no extra texture upload);
 *   1. the picture, sampled smooth, into the layout rectangle;
 *   2. x MASK  -- a texture the size of that rectangle holding the
 *                 scanline profile x the phosphor mask (SDL_BLENDMODE_MOD);
 *   3. + picture x BOOST -- brightness compensation: what the mask took
 *                 away, given back (SDL_BLENDMODE_ADD; the renderer's
 *                 clamp makes bright pixels bleed over the scanline gap,
 *                 as a real beam widens);
 *   4. + GLOW  -- a small blurred copy of the picture, stretched smooth
 *                 (SDL_BLENDMODE_ADD);
 *   5. x VIGNETTE -- a small darkening texture, stretched smooth
 *                 (SDL_BLENDMODE_MOD).
 * Flat, every pass is an SDL_RenderCopy into the rectangle; with curvature
 * every pass goes through one bent vertex mesh (SDL_RenderGeometry, SDL
 * 2.0.18+), so the scanlines curve with the tube.
 *
 * Everything below is pure (no SDL, no globals) so pc/tests/test_crt.c can
 * check it, including pc_crt_reference(): the same five passes evaluated on
 * the CPU for one pixel, the model the SDL passes implement.
 */
#ifndef PC_CRT_H
#define PC_CRT_H

#ifdef __cplusplus
extern "C" {
#endif

enum { PC_CRT_OFF, PC_CRT_LITE, PC_CRT_APERTURE, PC_CRT_SHADOW, PC_CRT_COUNT };
enum { PC_CRT_MASK_NONE, PC_CRT_MASK_APERTURE, PC_CRT_MASK_SHADOW };

typedef struct {
    const char *name;       /* settings.ini value */
    double scan;            /* 0..1: depth of the dark gap between lines */
    double scan_power;      /* beam profile exponent (higher = flatter beam) */
    int mask;               /* PC_CRT_MASK_* */
    double mask_depth;      /* 0..1: how dark the other two phosphors are */
    double comp;            /* 0..1: share of the lost light given back */
    double glow;            /* 0..1: strength of the blurred copy */
    double vignette;        /* 0..1: corner darkening */
    double gamma;           /* exponent on the picture (1 = untouched) */
} PcCrtParams;

/* The preset for PC_CRT_* (PC_CRT_OFF and out-of-range give NULL). */
const PcCrtParams *pc_crt_preset(int mode);

/* The barrel bend with curvature on (0 = flat). */
#define PC_CRT_CURVE 0.045

/* The CRT needs at least this many output rows per picture row: below
 * 2x a mask cannot be resolved, and the picture is drawn plain. */
#define PC_CRT_MIN_SCALE 2

/* The scanline factor (0..1) of output row y of an h-row rectangle that
 * shows src_rows picture rows: the beam profile averaged over the part of
 * the picture row that output row covers. */
double pc_crt_scan_factor(const PcCrtParams *p, int y, int h, int src_rows);

/* The phosphor mask factor of output pixel (x, y) for channel c (0 R,
 * 1 G, 2 B). */
double pc_crt_mask_factor(const PcCrtParams *p, int x, int y, int c);

/* Fill a w x h RGBA8888 (0xRRGGBBAA) MASK texture: scanline x phosphor
 * mask, alpha 255.  Returns the mean of the mask over its three channels
 * (the light that is left on a white picture), 0..1. */
double pc_crt_build_mask(const PcCrtParams *p, int w, int h, int src_rows, unsigned int *out);

/* The BOOST colour factor (0..1) for a mask whose mean is `mean`. */
double pc_crt_boost(const PcCrtParams *p, double mean);

/* 256-entry gamma table for p->gamma. */
void pc_crt_gamma_lut(double gamma, unsigned char lut[256]);

/* The GLOW picture: src (w x h RGBA8888, `stride` pixels per row) box-
 * averaged down to gw x gh (at most 4 x 4 samples per cell), then blurred
 * once more with a 3x3 box (edges clamped).  1..256 x 1..192. */
void pc_crt_glow(const unsigned int *src, int w, int h, int stride,
                 unsigned int *out, int gw, int gh);

/* The mesh: normalised rectangle coordinates (u, v in 0..1) -> where the
 * point lands, also normalised (0..1 = the rectangle).  curve 0 is the
 * identity; otherwise the edges bow out, the corners pulled in. */
void pc_crt_warp(double u, double v, double curve, double *x, double *y);

/* The vignette factor (0..1) at normalised (u, v). */
double pc_crt_vignette(const PcCrtParams *p, double u, double v);

/* One output channel value (0..255) of the composite, as the five passes
 * compute it: pic and glow are the 0..255 inputs at that pixel, mask the
 * MASK texel (0..255), boost and vig the factors above. */
int pc_crt_reference(int pic, int mask, int glow, double boost, double glow_k, double vig);

#ifdef __cplusplus
}
#endif
#endif /* PC_CRT_H */
