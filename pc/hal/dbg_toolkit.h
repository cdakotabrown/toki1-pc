/* SPDX-License-Identifier: GPL-3.0-or-later */
/* dbg_toolkit.h -- the PC port's debug toolkit (DEBUG-ONLY, off by default).
 *
 *   pc/hal/dbg_state.c    save states: the emulated machine and the port's
 *                         native game state, by name (that file's header)
 *   pc/hal/dbg_platform.h the host layer under all of it (Linux, Windows)
 *   pc/hal/dbg_cmds.c     the frame-boundary dispatcher, PC_SAVESTATE /
 *                         PC_LOADSTATE, the commands and command scripts
 *                         (PC_DBG_CMDS)
 *   pc/hal/dbg_console.c  the live console (PC_DBG_CONSOLE) feeding them
 *   pc/hal/quicksave.c    the player's quicksave slots, built on the states
 *
 * Nothing here runs unless an environment variable, a hotkey or a console
 * command asks for it; with none of them the game loop calls
 * dbg_frame_boundary(), which returns at once.
 */
#ifndef PC_DBG_TOOLKIT_H
#define PC_DBG_TOOLKIT_H

#include <stddef.h>

/* ---- pc/hal/dbg_cmds.c ---- */
void dbg_toolkit_init(void);       /* once, before the main loop */
void dbg_frame_boundary(void);     /* top of every main-loop iteration */
void dbg_request_save(const char *path);   /* hotkeys: act at the next boundary */
void dbg_request_load(const char *path);
void dbg_request_pause_toggle(void);
void dbg_request_step(int frames);
void dbg_toolkit_activate(void);   /* run the boundary from now on (quicksave) */

/* ---- pc/hal/dbg_state.c ---- */
/* Returns 0 on success; on failure a message is in *why.  A save that
 * cannot be taken yet (a heap-backed device operation is in flight) returns
 * 1 with *why set: try again at a later boundary. */
int dbg_state_save(const char *path, const char **why);
int dbg_state_load(const char *path, const char **why);
/* dbg_state_save in two halves: the capture (at a boundary, same return
 * values as dbg_state_save) copies everything the file will hold; the
 * write may then run on any thread, writes PATH.tmp, renames it over PATH
 * and frees the snapshot (0 ok, -1 failed, *why set either way). */
typedef struct DbgStateSnap DbgStateSnap;
int dbg_state_capture(DbgStateSnap **out, const char **why);
int dbg_state_write(DbgStateSnap *snap, const char *path, const char **why);
void dbg_state_snap_free(DbgStateSnap *snap);
/* Load the symbol table and build the rules now, so the first capture
 * does not pay for it (interactive runs, pc/hal/save_auto.c). */
void dbg_state_prepare(void);
/* NULL when this executable can take and load states; else why not (no
 * symbol table, no inventory).  The player's save features check it and
 * say so instead of failing. */
const char *dbg_state_unavailable(void);
const char *dbg_state_build_id(void);
/* Symbol lookup over the executable's own symbol table (globals, statics,
 * the PS1 data symbols linked at their PS1 addresses).  NAME may be
 * `file.c:name` to pick a static of one source file.  Returns the address
 * or NULL; *size gets the symbol size (0 if unknown). */
void *dbg_sym_lookup(const char *name, unsigned long *size);
/* Name of the data symbol containing ADDR (`file.c:name+0xOFF`), or NULL. */
const char *dbg_sym_name(const void *addr, char *buf, size_t cap);
/* The address range a peek/poke may touch (PS1 RAM, the MMIO window and the
 * executable's writable data sections). */
int dbg_addr_ok(const void *addr, unsigned long bytes);

/* In-memory states (pc/hal/rewind.c): the byte ranges a load restores --
 * the native records minus their KEEP pieces and minus EXCLUDE (symbols
 * in dbg_sym_lookup spelling, NULL-terminated; may be NULL), then PS1 RAM,
 * the MMIO window and the game stack below the boundary -- in that order,
 * adjacent ranges merged.  Fills at most CAP; returns the count (more than
 * CAP: call again with a bigger array), -1 when the symbol table cannot
 * be read.  Same build, same process: a copy of these ranges taken at a
 * boundary and copied back at a later boundary is a dbg_state_load of a
 * state saved there (pc/tools/test_rewind.py). */
typedef struct { unsigned long addr, size; } DbgRange;
int dbg_state_ranges(DbgRange *out, int cap, const char *const *exclude);
/* NULL when a capture may be taken now; else the QUIESCE rule that is busy
 * (a heap-backed device operation in flight). */
const char *dbg_state_busy(void);
/* After any restore of the machine -- dbg_state_load calls it with
 * DBG_RESTORE_FILE, pc/hal/rewind.c with DBG_RESTORE_REWIND: tells every
 * session-side observer (trace clock, extras, hi-res shadow, text assist,
 * pacing, autosave, and -- for a file -- the rewind buffer).  The single
 * list: docs/pc-port/STATE-MACHINES.md "After a restore". */
enum { DBG_RESTORE_FILE = 0, DBG_RESTORE_REWIND = 1 };
void dbg_state_after_restore(int how);

#endif
