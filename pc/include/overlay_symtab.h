/* SPDX-License-Identifier: GPL-3.0-or-later */
/* overlay_symtab.h -- overlay address -> native function tables
 * (generated pc/src/overlay_symtab.c, used by pc/hal/overlay.c). */
#ifndef OVERLAY_SYMTAB_H
#define OVERLAY_SYMTAB_H
#include "types.h"

typedef struct { u32 addr; void *fn; } OverlaySym;
typedef struct { const char *name; const OverlaySym *tab; int n; } OverlayTable;

extern const OverlayTable g_overlay_tables[];
extern const int g_overlay_table_count;

/* the overlay whose code is in the window: set when the CD HAL delivers an
 * .EXN read to 0x80132000 (pc/hal/cd.c) */
void hal_overlay_set_loaded(const char *exn_path);
const char *hal_overlay_loaded(void);
/* the native function at a PS1 code address of the loaded overlay, or NULL */
void *hal_overlay_lookup(u32 addr);
/* SIGSEGV fault fix-up: a call through a raw PS1 overlay code address */
int hal_overlay_fault_redirect(void *ucontext);
#endif
