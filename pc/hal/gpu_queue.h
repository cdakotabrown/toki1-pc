/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SDK dispatch and the separately modeled GPU command timeline. */
#ifndef PC_GPU_QUEUE_H
#define PC_GPU_QUEUE_H
#include "types.h"
u32 gpu_queue_begin(void);
void gpu_queue_end(u32 generation);
void gpu_queue_reset(s32 full);
void gpu_queue_work(u32 gpu_ticks);
void gpu_queue_hang(void);
void gpu_queue_alarm_set(void);
s32 gpu_queue_sync(s32 mode);
#endif
