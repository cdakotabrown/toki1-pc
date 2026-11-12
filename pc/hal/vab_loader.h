/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * vab_loader.h -- PS1 VAB soundbank loader for the PC port.
 *
 * Parses .VH (header) + .VB (body) files, uploads PCM samples to the SPU core's
 * SPU RAM, and provides a tone-lookup API for the SEQ driver.
 */
#ifndef VAB_LOADER_H
#define VAB_LOADER_H
#include "game/types.h"

typedef struct VabBank VabBank;

typedef struct {
    u32 spu_addr;       /* sample start in SPU RAM */
    u32 spu_loop_addr;  /* loop address (equal to spu_addr if no explicit loop) */
    u8  center_note;    /* MIDI note the sample plays at natural pitch */
    s8  pitch_fine;     /* fine tuning, signed cents/128 */
    u8  vol;            /* 0..127 tone volume */
    u8  pan;            /* 0..127 tone pan, 64=center */
    u16 adsr1;          /* raw SPU ADSR register 1 value */
    u16 adsr2;          /* raw SPU ADSR register 2 value */
    u8  priority;
    u8  min_note;       /* tone maps MIDI notes in [min_note, max_note] */
    u8  max_note;
    u8  program;        /* program number this tone belongs to */
} VabTone;

/* Load VH+VB. Uploads samples to SPU RAM starting at spu_base_addr.
 * Returns bank handle or NULL on failure. */
VabBank *vab_load(const char *vh_path, const char *vb_path, u32 spu_base_addr);

void vab_free(VabBank *bank);

/* Look up the tone for a program+note. Returns 1 on hit, 0 on miss. */
int vab_find_tone(const VabBank *bank, u8 program, u8 note, VabTone *out);

/* Debug: dump bank stats. */
void vab_dump(const VabBank *bank);

#endif
