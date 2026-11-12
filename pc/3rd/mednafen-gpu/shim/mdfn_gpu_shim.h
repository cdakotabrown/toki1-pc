/* Public C-linkage interface to the vendored Mednafen PSX GPU
 * software rasterizer.
 *
 * Users (pc/hal/gpu.c) feed parsed GP0 primitives in; the shim
 * builds the tri_vertex/line_point structs and dispatches to
 * the appropriate Mednafen template instantiation.
 */
#ifndef MDFN_GPU_SHIM_H
#define MDFN_GPU_SHIM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One-time init / teardown. `vram` must point to a 1MiB buffer
 * (1024 * 512 * 2 bytes) that will be written directly by the
 * rasterizer. */
void mdfn_gpu_init(uint16_t *vram_backing_store);
void mdfn_gpu_shutdown(void);

/* Drawing state — matches PS1 GP0 environment commands. */
void mdfn_set_drawing_area(int x0, int y0, int x1, int y1);
void mdfn_set_draw_offset(int x, int y);
void mdfn_set_texpage(uint16_t packed);   /* raw tpage bits (GP0 0xE1) */
void mdfn_set_clut(uint16_t packed);      /* raw clut word */
void mdfn_set_mask(int set_or, int eval_and);
void mdfn_set_texwindow(uint8_t tww, uint8_t twh, uint8_t twx, uint8_t twy);
/* GP0(01h): clear the texture and CLUT caches. */
void mdfn_clear_cache(void);
/* Start of GP0(80h/A0h/C0h): clear the texture cache only. */
void mdfn_invalidate_tex_cache(void);

/* Primitive submission.
 *
 * Vertex format matches what pc/hal/gpu.c already parses — x/y are
 * screen-space int16, u/v in 0..255 for textured prims, r/g/b 8-bit
 * per-channel. For flat-shaded prims, all three vertex colors are
 * equal. Caller specifies shading/texture/blend mode via flags.
 */
enum mdfn_blend_mode {
    MDFN_BLEND_OFF = 0,
    MDFN_BLEND_HALF,      /* B/2 + F/2 */
    MDFN_BLEND_ADD,       /* B + F */
    MDFN_BLEND_SUB,       /* B - F */
    MDFN_BLEND_ADD_QTR    /* B + F/4 */
};

typedef struct {
    int16_t x, y;
    uint8_t u, v;
    uint8_t r, g, b;
} MdfnVertex;

typedef struct {
    MdfnVertex verts[3];
    int gouraud;           /* 0/1 */
    int textured;          /* 0/1 */
    int tex_mult;          /* 0 = raw, 1 = modulate by vertex color */
    int blend;             /* enum mdfn_blend_mode */
    int mask_eval;         /* 0/1 — if 1, skip pixels where dest MSB set */
} MdfnTri;

typedef struct {
    int16_t x, y;          /* top-left */
    int16_t w, h;
    uint8_t u, v;          /* top-left UV (textured only) */
    uint8_t r, g, b;       /* fill color or modulate */
    int textured;
    int tex_mult;
    int blend;
    int mask_eval;         /* 0/1 -- skip pixels whose dest MSB is set */
    int flip_x, flip_y;    /* 0/1 -- mirror texel fetch along that axis */
} MdfnSprite;

typedef struct {
    MdfnVertex verts[2];   /* u/v ignored; r/g/b per endpoint */
    int gouraud;
    int blend;
    int mask_eval;
} MdfnLine;

void mdfn_draw_triangle(const MdfnTri *t);
void mdfn_draw_sprite  (const MdfnSprite *s);
void mdfn_draw_line    (const MdfnLine *l);

/* VRAM access (pass-through). */
uint16_t *mdfn_vram(void);

#ifdef __cplusplus
}
#endif

#endif /* MDFN_GPU_SHIM_H */
