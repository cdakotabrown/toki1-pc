/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_HAL_NBN_TRACE_H
#define PC_HAL_NBN_TRACE_H

#include "types.h"

/* Behavior-neutral NBN provenance tracing. All functions are no-ops unless
 * NBN_TRACE is set to a non-zero value. */
void nbn_trace_register_load(const char *path, const void *dest,
                             u32 file_offset, u32 length);
void nbn_trace_load_image(const void *data, u32 length,
                          s32 x, s32 y, s32 w_words, s32 h);
void nbn_trace_move_image(s32 sx, s32 sy, s32 w_words, s32 h,
                          s32 dx, s32 dy);

#endif
