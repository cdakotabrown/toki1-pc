/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gpu_texrep.h -- texture dumping and replacement packs for the native
 * rasterizer (the enhanced profile's GPU_TEXTURES; docs/pc-port/
 * ENHANCED-MODE.md, "Texture dumping and replacement").
 *
 * The unit of a texture is an UPLOAD: every LoadImage / GP0 A0h rectangle
 * (and the copy of a whole upload by MoveImage) is recorded as a source with the hash
 * of the 16-bit words it left in VRAM.  owner[] names, per native VRAM
 * pixel, the source that wrote it, and oval[] the value it wrote, so a
 * pixel the game has since overwritten by any other path (a draw, a fill,
 * a save-state load) is simply no longer owned -- nothing has to hook
 * every write.
 *
 * A textured sprite or polygon samples texels; each texel lies in one
 * native VRAM pixel and so in at most one source.  The texture KEY of a
 * source as the primitive sees it is
 *
 *     15-bit direct:  t16-<source hash>-<W>x<H>
 *     8-bit / 4-bit:  t8-<source hash>-<CLUT hash>-<W>x<H>  (t4-...)
 *
 * where W x H is the source in texels and the CLUT hash covers the 256 /
 * 16 palette entries the primitive uses (their values, not their place).
 * The same key names the dumped PNG and the replacement a pack provides.
 * A source drawn to the frame buffer directly (an upload into the display
 * or drawing area) is keyed t16 as well: DuckStation's "VRAM write".
 *
 * Dumping only reads.  Replacement only writes the high-resolution shadow
 * (pc/hal/gpu_hires.h): the native VRAM the game sees is never touched,
 * and every replaced shadow pixel lies in a block whose native pixel the
 * native pass draws as at 1x.  Nothing here runs unless the profile switch
 * is on and a dump or a pack was asked for: every hook in gpu.c is a
 * not-taken branch on g_texrep_on.
 */
#ifndef PC_GPU_TEXREP_H
#define PC_GPU_TEXREP_H

extern int g_texrep_on;       /* dumping or a pack: gpu.c's hooks run */
extern int g_texrep_replace;  /* a pack is loaded and the shadow is on */

/* Settings, environment, pack index.  Before gpu_hires_init: a pack can
 * raise the shadow's scale (gpu_texrep_pack_scale). */
void gpu_texrep_init(void);
/* The scale a loaded pack wants (its largest integer factor, <= 4); 1 when
 * none.  gpu_hires_init takes the larger of this and resolution_scale. */
int gpu_texrep_pack_scale(void);

/* After the native write of an upload (LoadImage, GP0 A0h, a MoveImage
 * destination): x/y are native, w/h the rectangle as gpu_vram_load
 * wrapped it.  With a pack and a t16 replacement for these words, the
 * shadow blocks of the rectangle get the replacement. */
void gpu_texrep_upload(int x, int y, int w, int h);
/* After a native MoveImage (sx,sy,w,h) -> (dx,dy): the copy of a whole,
 * unchanged upload becomes a source with the same key (a sheet moved to
 * another page); any other copy (a captured frame) leaves the destination
 * unowned. */
void gpu_texrep_move(int sx, int sy, int w, int h, int dx, int dy);

/* A textured primitive is about to sample: tpage/clut as the primitive
 * gives them, the inclusive UV box it can reach (u0 <= u1, v0 <= v1, page
 * coordinates 0..255).  Dumps every source it reaches under this CLUT that
 * is not dumped yet. */
void gpu_texrep_note_draw(unsigned short clut, unsigned short tpage,
                          int u0, int v0, int u1, int v1);

/* Called once per game frame (hal_frame_end) with the game's scene number
 * (now_step, for the dump index): frame-buffer uploads of the frame are
 * dumped and the index is flushed. */
void gpu_texrep_frame_end(int scene);

/* Replacement sampling for the shadow passes (gpu.c).  prim_begin sets the
 * primitive's page and palette; it returns 0 when no replacement can apply
 * (no pack), and the caller then keeps its 1x path.  sample() takes the
 * texture-windowed texel coordinates and the 8-bit fraction inside the
 * texel, and answers 0 = no replacement (use the native texel), 1 = the
 * replacement is see-through here (alpha < GPU_TEXREP_ALPHA_CLEAR), 2 =
 * *rgba (0xRRGGBBAA).  Alpha is coverage only: the semi-transparency bit
 * stays the native texel's, and alpha below GPU_TEXREP_ALPHA_SOLID mixes
 * the finished pixel with what is behind it (gpu_texrep_alpha_mix), so an
 * upscaler's soft edges stay soft. */
#define GPU_TEXREP_ALPHA_CLEAR 16
#define GPU_TEXREP_ALPHA_SOLID 240
/* px16 (GpuTexrepTexel): a solid pixel's 15-bit colour, else one of these */
#define GPU_TEXREP_PX16_CLEAR 0xFFFFu
#define GPU_TEXREP_PX16_MIX   0xFFFEu
int gpu_texrep_prim_begin(unsigned short clut, unsigned short tpage);
/* back * (255 - a) + front * a per 5-bit channel; front's bit 15 kept. */
static __inline__ unsigned short gpu_texrep_alpha_mix(unsigned short back,
                                                      unsigned short front, unsigned a)
{
    unsigned r = ((back & 31u) * (255u - a) + (front & 31u) * a + 127u) / 255u;
    unsigned g = (((back >> 5) & 31u) * (255u - a) + ((front >> 5) & 31u) * a + 127u) / 255u;
    unsigned b = (((back >> 10) & 31u) * (255u - a) + ((front >> 10) & 31u) * a + 127u) / 255u;
    return (unsigned short)(r | (g << 5) | (b << 10) | (front & 0x8000u));
}
int gpu_texrep_sample(unsigned u, unsigned v, unsigned fu, unsigned fv,
                      unsigned int *rgba);
/* The same in two steps, for a pass that samples one texel many times (a
 * sprite's N x N sub-pixels): texel() finds the replacement of texel
 * (u, v) once (0: none), texel_at() reads it at a fraction of the texel. */
typedef struct {
    const unsigned int *px;     /* the replacement, 0xRRGGBBAA, rows of w */
    const unsigned short *px16; /* the same, 15-bit, or GPU_TEXREP_PX16_* */
    int w, h;
    unsigned tx, ty;            /* the texel inside its source */
    unsigned kx, ky;            /* replacement pixels per texel, 16.16 */
} GpuTexrepTexel;
int gpu_texrep_texel(unsigned u, unsigned v, GpuTexrepTexel *t);
static __inline__ unsigned int gpu_texrep_texel_at(const GpuTexrepTexel *t,
                                                   unsigned fu, unsigned fv)
{
    unsigned ix = (unsigned)((((unsigned long long)t->tx << 8 | fu) * t->kx) >> 24);
    unsigned iy = (unsigned)((((unsigned long long)t->ty << 8 | fv) * t->ky) >> 24);
    if (ix >= (unsigned)t->w) ix = (unsigned)t->w - 1u;
    if (iy >= (unsigned)t->h) iy = (unsigned)t->h - 1u;
    return t->px[(unsigned long)iy * (unsigned)t->w + ix];
}

/* The settings menu (pc/hal/present.c): switch dumping, choose a pack
 * (a name under gpu_texrep_packs_dir(), a path, or "" for none) while the
 * game runs.  A pack shows at once when the shadow is running; otherwise
 * gpu_texrep_needs_restart() says so.  -1: the profile switch is off. */
int gpu_texrep_set_dump(int on);
int gpu_texrep_set_pack(const char *pack);
int gpu_texrep_needs_restart(void);
const char *gpu_texrep_packs_dir(void);

/* Drop every decoded replacement and re-read the pack directory
 * (settings menu "Reload texture pack"); the new pack shows on the next
 * draw.  Returns the number of replacement files found. */
int gpu_texrep_reload(void);

/* Diagnostics (the settings menu's notes, tests). */
typedef struct {
    unsigned long uploads, sources_live, draws, dumped, dump_skipped_small,
                  dump_skipped_changed, pack_files, pack_loaded, samples_replaced;
    char dump_dir[1024];
    char pack_dir[1024];
} GpuTexrepStats;
void gpu_texrep_stats(GpuTexrepStats *out);

#endif
