/* SPDX-License-Identifier: GPL-3.0-or-later */
/* dbg_platform.h -- what the debug toolkit and the player's save states need
 * from the host OS, behind one small interface (docs/pc-port/DEBUG-TOOLS.md
 * "State format").
 *
 *   pc/hal/dbg_platform_posix.c   Linux (ELF, procfs, mmap, ucontext, BSD
 *                                 sockets; macOS would add its Mach-O reader
 *                                 here or beside it)
 *   pc/hal/win/win_dbg_platform.c Windows (PE/COFF, VirtualQuery,
 *                                 VirtualAlloc, fibers, Winsock)
 *
 * The portable parts -- the state format, the rules of what a state holds,
 * the commands, the console protocol -- are pc/hal/dbg_state.c,
 * dbg_cmds.c and dbg_console.c, which call only this.
 */
#ifndef PC_DBG_PLATFORM_H
#define PC_DBG_PLATFORM_H

#include <stddef.h>

/* ---- the running executable's data symbols ----
 * Every named object of the executable's writable sections (.data, .bss
 * and the like), sorted by address.  FILE is the source file of a static
 * (`quicksave.c`), NULL for a global; SIZE is the object's size (Windows:
 * up to the next symbol, COFF records no sizes).  Foreign objects (libc,
 * SDL, the C++ runtime) are in the list too: pc/hal/dbg_state.c picks the
 * project's own with the build's inventory (pc/tools/gen_state_inventory.py). */
typedef struct {
    unsigned long addr, size;
    const char *name;
    const char *file;
    int data;                   /* in a writable data section of the image */
} DbgImgSym;

/* 1 when the table is read; the arrays stay valid for the process */
int dbg_plat_image(const DbgImgSym **syms, int *n);
/* why dbg_plat_image failed ("" otherwise) */
const char *dbg_plat_image_error(void);
/* the executable's writable data sections (Linux: __data_start.._end;
 * Windows: .data and .bss, not .rdata between them): up to CAP [lo, hi)
 * pairs, returns how many */
int dbg_plat_data_ranges(unsigned long (*out)[2], int cap);
/* this executable's build id (hex; "" when unreadable): GNU build-id on
 * Linux, the PE debug directory's CodeView signature on Windows (both
 * written by `ld --build-id`) */
const char *dbg_plat_build_id(void);
/* "linux-x86_64", "windows-x86_64" */
const char *dbg_plat_name(void);
/* The host's memory map in /proc/self/maps format (Windows: VirtualQuery,
 * one line per region, module paths for images, [heap] for the process
 * heap): savestate_tool.py audit classifies host pointers with it.  A
 * dbg_mem buffer (NUL-terminated), NULL on failure. */
char *dbg_plat_maps(unsigned long *len);

/* ---- memory and files ----
 * Toolkit buffers never come from malloc (Linux: the brk heap can hold the
 * MMIO page, dbg_state.c); files are written whole and replaced
 * atomically. */
void *dbg_mem_alloc(unsigned long n);
void dbg_mem_free(void *p);
void *dbg_mem_realloc(void *p, unsigned long n);
/* the whole file in a dbg_mem buffer (NUL-terminated); NULL on error */
unsigned char *dbg_read_file(const char *path, unsigned long *len);
/* create or truncate PATH with N bytes; 1 ok */
int dbg_write_file(const char *path, const void *data, unsigned long n);
/* rename FROM over TO, replacing it atomically (Windows: MoveFileEx
 * REPLACE_EXISTING | WRITE_THROUGH, rename() refuses an existing TO); 0 ok */
int dbg_replace_file(const char *from, const char *to);

/* ---- the toolkit's own stack ----
 * Run FN on a separate stack (Linux: ucontext, Windows: a fiber), so its
 * work leaves the game stack as an unused run would.  Called at the
 * main-loop boundary only. */
void dbg_plat_run_on_toolkit_stack(void (*fn)(void));
/* Make the SIZE bytes below the caller's stack pointer readable and
 * writable (Windows commits a thread stack page by page; the save copies
 * and a load writes that window).  Called on the game stack. */
void dbg_plat_stack_prepare(unsigned long size);

/* ---- the live console's transport (pc/hal/dbg_console.c) ----
 * Handles are small integers >= 0, or -1. */
int dbg_sock_listen_tcp(int port);          /* 127.0.0.1 only */
int dbg_sock_listen_unix(const char *path); /* -1 where the host has none */
int dbg_sock_accept(int listener);          /* non-blocking: -1 when none */
/* >0 bytes, 0 end of input, -1 nothing yet, -2 error */
long dbg_sock_recv(int h, char *buf, unsigned long cap);
void dbg_sock_send(int h, const char *buf, unsigned long n);   /* never SIGPIPEs */
void dbg_sock_close(int h);
const char *dbg_sock_error(void);
/* stdin as a non-blocking line source (a reader thread on Windows):
 * returns a handle for dbg_sock_recv, -1 on failure */
int dbg_stdin_open(void);

/* setenv for the console's `debug` command */
void dbg_plat_setenv(const char *name, const char *value);

#endif
