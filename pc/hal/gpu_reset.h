/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SDK reset metadata and the native GPU reset hook. */
#ifndef PC_GPU_RESET_H
#define PC_GPU_RESET_H
#include "types.h"
void gpu_reset_graph(s32 mode);
void gpu_reset_device(void);
void gpu_cache_draw_env(const void *env);
void gpu_apply_disp_env(void *env);
void gpu_control_shadow(u32 command);
#endif
