/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * spu_psx.h -- C shim wrapping the upstream PS1 SPU core.
 *
 * The library is built separately (pc/3rd/SPU-CORE.md) and produces a
 * static archive (spu_psx.a). This header exposes a pure-C ABI the rest of
 * the PC port can link against without pulling in C++ headers.
 *
 * All register addresses are PSX-native (0x1F801C00..0x1F801E00).
 */
#ifndef SPU_PSX_H
#define SPU_PSX_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the SPU core. Call once at startup. */
void spu_psx_init(void);

/* Full reset -- clears RAM, registers, and voice state. */
void spu_psx_reset(void);

/* Shutdown. Releases internal resources. */
void spu_psx_shutdown(void);

/* I/O register write (16-bit) at a PSX-native address. */
void spu_psx_write_reg(u32 addr, u16 value);

/* I/O register read (16-bit). Returns the last stored/computed value. */
u16  spu_psx_read_reg(u32 addr);

/* Copy `size` bytes of sample data into SPU RAM at `dest`. Mimics the
 * DMA-channel-4 path the game normally drives via SsSpuSetData. */
void spu_psx_dma_write(const u8 *data, u32 dest, u32 size);

/* Copy `size` bytes of SPU RAM at `src` (wrapping at 512 KiB) to `out`:
 * the data side of a DMA-channel-4 read. */
void spu_psx_dma_read(u8 *out, u32 src, u32 size);

/* Generate `num_samples` stereo-interleaved 44100Hz frames into `out`.
 * Each frame is two s16 samples (L, R). `out` must have space for
 * 2 * num_samples s16 values. */
void spu_psx_mix(s16 *out, int num_samples);

/* Install an IRQ line callback. The SPU raises IRQ9 when a specific RAM
 * address is accessed (the PSX "SPU IRQ" mechanism). The hook is invoked
 * with (irq_index, state) where irq_index == 9 (InterruptController::IRQ::SPU)
 * and state is 0 or 1. May be NULL to clear. */
void spu_psx_set_irq_hook(void (*hook)(int irq, int state));

/* Install the CD-ROM controller's audio output (pc/hal/cd_audio.c): the
 * core calls it once per 44.1 kHz sample, mixes the frame with the SPU's CD
 * volume (0x1F801DB0/2, SPUCNT bit 0) and writes it to the CD capture
 * buffers at SPU RAM 0x000 (left) / 0x400 (right).  Returning 0 means the
 * drive's audio FIFO is empty (silence).  NULL clears it. */
void spu_psx_set_cd_audio_source(int (*source)(s16 *left, s16 *right));

/* The music split (pc/hal/music_pack.c; the core patch is described in
 * pc/3rd/SPU-CORE.md, "Local patches").  With `on`, the core also
 * computes a "rest" mix that leaves out the voices in `voice_mask` (dry and
 * reverb input; the reverb of the rest runs in a shadow unit with its own
 * copy of the work area).  `replace` outputs the rest mix instead of the full
 * one.  Device state -- voices, ADSR, ENDX, capture buffers, the real reverb
 * unit and SPU RAM -- is the same as without the split.  Off: the unpatched
 * output.  Call under the audio lock. */
void spu_psx_music_split(int on, u32 voice_mask, int replace);

/* spu_psx_mix, and while the split is on `music` (may be NULL; 2 s16 per
 * frame) receives the masked voices' share of the output before the main
 * volume (full mix minus rest mix); zeros while it is off. */
void spu_psx_mix_split(s16 *out, s16 *music, int num_samples);

/* Enhanced sound quality (pc/hal/spu.c; core patch: README.md "PC patches").
 * interp: 0 the console's Gaussian (default), 1 cubic, 2 sinc; reverb_hq: a
 * floating-point reverb unit replaces the reverb in the output (the real
 * unit keeps running on SPU RAM).  (0, 0) is the unpatched computation.
 * spu_psx_prepare_quality(2) builds the sinc tables; call it outside the
 * audio lock before selecting mode 2 (mode 2 without them falls back to
 * cubic).  Call spu_psx_set_quality under the audio lock. */
void spu_psx_prepare_quality(int interp);
void spu_psx_set_quality(int interp, int reverb_hq);

/* The SoundFont renderer's frames for the next spu_psx_mix / _mix_split
 * call (pc/hal/soundfont.c): 4 s32 per frame (dry L, dry R, wet L, wet R),
 * at least as many frames as the call mixes; added to the split's rest mix
 * (the wet pair also to the rest's reverb input) and ignored while the
 * split is off.  NULL after the call. */
void spu_psx_inject(const s32 *frames);

/* Bit v set while voice v sounds (its envelope is not off). */
u32  spu_psx_voice_on_mask(void);

#ifdef __cplusplus
}
#endif

#endif /* SPU_PSX_H */
