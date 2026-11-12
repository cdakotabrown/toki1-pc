/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PS1-word printf adapter and profiled CPU work (not host libc printf). */
#ifndef PC_BIOS_PRINTF_H
#define PC_BIOS_PRINTF_H
#include "types.h"
#include <stdarg.h>
s32 hal_bios_vprintf(const char *format, va_list args);
s32 func_800B4850(const char *format, ...);
#endif
