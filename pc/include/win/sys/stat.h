/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sys/stat.h -- Windows build only (see pc/include/win/dlfcn.h).  The
 * MinGW header plus the POSIX two-argument mkdir(path, mode) the HAL uses
 * (mode has no meaning on Windows; the folder inherits its parent's ACL). */
#ifndef PC_WIN_SYS_STAT_H
#define PC_WIN_SYS_STAT_H

#include_next <sys/stat.h>
#include <io.h>
#include <direct.h>

static __inline__ int pc_win_mkdir(const char *path, int mode)
{
    (void)mode;
    return _mkdir(path);
}
#define mkdir(path, mode) pc_win_mkdir((path), (mode))

#endif
