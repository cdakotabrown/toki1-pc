/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Bounded adapter for the unchanged SDK _clr packet producer. */
#ifndef PC_GPU_CLEAR_H
#define PC_GPU_CLEAR_H
#include "types.h"
void gpu_clear_image(void *rect, u32 color);
u32 gpu_clear_get_info(s32 which);
int gpu_execute_clear_words(const u32 *words, u32 count);
#endif
