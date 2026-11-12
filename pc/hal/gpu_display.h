/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Read-only device state shared by presentation and parity observation. */
#ifndef PC_GPU_DISPLAY_H
#define PC_GPU_DISPLAY_H

#include "gpu_scanout.h"

void gpu_get_scanout_state(GpuScanoutState *state);

#endif
