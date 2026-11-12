/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Pure PS1 VRAM image transfers; no device, SDL or game dependencies. */
#ifndef GPU_VRAM_H
#define GPU_VRAM_H

#include <stddef.h>

#define GPU_VRAM_WIDTH 1024
#define GPU_VRAM_HEIGHT 512
#define GPU_VRAM_PIXELS (1024UL * 512UL)

/* SDK preparation, NOT raw GP0 size decoding. Matched _dws/_drs clamp
 * negative dimensions to zero and positive dimensions to the VRAM limits,
 * then reject an empty image. Returns the halfword count and writes both
 * clamped dimensions (output pointers must be valid).
 *
 * Call this before load/store when implementing LoadImage/StoreImage.
 * Reserve (count + 1) / 2 CPU words for the SDK's DMA/port interface.
 */
size_t gpu_vram_image_pixels(int width, int height,
                             unsigned int *copy_width,
                             unsigned int *copy_height);

/* Raw GP0(A0h/C0h) data-plane operations:
 *   x &= 1023; y &= 511;
 *   width = ((unsigned(width) - 1) & 1023) + 1;
 *   height = ((unsigned(height) - 1) & 511) + 1;
 * Each axis wraps independently: X never carries into Y.
 *
 * vram must address GPU_VRAM_PIXELS contiguous 16-bit halfwords.
 * pixels must address decoded_width * decoded_height halfwords and must
 * NOT alias vram. Storage is caller-owned and pointer-valid; validation,
 * pointer folding, logging, timing and trace ownership remain in gpu.c.
 * Load mask_set/check are boolean GP0(E6h) flags. Store ignores mask flags.
 * Only image halfwords are accessed: the odd transfer's extra CPU halfword
 * is neither read nor written. Its readback value is not specified here.
 * No global state, allocations, drawing-area clipping or color conversion.
 */
void gpu_vram_load(unsigned short *vram, int x, int y, int width, int height,
                   const unsigned short *pixels, int mask_set,
                   int mask_check);
void gpu_vram_store(const unsigned short *vram, int x, int y,
                    int width, int height, unsigned short *pixels);

/* Raw GP0(02h) fill: packed RGB24 color, XY and WH command words.
 * Normal 1024x512 GPU contract: X aligns down to 16, width masks to
 * 10 bits THEN rounds up to 16, Y/height mask to 9 bits; zero is empty.
 * Axes wrap independently. Color truncates to BGR555 with bit15 clear.
 * E6 mask settings, drawing area and dithering do not alter this fill.
 * This does NOT implement the SDK ClearImage rectangle-selection policy.
 * The old v0 GPU's X-low-nibble=15 address anomaly is not modeled.
 * Evidence: PSX-SPX "Quick Rectangle Fill", "Masking and Rounding for
 * FILL Command parameters", "Wrapping", and "GP0(02h) FillVram":
 * https://psx-spx.consoledev.net/graphicsprocessingunitgpu/
 * consulted 2026-10-21. Caller supplies valid full VRAM; no host guards.
 */
void gpu_vram_fill(unsigned short *vram, unsigned int color,
                   unsigned int position, unsigned int size);

/* Evidence: matched src/main/matched/{_dws,_drs,checkRECT}.c and PSX-SPX,
 * https://psx-spx.consoledev.net/graphicsprocessingunitgpu/
 * sections "VRAM to VRAM/CPU to VRAM/VRAM to CPU blitting", "Masking for
 * COPY Commands parameters", and "Wrapping" (consulted 2026-10-21).
 * checkRECT only diagnoses; it does not clip the caller's origin.
 *
 * Not ClearImage: GP0(02h) fill aligns X down and rounds width up to 16;
 * zero sizes mean no fill. Do not apply this copy contract to that API.
 * Not MoveImage: neither these matched wrappers nor that spec establish
 * overlapping-copy read/write ordering or buffering. Leave its behavior
 * untouched until a focused hardware/reference probe establishes it.
 */
#endif
