/* SPDX-License-Identifier: GPL-3.0-or-later */
/* extras_state.h -- read-only decoding of the game state the extras need
 * (pc/hal/extras.c: endings, achievements; pc/hal/extras_gallery.c:
 * pictures).
 *
 * Everything here READS: PS1 RAM at fixed addresses (the matched C names
 * are given beside each address; addresses, not names, are the contract,
 * since symbols are still being renamed), and the CD layer's record of
 * which disc file was read where.  Nothing writes the machine.
 *
 * SDL-free and RAM-free in its interface: ExView is a plain struct, so the
 * detectors in extras.c can be unit-tested with hand-built views
 * (pc/tests/test_extras.c). */
#ifndef PC_EXTRAS_STATE_H
#define PC_EXTRAS_STATE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EX_GIRL_RECORDS 13      /* girl_tbl records 0..12 (11 Miharu, 12 Rei) */
#define EX_ENDINGS 14           /* now_girl 0..13 after ending_select_init */
#define EX_PARAMS 9

/* now_step values (overlay_load_init.c / schedule_day_main.c; the evidence
 * per achievement is in extras.c) */
#define EX_STEP_LOADER 2
#define EX_STEP_ENDING 99

typedef struct {
    int tokimeki, yuukou, bomb; /* integer halves of the 16.16 gauges */
    int met;                    /* girl flag word bit 1 */
} ExGirl;

typedef struct {
    int in_game;                /* a game in progress (quicksave.c's test) */
    int step, sub, subsub;      /* now_step / now_sub_step / now_sub_sub_step */
    int prev_step;              /* prev_step (the loader's target while step == 2) */
    int year, month, day, weekday;  /* year 1995..1998; weekday 0 = Monday */
    int pass_days;
    int now_girl;               /* 0x800EF6BF (screen_w.ending) */
    int scene_girl;             /* the girl of the current girl scene, -1 none */
    int params[EX_PARAMS];      /* stamina .. stress (s16 high halves) */
    int basyo_pack;             /* u16: club in bits 12..15 */
    ExGirl girl[EX_GIRL_RECORDS];
} ExView;

/* Fill V from PS1 RAM (the running port only). */
void ex_state_read(ExView *v);

/* Names (uppercase ASCII for the presenter's 5x7 font).  ex_girl_name:
 * date_name_set_tbl0's sixteen entries, romanised as
 * translation/localization/CHARACTER-BIBLE.md; ex_girl_short the given
 * name only.  NULL outside the table. */
const char *ex_girl_name(int g);
const char *ex_girl_short(int g);
/* The girl a disc file's name stands for (EVENT/EV_FU01.BIN -> 0), or -1. */
int ex_girl_of_code(const char *two_letters);

/* ---- disc files read into RAM (pc/hal/cd.c deliver_sector) ----
 * The CD layer reports each data read's first sector: the disc path and
 * the PS1 destination.  Kept: the newest reads (a ring); the newest one
 * covering an address wins, as RAM is reused. */
void extras_note_cd_load(const char *path, unsigned ps1_dest, unsigned bytes);
/* The disc path (below CDROM/, e.g. "BS/D_09_02.BS") of the newest read
 * covering PS1 address ADDR, with *offset inside it; 0 when none. */
int ex_file_at(unsigned addr, char *rel, size_t cap, unsigned *offset);
/* Forget every read (a state load: RAM is another session's). */
void ex_files_reset(void);
/* The CDROM-relative part of a host path ("./pc/assets/raw/CDROM/BS/X.BS"
 * -> "BS/X.BS"); the path itself when it has no CDROM/ component. */
const char *ex_disc_rel(const char *path);

#ifdef __cplusplus
}
#endif
#endif
