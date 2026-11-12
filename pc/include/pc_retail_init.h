/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pc_retail_init.h -- initial values that are retail bytes, filled at run
 * time from the player's own PSX.EXE (pc/src/retail_init.c).
 *
 * The port's executable carries no retail data (a player download is the
 * program alone; pc/tools/audit_retail_bytes.py is the gate).  Host
 * definitions whose initial value is a copy of the retail image's bytes --
 * the generated fallback globals of pc/src/missing_globals_gen.c
 * (pc/tools/gen_missing_globals.py) -- are therefore defined zero and listed
 * in pc_retail_init[]; main() calls pc_retail_init_load() right after it
 * loads the executable, before any game code runs, so every one of them
 * holds the same bytes it held as an initialiser.
 */
#ifndef PC_RETAIL_INIT_H
#define PC_RETAIL_INIT_H

typedef struct {
    void *dst;              /* the host definition */
    unsigned int addr;      /* its retail address in PSX.EXE's image */
    unsigned int size;      /* bytes */
} PcRetailInit;

/* the generated table, ended by a { 0, 0, 0 } row */
extern const PcRetailInit pc_retail_init[];
extern const unsigned pc_retail_init_count;
/* the hand-written globals' (pc/src/globals.c) */
extern const PcRetailInit pc_retail_init_manual[];
extern const unsigned pc_retail_init_manual_count;

/* Copy every entry from the PS-X EXE at `exe_path`.  main() passes the
 * executable it loaded into PS1 RAM: the Japanese retail one (the values
 * these definitions always had) or, with [game] language = en, the English
 * one.  Most of these names link to their PS1 addresses
 * (pc/ld/psx_globals.syms), so `dst` is PS1 RAM itself: filling them from
 * the Japanese file in English mode wrote Japanese back over the English
 * executable's strings (docs/translation/BROKEN-STRINGS-AUDIT.md).  Returns
 * the number of entries filled, -1 when the file is not a PS-X EXE. */
int pc_retail_init_load(const char *exe_path);

#endif
