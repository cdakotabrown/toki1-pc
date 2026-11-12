/* SPDX-License-Identifier: GPL-3.0-or-later */
/* dlfcn.h -- Windows build only (pc/include/win is on the include path when
 * PLATFORM=windows).  The port uses dladdr()/dlsym() on its own executable
 * for diagnostics and, in pc/hal/cpu_cost.c, to key the retail CPU-time
 * table on function names.  pc/hal/win/win_dl.c answers both from the
 * executable's COFF symbol table with the semantics the Linux build gets
 * from glibc over a -rdynamic image:
 *   - dladdr names an address only when it lies in a GLOBAL function (a
 *     static function, like a non-exported one on Linux, gives
 *     dli_sname == NULL);
 *   - dlsym(RTLD_DEFAULT, name) finds any global symbol (code, data, or an
 *     absolute PS1 address from pc/ld/psx_globals.syms). */
#ifndef PC_WIN_DLFCN_H
#define PC_WIN_DLFCN_H

typedef struct {
    const char *dli_fname;  /* the executable's path */
    void *dli_fbase;        /* its image base */
    const char *dli_sname;  /* the containing global function, or NULL */
    void *dli_saddr;        /* its address, or NULL */
} Dl_info;

#define RTLD_LAZY    0x0001
#define RTLD_NOW     0x0002
#define RTLD_GLOBAL  0x0100
#define RTLD_DEFAULT ((void *)0)

#ifdef __cplusplus
extern "C" {
#endif
int dladdr(const void *addr, Dl_info *info);
void *dlsym(void *handle, const char *name);
#ifdef __cplusplus
}
#endif

#endif
