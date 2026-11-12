/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Pure command-addressed texture sampling, with caller-owned native VRAM. */
#ifndef GPU_TEXTURE_H
#define GPU_TEXTURE_H

/* Encodings: standard PS1 (GetGraphType()==0), not the alternate GPU type.
 * u/v are unsigned 8-bit texture coordinates, AFTER texture-window mapping.
 * tpage bits 0..3 select X/64, bit 4 selects Y/256, bits 7..8 select
 * 0=4-bit indexed, 1=8-bit indexed, 2=15-bit direct. Other bits are ignored.
 * clut bits 0..5 select X/16 and bits 6..14 select Y; bit 15 is ignored.
 * VRAM has 512 rows of 1024 unsigned 16-bit words; pass &g_vram[0][0].
 *
 * Returns the literal 16-bit palette/direct color, including bit 15.
 * Palette INDEX zero is not intrinsically transparent: its COLOR is read.
 * Actual color 0x0000 remains zero; 0x8000 remains 0x8000. Transparency,
 * blending, modulation, caching and texture-window mapping are caller jobs.
 * No asset/scene registrations, retained state, allocation or VRAM writes.
 *
 * Unsupported: reserved depth 3 and NULL VRAM return zero.  A texture word
 * or palette entry past X=1023 wraps to X & 1023 of the SAME row (no carry
 * into Y).  PSX-SPX states this edge rule for VRAM copies and fills
 * ("wrap to the opposite memory edge ... without any carry-out from X to
 * Y"); for texture and CLUT fetches it is what both reference rasterizers
 * implement (Mednafen GetTexel/Update_CLUT_Cache `& 1023`, DuckStation
 * `% VRAM_WIDTH`).  Encoded Y plus u8 v cannot exceed row 511.  Hardware
 * cache behavior is not modelled here (texel_fetch in gpu.c does).
 *
 * Evidence: Psy-Q 2.5 LIBGPU.H getTPage/getClut document the standard bit
 * fields (4/8-bit indexed and 15-bit direct formats).  The row-edge wrap is
 * the PSX-SPX VRAM edge rule (GPU "Wrapping"), corroborated by the reference
 * emulators named above; neither source establishes reserved-depth behavior.
 * The reserved-depth zero result is explicit unsupported-input handling,
 * not a claim that hardware returns zero (old texel_fetch treated 3 as 2).
 */
unsigned short gpu_texture_sample(const unsigned short *vram,
                                  unsigned char u, unsigned char v,
                                  unsigned short clut,
                                  unsigned short tpage);

#endif /* GPU_TEXTURE_H */
