/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc/hal/mdec_native.h — pure C MDEC decoder (no Java/jpsxdec).
 */
#ifndef PC_HAL_MDEC_NATIVE_H
#define PC_HAL_MDEC_NATIVE_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Decode a BS bitstream into PSX 5-5-5 pixels. Returns 0 on success.
 * out_psx555 must hold width*height u16 entries. White→transparent sets
 * pixels with R/G/B >= 248 to value 0. */
s32 mdec_decode_bs_native(const u8 *bs, u32 bs_size, u32 width, u32 height,
                          u16 *out_psx555, s32 white_to_transparent);

/* The same decode with the MDEC's 24-bit output: width*height R, G, B
 * byte triples (the unrounded channels the 15-bit output rounds). */
s32 mdec_decode_bs_native_rgb24(const u8 *bs, u32 bs_size, u32 width,
                                u32 height, u8 *out_rgb24);

/* Macroblock stream: the MDEC's view of a BS (no image width).  open
 * copies bs_size bytes of the bitstream (NULL on a bad header);
 * macroblock decodes the next 16x16 block into out (row pitch stride,
 * in pixels) and returns 0, or negative once the stream is exhausted or
 * corrupt (out is then untouched or partially written). */
typedef struct MdecBsStream MdecBsStream;
MdecBsStream *mdec_bs_stream_open(const u8 *bs, u32 bs_size);
s32 mdec_bs_stream_macroblock(MdecBsStream *s, u16 *out_psx555, u32 stride);
void mdec_bs_stream_close(MdecBsStream *s);

/* The bitstream bytes a VLC pass reads, and the run-level halfwords it
 * produces (*halfwords), up to the header's count or the stream's end. */
u32 mdec_bs_vlc_extent(const u8 *bs, u32 bs_size, u32 *halfwords);

/* Internal: VLC decode → RLE. Exposed for testing. Returns word count
 * written (or negative on error). out_max in u16 elements. */
s32 mdec_vlc_decode(const u8 *bs, u32 bs_size, u16 *out_rle, u32 out_max);

#ifdef __cplusplus
}
#endif

#endif
