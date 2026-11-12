/* SPDX-License-Identifier: GPL-3.0-or-later */
/* windows.h -- Windows build only (pc/include/win is on the include path when
 * PLATFORM=windows).  Every translation unit of the port is compiled with
 * pc/include/game/types.h forced in, which defines the PS1 `RECT`
 * (s16 x, y, w, h); <windows.h> defines its own `RECT` (LONG left, top,
 * right, bottom).  The Windows one is renamed while the system header is
 * read, so the name keeps meaning the PS1 type in the port's code. */
#ifndef PC_WIN_WINDOWS_H
#define PC_WIN_WINDOWS_H

#define RECT PC_WIN32_RECT
#include_next <windows.h>
#undef RECT

#endif
