/* SPDX-License-Identifier: GPL-3.0-or-later */
/* seq_spu.h -- PS1 SEQ parser that drives SPU core voice
 * registers directly.
 *
 * The caller advances playback from inside the audio callback. Every
 * call to seq_spu_advance() pops any MIDI events whose scheduled
 * sample-tick has arrived and translates them into SPU register
 * writes via spu_psx_write_reg().
 */
#ifndef SEQ_SPU_H
#define SEQ_SPU_H
#include "game/types.h"
#include "vab_loader.h"

typedef struct SeqSpuState SeqSpuState;

/* Start playback. seq_data must stay valid for the lifetime of the state.
 * bank is the VAB bank whose tones will be used to interpret program changes.
 * Returns a handle or NULL on failure. */
SeqSpuState *seq_spu_start(const u8 *seq_data, u32 seq_size, const VabBank *bank, int loop);

void         seq_spu_stop(SeqSpuState *s);

/* Advance playback: emit any events whose scheduled sample-tick has been
 * reached, after advancing the clock by `sample_frames` audio frames.
 * Call this from the SDL audio callback BEFORE spu_psx_mix. */
void         seq_spu_advance(SeqSpuState *s, u32 sample_frames);

/* Set global volume (0..127). */
void         seq_spu_set_volume(SeqSpuState *s, u8 vol);

/* Emergency stop all voices. */
void         seq_spu_all_off(SeqSpuState *s);

/* Frames remaining until the next scheduled event. The audio mixer uses
 * this to slice its mix at exact event boundaries (KEY_ON / KEY_OFF then
 * land on the intended sample instead of quantizing to a fixed grid).
 * Returns 0xFFFFFFFFu when there is no upcoming event. */
u32          seq_spu_frames_to_next_event(const SeqSpuState *s);

#endif
