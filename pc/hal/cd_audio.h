/* SPDX-License-Identifier: GPL-3.0-or-later */
/* cd_audio.h -- the CD-ROM controller's audio path (pc/hal/cd_audio.c). */
#ifndef CD_AUDIO_H
#define CD_AUDIO_H

#include "types.h"

/* Size of a raw Mode 2 sector without sync and header: 8-byte subheader,
 * 2,324 bytes of Form 2 data, 4-byte EDC (the extracted .XA/.STR layout). */
#define CD_AUDIO_RAW_SECTOR 2336

/* Install the controller's output as the SPU core's CD audio input. */
void cd_audio_init(void);

/* Drive events (pc/hal/cd.c).  `now` is the system clock in CPU ticks. */
void cd_audio_reset_decoder(u64 now);           /* Pause, ReadN/ReadS/Play start, seek, Init */
void cd_audio_set_filter(u8 file, u8 channel);  /* Setfilter */
void cd_audio_set_mute(int muted);              /* Mute / Demute */
/* An XA-ADPCM sector (realtime + audio subheader) read with Setmode bit 6;
 * `filter` is Setmode bit 3.  `raw` is CD_AUDIO_RAW_SECTOR bytes. */
void cd_audio_xa_sector(const u8 *raw, int filter, u64 now);

/* The ATV volume matrix: vol[0] CD-L -> SPU-L, vol[1] CD-L -> SPU-R,
 * vol[2] CD-R -> SPU-R, vol[3] CD-R -> SPU-L (0x80 = 1.0), as libcd's
 * CD_vol writes them, applied at once (its 0x20 write to 0x1F801803). */
void cd_audio_set_volume(const u8 vol[4]);

/* The SPU HAL is about to mix `frames` samples ending at tick `now` (the
 * deterministic per-VBlank mixer): the frames the core pops are the ones the
 * hardware would have played in (now - frames * 768, now]. */
void cd_audio_mix_window(u64 now, u32 frames);

/* The PC mixer's view of the CD input (pc/hal/spu.c, the player's voice
 * volume): while a tap is set, every frame the controller hands the SPU core
 * is also copied to `buf` (interleaved L/R, up to `cap` frames).  NULL
 * clears it.  Observation only: the core, its capture buffers and the
 * game's level reads (SD_CDLevelRead) get the same samples as without it. */
void cd_audio_tap(s16 *buf, int cap);
int cd_audio_tap_count(void);

/* The controller's XA-ADPCM decoder as a reusable piece: ADPCM history and
 * the 44.1 kHz interpolation ring of one stream.  cd_audio_xa_sector runs
 * the controller's own instance; the backlog's voice replay
 * (pc/hal/voice_replay.c) runs a private one over the same sectors, so a
 * replayed line is the exact PCM the controller produced (same history
 * from a decoder reset, same interpolation). */
typedef struct CdXaDecoder {
    s32 last[4];                /* L old, L older, R old, R older */
    s16 ring[2][32];
    u32 ring_p;
    u32 sixstep;
} CdXaDecoder;
typedef void (*CdXaEmit)(void *ctx, s16 l, s16 r);
void cd_xa_decoder_reset(CdXaDecoder *d);
/* One sector's 18 sound groups (`groups` = raw + 8) -> samples at the
 * sector's rate (stereo interleaved); returns the frame count. */
u32 cd_xa_adpcm(CdXaDecoder *d, const u8 *groups, int stereo, int eight_bit, s16 *out);
/* Those samples -> 44.1 kHz frames, handed to `emit` one by one. */
void cd_xa_resample(CdXaDecoder *d, const s16 *in, u32 frames, int stereo, int half,
                    CdXaEmit emit, void *ctx);
/* Would cd_audio_xa_sector take this sector into its stream now (the
 * file/channel filter, the current-file lock, channel-255 junk)?  Pure:
 * reads the controller's state, changes nothing. */
int cd_audio_accepts(const u8 *raw, int filter);

/* Diagnostics: XA sectors played / dropped / filtered since start. */
void cd_audio_stats(u32 *played, u32 *dropped, u32 *skipped);
/* Diagnostics: frames queued in the controller's FIFO now. */
u32 cd_audio_fifo_frames(void);
/* Diagnostics: gaps in the middle of a stream (the FIFO ran dry within
 * 0.3 s of a queued sector) and the silent frames they cost. */
void cd_audio_underruns(u32 *gaps, u32 *frames);
/* The real-time mixer, under the audio lock: drop the oldest frames so at
 * most `keep` stay (fast-forward ended); the margin it plays streams late. */
void cd_audio_trim(u32 keep);
u32 cd_audio_margin_frames(void);

#endif /* CD_AUDIO_H */
