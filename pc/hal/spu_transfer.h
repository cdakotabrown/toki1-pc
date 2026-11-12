/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SPU DMA requests on the shared emulated clock. */
#ifndef PC_SPU_TRANSFER_H
#define PC_SPU_TRANSFER_H
#include "types.h"
/* Measured CPU work in the SDK 240-iteration settling loop. */
#define SPU_WAITFS_TICKS 7470u
void spu_transfer_init(void);
void spu_transfer_shutdown(void);
u64 spu_transfer_next_event(void);
void spu_transfer_complete_event(void);
s32 spu_transfer_mode(s32 mode);
u32 spu_transfer_address(u32 address);
s32 spu_transfer_write(const u8 *data, u32 size);
s32 spu_transfer_completed(s32 wait);
s32 spu_transfer_clear(s32 mode);
void spu_transfer_commit(const u8 *data, u32 address, u32 size);
/* SPU RAM -> main RAM under the audio lock (pc/hal/spu.c). */
void spu_transfer_readback(u8 *dst, u32 address, u32 size);
#endif
