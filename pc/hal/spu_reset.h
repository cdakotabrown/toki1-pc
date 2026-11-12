/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Native adapter for the ready-device SpuInit register sequence. */
#ifndef PC_SPU_RESET_H
#define PC_SPU_RESET_H
#include "types.h"
int spu_reset_initialize(const u8 *silence);
void spu_reset_cancel(void);
/* Short host-device operations; no audio lock may span a settling wait. */
void spu_reset_write(u32 offset, u16 value);
u16 spu_reset_read(u32 offset);
void spu_reset_enable_dma(void);
#endif
