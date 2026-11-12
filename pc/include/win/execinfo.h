/* SPDX-License-Identifier: GPL-3.0-or-later */
/* execinfo.h -- Windows build only (see pc/include/win/dlfcn.h).  The crash
 * reporter's backtrace: RtlCaptureStackBackTrace for the frames, the
 * executable's COFF symbols for the names (pc/hal/win/win_dl.c). */
#ifndef PC_WIN_EXECINFO_H
#define PC_WIN_EXECINFO_H

#ifdef __cplusplus
extern "C" {
#endif
int backtrace(void **buffer, int size);
void backtrace_symbols_fd(void *const *buffer, int size, int fd);
#ifdef __cplusplus
}
#endif

#endif
