/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libgpu SDK CPU work phases (pc/hal/gpu_cpu.c). */
#ifndef PC_GPU_CPU_H
#define PC_GPU_CPU_H
#include "types.h"
void gpu_cpu_submit_entry(u32 to_alarm, u32 to_mask);
void gpu_cpu_masked(u32 masked, u32 to_return);
#endif
