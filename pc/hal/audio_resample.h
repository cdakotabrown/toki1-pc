/* SPDX-License-Identifier: GPL-3.0-or-later */
/* audio_resample.h -- 44.1 kHz -> 48 kHz for the real-time audio device
 * ([audio] output_rate = 48000, enhanced; pc/hal/spu.c, pc/hal/spu_quality.c).
 *
 * The SPU runs at 44.1 kHz and the mixer stays there (the register queue,
 * the hashes, the dumps, the deterministic mix); the device callback asks
 * this resampler for its 48 kHz frames and it pulls exactly the 44.1 kHz
 * frames they need.  Exact rational ratio 160/147, polyphase windowed sinc:
 * 160 phases x 64 taps, Kaiser (beta 8.5), cutoff 20.25 kHz -- flat to
 * ~18.5 kHz, images and aliases at least ~80 dB down from 22 kHz; 32 input
 * samples (0.73 ms) of delay.  Deterministic, no allocation after init.
 */
#ifndef PC_AUDIO_RESAMPLE_H
#define PC_AUDIO_RESAMPLE_H

#include "types.h"

#define AUDIO_RS_IN_RATE 44100
#define AUDIO_RS_OUT_RATE 48000
#define AUDIO_RS_MAX_OUT 8192          /* device frames per call */

/* Produces the input frames (stereo s16) the resampler asks for. */
typedef void (*AudioRsPull)(void *ctx, s16 *in, int frames);

/* Reset to silence (history zero, phase 0). */
void audio_rs_reset(void);
/* How many 44.1 kHz frames the next `out_frames` 48 kHz frames need. */
int  audio_rs_needed(int out_frames);
/* Fill `out` (stereo s16, out_frames <= AUDIO_RS_MAX_OUT): pulls
 * audio_rs_needed(out_frames) frames through `pull` first. */
void audio_rs_run(s16 *out, int out_frames, AudioRsPull pull, void *ctx);

#endif
