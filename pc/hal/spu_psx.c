/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * spu_psx.c -- C-callable dispatch into the SPU core.
 *
 * This file is compiled as C and simply forwards each call to a C++ shim
 * symbol (ds_spu_*), which is defined in pc/3rd/spu-glue/stubs/spu_shim.cpp.
 * Keeping this translation unit as pure C means the rest of the PC port
 * (also C) can link against it without any C++ headers.
 */
#include "spu_psx.h"

/* Declarations for the C++ shim (see stubs/spu_shim.cpp). */
extern void ds_spu_init(void);
extern void ds_spu_reset(void);
extern void ds_spu_shutdown(void);
extern void ds_spu_write_reg(u32 addr, u16 value);
extern u16  ds_spu_read_reg(u32 addr);
extern void ds_spu_dma_write(const u8 *data, u32 dest, u32 size);
extern void ds_spu_dma_read(u8 *out, u32 src, u32 size);
extern void ds_spu_mix(s16 *out, int num_samples);
extern void ds_spu_music_split(int on, u32 voice_mask, int replace);
extern void ds_spu_mix_split(s16 *out, s16 *music, int num_samples);
extern void ds_spu_set_quality(int interp, int reverb_hq);
extern void ds_spu_prepare_quality(int interp);
extern void ds_spu_inject(const s32 *frames);
extern u32  ds_spu_voice_on_mask(void);
extern void spu_core_set_irq_hook(void (*hook)(int, int));
extern void spu_core_set_cd_audio_source(int (*source)(s16 *, s16 *));

void spu_psx_init(void)                          { ds_spu_init(); }
void spu_psx_reset(void)                         { ds_spu_reset(); }
void spu_psx_shutdown(void)                      { ds_spu_shutdown(); }
void spu_psx_write_reg(u32 addr, u16 value)      { ds_spu_write_reg(addr, value); }
u16  spu_psx_read_reg(u32 addr)                  { return ds_spu_read_reg(addr); }
void spu_psx_dma_write(const u8 *data, u32 dest, u32 size)
                                                 { ds_spu_dma_write(data, dest, size); }
void spu_psx_dma_read(u8 *out, u32 src, u32 size) { ds_spu_dma_read(out, src, size); }
void spu_psx_mix(s16 *out, int num_samples)      { ds_spu_mix(out, num_samples); }
void spu_psx_music_split(int on, u32 voice_mask, int replace)
                                                 { ds_spu_music_split(on, voice_mask, replace); }
void spu_psx_mix_split(s16 *out, s16 *music, int num_samples)
                                                 { ds_spu_mix_split(out, music, num_samples); }

void spu_psx_set_irq_hook(void (*hook)(int irq, int state))
{
    spu_core_set_irq_hook(hook);
}

void spu_psx_set_cd_audio_source(int (*source)(s16 *left, s16 *right))
{
    spu_core_set_cd_audio_source(source);
}
void spu_psx_prepare_quality(int interp)         { ds_spu_prepare_quality(interp); }
void spu_psx_set_quality(int interp, int reverb_hq)
                                                 { ds_spu_set_quality(interp, reverb_hq); }
void spu_psx_inject(const s32 *frames)           { ds_spu_inject(frames); }
u32  spu_psx_voice_on_mask(void)                 { return ds_spu_voice_on_mask(); }
