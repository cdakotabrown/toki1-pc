/* SPDX-License-Identifier: GPL-3.0-or-later */
/* quicksave.h -- the player's save states (PC layer): quicksave slots,
 * named saves, autosaves, the resume state (pc/hal/quicksave.c), the save
 * browser page of the settings menu (pc/hal/save_browser.c) and the
 * automatic saves (pc/hal/save_auto.c).
 *
 * Every save is an ENTRY of <saves>/quicksave/, three files named by its id:
 * ID.tks (the state), ID.png (160x120 thumbnail), ID.ini (the index;
 * docs/pc-port/ENHANCED-MODE.md "Quicksave slots").  Ids:
 *
 *   slot1..slot9   the quicksave slots (F5 / F9, ] / [)
 *   slot0          the automatic undo: the machine before the last load
 *   save0001..     the save browser's NEW SAVE (no limit but the disk)
 *   auto1..auto9   the weekly autosaves, rotating ([saves] autosave_slots)
 *   ending         before the ending ([saves] autosave_ending)
 *   resume         written on quit, offered at the next launch ([saves] resume)
 *
 * The game's own memory-card saves are a different thing and untouched: a
 * load keeps the live card image (quicksave.c card_hold).
 */
#ifndef PC_QUICKSAVE_H
#define PC_QUICKSAVE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { QS_KIND_QUICK, QS_KIND_UNDO, QS_KIND_SAVE, QS_KIND_AUTO, QS_KIND_ENDING,
       QS_KIND_RESUME, QS_KIND_NONE };

#define QS_ID_MAX 16
#define QS_THUMB_W 160
#define QS_THUMB_H 120

typedef struct QsEntry {
    char id[QS_ID_MAX];
    int kind;                   /* QS_KIND_* (from the id) */
    int number;                 /* slot / save / autosave number, 0 otherwise */
    int has_state;              /* ID.tks exists */
    int has_index;              /* ID.ini was read */
    int same_build;             /* the index names this executable's build */
    int in_progress;            /* the index's [game] block is there */
    long long saved_unix;       /* 0 when unknown */
    long long saved_ms;         /* the same in ms (saved_unix_ms; orders saves of one second) */
    char saved[24];             /* "YYYY-MM-DD HH:MM:SS" local time, or "" */
    int year, month, day, youbi, pass_days, week;
    char weekday[4];
    int stats[9];               /* para_tai .. para_str */
    char scene[16];
    char profile[16];
    char label[32];
} QsEntry;

/* ---- ids ---- */
int  qs_id_kind(const char *id, int *number);   /* QS_KIND_NONE when not an id */
void qs_id_name(const char *id, char *out, size_t cap);   /* "QUICK SLOT 3" ... */

/* ---- entries ---- */
/* Read one entry's index (and whether its state exists); 0 when it has
 * neither a state nor an index. */
int  qs_entry_read(const char *id, QsEntry *e);
/* Every entry of the folder (state or index present), newest first;
 * returns how many were written to OUT (at most MAX). */
int  qs_entries(QsEntry *out, int max);
/* The next unused saveNNNN id; 0 when none is left. */
int  qs_new_save_id(char *out, size_t cap);
/* Delete an entry's three files; 0 when something was deleted. */
int  qs_delete_id(const char *id);
/* The entry's thumbnail into RGB (QS_THUMB_W * QS_THUMB_H * 3); 1 when read. */
int  qs_thumb_read(const char *id, unsigned char *rgb);

/* ---- requests (acted on at the next main-loop boundary) ---- */
void qs_request_save_id(const char *id);
void qs_request_load_id(const char *id);
/* The int-slot forms (hotkeys, toolkit qsave / qload): -1 = the selected slot */
void qs_request_save(int slot);
void qs_request_load(int slot);
int  qs_pending(void);
/* 1 when save states work in this executable; else the player is told
 * (a toast) and 0 is returned (pc/hal/dbg_state.c dbg_state_unavailable) */
int  qs_available(void);
int  qs_last_result(void);

/* ---- at the boundary (pc/hal/save_auto.c) ---- */
/* Save ID now: 0 the state is captured and being written, 1 not now (a
 * device operation in flight or the writer busy: try at a later boundary),
 * -1 failed.  QUIET: no toast. */
int  qs_save_now(const char *id, int quiet);
/* Load ID now (with the undo slot and the card kept); 0 ok. */
int  qs_load_now(const char *id, int quiet);
/* Wait for the state writer (shutdown, before a load). */
void qs_writer_wait(void);
void qs_shutdown(void);
/* 1 when a game is under way (scene past the name entry, a valid date) */
int  qs_game_in_progress(void);
void qs_notify(const char *text);

/* ---- pc/hal/save_auto.c ---- */
void sa_boundary(void);         /* qs_boundary calls it */
void sa_after_load(void);       /* a state was just loaded */
int  sa_autosave_enabled(void);
int  sa_resume_enabled(void);

/* ---- pc/hal/save_browser.c ---- */
void save_browser_register(void);   /* the presenter, once the menu exists */
void save_browser_resume_prompt(void);

#ifdef __cplusplus
}
#endif
#endif /* PC_QUICKSAVE_H */
