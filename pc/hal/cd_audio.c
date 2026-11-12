/* SPDX-License-Identifier: GPL-3.0-only */
/* cd_audio.c -- the CD-ROM controller's audio path: XA-ADPCM sectors from
 * the drive to the SPU's CD audio input.
 *
 * On the console the drive decodes the XA-ADPCM sectors it reads itself
 * (Setmode bit 6, "XA-ADPCM enable"): an audio sector (subheader submode
 * realtime + audio) is not delivered to the CPU and raises no interrupt; the
 * controller decodes it, resamples it to 44.1 kHz and feeds the SPU's CD
 * audio input through its ATV volume matrix.  The SPU mixes that input with
 * its CD volume (0x1F801DB0/2) when SPUCNT bit 0 is set, and writes every
 * frame it receives to the CD capture buffers at SPU RAM 0x000 (left) and
 * 0x400 (right), where the game's SD_CDLevelRead reads the voice level for
 * the lip-sync (kuchi) animations.
 *
 * The drive model (pc/hal/cd.c) calls cd_audio_xa_sector() at the tick each
 * sector is read -- 150 sectors per second at 2x, so a 1-in-32 interleaved
 * 18.9 kHz mono voice channel arrives every 32 sectors -- and the vendored
 * SPU core pops one frame per output sample through
 * cd_audio_frame() (stubs/cdrom.h).  Everything between is the hardware's
 * behaviour, per psx-spx ("CDROM XA Audio ADPCM Compression", "XA-ADPCM
 * Interpolation"):
 *
 *   Setfilter / Setmode bit 3    only sectors of the filter's file/channel;
 *                                without it, the first file/channel read
 *                                (channel 255 junk sectors skipped) until EOF
 *   ADPCM                        18 sound groups x 4 (8-bit) or 8 (4-bit)
 *                                blocks of 28 samples; filters (0,0) (60,0)
 *                                (115,-52) (98,-55) /64, s16 saturation,
 *                                per-channel history
 *   37.8 kHz -> 44.1 kHz         the 7-table zigzag interpolator over a
 *                                32-sample ring (7 outputs per 6 inputs)
 *   18.9 kHz -> 44.1 kHz         7 outputs per 3 inputs with the same
 *                                filter's 25-tap phases (the weights
 *                                Mednafen and DuckStation use; the
 *                                resampler is derived from DuckStation's,
 *                                see the notice below; the retail reference
 *                                for parity is the reference emulator)
 *   ATV matrix                   applied as the SPU takes each frame
 *   Mute (CdlMute), ADPCM mute   the sector is decoded (history kept) but
 *                                nothing reaches the FIFO
 *   decoder reset                Pause, the start of a read/play, a seek,
 *                                Init: history, ring and FIFO cleared
 *
 * The FIFO and the clock.  The controller keeps decoded frames in a FIFO the
 * SPU drains at 44.1 kHz (one frame every 768 CPU ticks); a sector is
 * dropped undecoded when more than 10 frames are still queued on arrival
 * (DuckStation's low watermark; with the drive's sector rate and the
 * channel interleave matched, the FIFO is empty when the next sector of the
 * channel lands).  The PC's deterministic mixer (spu_parity_mix, once per
 * VBlank) produces each VBlank's samples at the VBlank, after the sectors
 * that arrived during it, so the FIFO is kept on the system clock instead:
 * cd_audio_mix_window() records the tick the last batch ended at, and a
 * sector arriving at tick T counts the (T - end) / 768 frames the SPU would
 * already have taken -- from the FIFO, or as silence when it ran dry, which
 * is queued as zero frames so the new sector starts sounding at T and not at
 * the start of the batch.  The real-time SDL mixer has no common clock with
 * the game; there the FIFO just buffers (up to 0.5 s).
 *
 * CD-DA (CdlPlay of the audio track) is not decoded: the game never issues
 * it (the CD queue's command 3 has no caller in the matched code).
 *
 * Copyright (C) 2026 cdakotabrown and contributors.
 * 18.9 kHz XA resampler (k_half table, cd_xa_resample) derived from
 * DuckStation (C) Connor McLaughlin, GPL-3.0 (rev ee538ae0e); interpolation
 * weights from Mednafen (GPL-2.0-or-later).
 *
 * This file is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License, version 3, as
 * published by the Free Software Foundation (pc/LICENSE).  Because it
 * contains DuckStation-derived code, which DuckStation licenses under
 * GPL-3.0 without an "or later" grant, the file as a whole is GPL-3.0-only;
 * the rest of the PC port is GPL-3.0-or-later.  It is distributed WITHOUT
 * ANY WARRANTY; see the licence for details.
 */
#include "cd_audio.h"
#include "spu_psx.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define TICKS_PER_SAMPLE  768u          /* 33,868,800 / 44,100 */
#define FIFO_FRAMES       (44100u * 2u)
#define FIFO_LOW_WATERMARK 10u
#define RELAXED_FIFO_LIMIT 22050u
#define RING_SIZE         32u
#define RING_MASK         (RING_SIZE - 1u)

/* subheader submode bits */
#define SUBMODE_EOF       0x80
#define SUBMODE_REALTIME  0x40
#define SUBMODE_AUDIO     0x04
/* subheader coding bits */
#define CODING_STEREO     0x01
#define CODING_HALF_RATE  0x04
#define CODING_8BIT       0x10

/* The SPU HAL's audio-device lock (real-time mixer thread); weak so the
 * harnesses that link this file alone build. */
extern void spu_hal_audio_lock(void) __attribute__((weak));
extern void spu_hal_audio_unlock(void) __attribute__((weak));
static void lock(void)   { if (spu_hal_audio_lock) spu_hal_audio_lock(); }
static void unlock(void) { if (spu_hal_audio_unlock) spu_hal_audio_unlock(); }

static u32 g_fifo[FIFO_FRAMES];         /* left | right << 16 */
static u32 g_fifo_head, g_fifo_count;

/* the controller's decoder: ADPCM history, the interpolation ring */
static CdXaDecoder g_dec = { { 0, 0, 0, 0 }, { { 0 } }, 0, 6 };

static u8  g_filter_file, g_filter_channel;
static u8  g_cur_file, g_cur_channel;
static int g_cur_set;
static int g_muted;
static u8  g_matrix[2][2] = { { 0x80, 0x00 }, { 0x00, 0x80 } };   /* [from L/R][to L/R] */

static int g_have_window;               /* the deterministic mixer runs */
static u64 g_window_end;                /* the tick its last sample completed at */

static u32 g_played, g_dropped, g_skipped;
/* Diagnostics: frames the SPU found the FIFO empty since it last had data;
 * a sector that arrives after a dry spell shorter than 0.3 s ends a gap in
 * the middle of a stream (an underrun), not the silence between two. */
static u32 g_dry_frames;
static u32 g_underrun_frames, g_underruns;
static int g_gap_stream;                /* a stream is running (no reset since) */
static u8  g_gap_file, g_gap_channel;   /* ... and which */

static int xa_debug(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("PC_DEBUG");
        on = e && (strstr(e, "xa") || !strcmp(e, "all")) ? 1 : 0;
    }
    return on;
}

/* ---- FIFO ---- */
static void fifo_push(s16 l, s16 r) {
    if (g_fifo_count >= FIFO_FRAMES) return;
    g_fifo[(g_fifo_head + g_fifo_count) % FIFO_FRAMES] = (u32)(u16)l | ((u32)(u16)r << 16);
    g_fifo_count++;
}

/* frames the SPU has taken (or played as silence) since the last batch */
static u32 consumed_by(u64 now) {
    if (!g_have_window || now <= g_window_end) return 0;
    return (u32)((now - g_window_end) / TICKS_PER_SAMPLE);
}

static s16 clamp16(s32 v) { return (s16)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

static s16 apply_volume(s16 s, u8 vol) { return (s16)(((s32)s * (s32)vol) >> 7); }

static s16 *g_tap;
static int g_tap_cap, g_tap_n;

void cd_audio_tap(s16 *buf, int cap) { g_tap = buf; g_tap_cap = cap; g_tap_n = 0; }
int cd_audio_tap_count(void) { return g_tap_n; }

static int cd_audio_frame_raw(s16 *left, s16 *right);

/* The SPU core's per-sample fetch (stubs/cdrom.h).  Runs inside the mix. */
static int cd_audio_frame(s16 *left, s16 *right) {
    int r = cd_audio_frame_raw(left, right);
    if (g_tap && g_tap_n < g_tap_cap) {
        g_tap[g_tap_n * 2] = *left;
        g_tap[g_tap_n * 2 + 1] = *right;
        g_tap_n++;
    }
    return r;
}

/* The real-time mixer's jitter margin (TOKI_XA_MARGIN_MS, default 70; 0 =
 * none).  The deterministic mixer keeps the FIFO on the system clock (see
 * the top of this file); the real-time SDL mixer has no common clock with
 * the game, and the two meet at very different granularities: the drive
 * model queues sectors on the game thread, in batches at the VBlanks it
 * processes (16.7 ms apart, later when the host stalls), while the device
 * pulls 1,024 frames (23.2 ms) per callback.  A FIFO that starts playing
 * the moment the first sector lands runs dry whenever the next one is a
 * little late: measured on new_game (real time, both profiles), 64..1,440
 * frame gaps inside the boot movie's and the voice lines' streams.  So a
 * stream that starts (or restarts after running dry) plays only once its
 * first data has waited the margin; the FIFO then carries that much
 * slack.  Cost: voice and movie sound start the margin later. */
static u32 xa_margin_frames(void) {
    static long frames = -1;
    if (frames < 0) {
        const char *e = getenv("TOKI_XA_MARGIN_MS");
        long ms = e && *e ? atol(e) : 70;
        if (ms < 0) ms = 0;
        if (ms > 500) ms = 500;
        frames = ms * 441 / 10;
    }
    return (u32)frames;
}

static int cd_audio_frame_raw(s16 *left, s16 *right) {
    u32 f;
    s16 l, r;
    static int priming = 1;
    static u32 primed;
    if (!g_fifo_count) {
        g_dry_frames++;                 /* diagnostics: see cd_audio_xa_sector */
        priming = 1;
        primed = 0;
        *left = *right = 0;
        return 0;
    }
    if (priming && !g_have_window && ++primed <= xa_margin_frames()) {
        *left = *right = 0;             /* the stream's first data waits the margin */
        return 0;
    }
    priming = 0;
    g_dry_frames = 0;
    f = g_fifo[g_fifo_head];
    g_fifo_head = (g_fifo_head + 1) % FIFO_FRAMES;
    g_fifo_count--;
    l = (s16)(f & 0xFFFF);
    r = (s16)(f >> 16);
    *left  = clamp16((s32)apply_volume(l, g_matrix[0][0]) + apply_volume(r, g_matrix[1][0]));
    *right = clamp16((s32)apply_volume(l, g_matrix[0][1]) + apply_volume(r, g_matrix[1][1]));
    return 1;
}

void cd_audio_init(void) {
    spu_psx_set_cd_audio_source(cd_audio_frame);
}

void cd_audio_mix_window(u64 now, u32 frames) {
    /* The mixer's samples sit on a 768-tick grid from power-on (the
     * deterministic mixer starts at tick 0 and carries the remainder from
     * VBlank to VBlank), so the batch ends at a grid point, a fraction of a
     * sample before `now` (an absorbed VBlank's batch is mixed later inside
     * the exception that took it, so `now` is not used after the first). */
    if (!g_have_window) g_window_end = now - now % TICKS_PER_SAMPLE;
    else g_window_end += (u64)frames * TICKS_PER_SAMPLE;
    g_have_window = 1;
}

/* ---- controller state ---- */
static void reset_current_file(void) {
    g_cur_file = g_cur_channel = 0;
    g_cur_set = 0;
}

/* The enhanced backlog's voice capture ends its stream here (read only). */
extern void backlog_xa_reset(void) __attribute__((weak));

void cd_audio_reset_decoder(u64 now) {
    u32 keep;
    if (backlog_xa_reset) backlog_xa_reset();
    lock();
    reset_current_file();
    g_gap_stream = 0;
    cd_xa_decoder_reset(&g_dec);
    /* the frames the SPU took before `now` are gone already.  The
     * real-time mixer plays the FIFO a margin late (xa_margin_frames): the
     * stream's last margin of frames, decoded before `now`, is what the
     * console had already played -- keep it (STATE-MACHINES.md C8: cutting
     * it clipped every voice line's tail at its Pause). */
    if (g_have_window) keep = consumed_by(now);
    else keep = xa_margin_frames() + (u32)FIFO_LOW_WATERMARK;
    if (g_fifo_count > keep) g_fifo_count = keep;
    unlock();
}

/* The real-time mixer, when fast-forward ends (pc/hal/spu.c): the FIFO
 * filled while the game ran ahead; keep its newest `keep` frames, the rest
 * is voice the player would hear late (STATE-MACHINES.md C6).  Under the
 * audio lock. */
void cd_audio_trim(u32 keep) {
    if (g_fifo_count <= keep) return;
    g_fifo_head = (g_fifo_head + (g_fifo_count - keep)) % FIFO_FRAMES;
    g_fifo_count = keep;
}

u32 cd_audio_margin_frames(void) { return xa_margin_frames(); }

void cd_audio_set_filter(u8 file, u8 channel) {
    g_filter_file = file;
    g_filter_channel = channel;
    g_cur_set = 0;
}

void cd_audio_set_mute(int muted) { g_muted = muted != 0; }

void cd_audio_set_volume(const u8 vol[4]) {
    lock();
    g_matrix[0][0] = vol[0];
    g_matrix[0][1] = vol[1];
    g_matrix[1][1] = vol[2];
    g_matrix[1][0] = vol[3];
    unlock();
}

u32 cd_audio_fifo_frames(void) { return g_fifo_count; }
void cd_audio_underruns(u32 *gaps, u32 *frames) { *gaps = g_underruns; *frames = g_underrun_frames; }

void cd_audio_stats(u32 *played, u32 *dropped, u32 *skipped) {
    if (played) *played = g_played;
    if (dropped) *dropped = g_dropped;
    if (skipped) *skipped = g_skipped;
}

/* ---- XA-ADPCM decode (psx-spx) ---- */
static const s32 k_filter_pos[4] = { 0, 60, 115, 98 };
static const s32 k_filter_neg[4] = { 0, 0, -52, -55 };

void cd_xa_decoder_reset(CdXaDecoder *d) {
    memset(d, 0, sizeof *d);
    d->sixstep = 6;
}

/* One sector's 18 sound groups -> samples (stereo interleaved L,R).
 * Returns the number of frames. */
u32 cd_xa_adpcm(CdXaDecoder *d, const u8 *groups, int stereo, int eight_bit, s16 *out) {
    u32 nblocks = eight_bit ? 4u : 8u;
    u32 per_group = 28u * nblocks;
    u32 g, blk, w;
    for (g = 0; g < 18; g++) {
        const u8 *grp = groups + g * 128;
        for (blk = 0; blk < nblocks; blk++) {
            u8 hdr = grp[4 + blk];
            u32 shift = hdr & 0x0F;
            u32 filter = (hdr >> 4) & 0x03;
            s32 *prev = stereo ? &d->last[(blk & 1) * 2] : &d->last[0];
            s16 *dst = stereo ? &out[g * per_group + (blk / 2) * 56 + (blk & 1)]
                              : &out[g * per_group + blk * 28];
            u32 step = stereo ? 2u : 1u;
            if (shift > 12) shift = 9;
            for (w = 0; w < 28; w++) {
                const u8 *word = grp + 16 + w * 4;
                u32 code = eight_bit ? word[blk] : ((word[blk >> 1] >> ((blk & 1) * 4)) & 0x0F);
                s16 sample = (s16)((s16)(u16)(code << (eight_bit ? 8 : 12)) >> shift);
                s32 v = (s32)sample + ((prev[0] * k_filter_pos[filter]) >> 6)
                                    + ((prev[1] * k_filter_neg[filter]) >> 6);
                v = clamp16(v);
                prev[1] = prev[0];
                prev[0] = v;
                *dst = (s16)v;
                dst += step;
            }
        }
    }
    return 18u * per_group / (stereo ? 2u : 1u);
}

/* ---- resampling to 44.1 kHz ---- */
/* psx-spx zigzag tables, 37.8 kHz: output = sum((ring[p - i] * t[i]) >> 15) */
static const s16 k_zigzag[7][29] = {
    { 0, 0x0, 0x0, 0x0, 0x0, -0x0002, 0x000A, -0x0022, 0x0041, -0x0054,
      0x0034, 0x0009, -0x010A, 0x0400, -0x0A78, 0x234C, 0x6794, -0x1780, 0x0BCD, -0x0623,
      0x0350, -0x016D, 0x006B, 0x000A, -0x0010, 0x0011, -0x0008, 0x0003, -0x0001 },
    { 0, 0x0, 0x0, -0x0002, 0x0, 0x0003, -0x0013, 0x003C, -0x004B, 0x00A2,
      -0x00E3, 0x0132, -0x0043, -0x0267, 0x0C9D, 0x74BB, -0x11B4, 0x09B8, -0x05BF, 0x0372,
      -0x01A8, 0x00A6, -0x001B, 0x0005, 0x0006, -0x0008, 0x0003, -0x0001, 0x0 },
    { 0, 0x0, -0x0001, 0x0003, -0x0002, -0x0005, 0x001F, -0x004A, 0x00B3, -0x0192,
      0x02B1, -0x039E, 0x04F8, -0x05A6, 0x7939, -0x05A6, 0x04F8, -0x039E, 0x02B1, -0x0192,
      0x00B3, -0x004A, 0x001F, -0x0005, -0x0002, 0x0003, -0x0001, 0x0, 0x0 },
    { 0, -0x0001, 0x0003, -0x0008, 0x0006, 0x0005, -0x001B, 0x00A6, -0x01A8, 0x0372,
      -0x05BF, 0x09B8, -0x11B4, 0x74BB, 0x0C9D, -0x0267, -0x0043, 0x0132, -0x00E3, 0x00A2,
      -0x004B, 0x003C, -0x0013, 0x0003, 0x0, -0x0002, 0x0, 0x0, 0x0 },
    { -0x0001, 0x0003, -0x0008, 0x0011, -0x0010, 0x000A, 0x006B, -0x016D, 0x0350, -0x0623,
      0x0BCD, -0x1780, 0x6794, 0x234C, -0x0A78, 0x0400, -0x010A, 0x0009, 0x0034, -0x0054,
      0x0041, -0x0022, 0x000A, -0x0001, 0x0, 0x0001, 0x0, 0x0, 0x0 },
    { 0x0002, -0x0008, 0x0010, -0x0023, 0x002B, 0x001A, -0x00EB, 0x027B, -0x0548, 0x0AFA,
      -0x16FA, 0x53E0, 0x3C07, -0x1249, 0x080E, -0x0347, 0x015B, -0x0044, -0x0017, 0x0046,
      -0x0023, 0x0011, -0x0005, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 },
    { -0x0005, 0x0011, -0x0023, 0x0046, -0x0017, -0x0044, 0x015B, -0x0347, 0x080E, -0x1249,
      0x3C07, 0x53E0, -0x16FA, 0x0AFA, -0x0548, 0x027B, -0x00EB, 0x001A, 0x002B, -0x0023,
      0x0010, -0x0008, 0x0002, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }
};

/* 18.9 kHz: 25-tap phases, output = (sum(ring[p - 25 + i] * t[i])) >> 15.
 * Table and loop derived from DuckStation (XAInterpolate18900 /
 * ResampleXAADPCM18900, GPL-3.0, rev ee538ae0e); see the file notice. */
static const s16 k_half[7][25] = {
    { 0x0, -0x5, 0x11, -0x23, 0x46, -0x17, -0x44, 0x15b, -0x347, 0x80e, -0x1249, 0x3c07, 0x53e0,
      -0x16fa, 0xafa, -0x548, 0x27b, -0xeb, 0x1a, 0x2b, -0x23, 0x10, -0x8, 0x2, 0x0 },
    { 0x0, -0x2, 0xa, -0x22, 0x41, -0x54, 0x34, 0x9, -0x10a, 0x400, -0xa78, 0x234c, 0x6794,
      -0x1780, 0xbcd, -0x623, 0x350, -0x16d, 0x6b, 0xa, -0x10, 0x11, -0x8, 0x3, -0x1 },
    { -0x2, 0x0, 0x3, -0x13, 0x3c, -0x4b, 0xa2, -0xe3, 0x132, -0x43, -0x267, 0xc9d, 0x74bb,
      -0x11b4, 0x9b8, -0x5bf, 0x372, -0x1a8, 0xa6, -0x1b, 0x5, 0x6, -0x8, 0x3, -0x1 },
    { -0x1, 0x3, -0x2, -0x5, 0x1f, -0x4a, 0xb3, -0x192, 0x2b1, -0x39e, 0x4f8, -0x5a6, 0x7939,
      -0x5a6, 0x4f8, -0x39e, 0x2b1, -0x192, 0xb3, -0x4a, 0x1f, -0x5, -0x2, 0x3, -0x1 },
    { -0x1, 0x3, -0x8, 0x6, 0x5, -0x1b, 0xa6, -0x1a8, 0x372, -0x5bf, 0x9b8, -0x11b4, 0x74bb,
      0xc9d, -0x267, -0x43, 0x132, -0xe3, 0xa2, -0x4b, 0x3c, -0x13, 0x3, 0x0, -0x2 },
    { -0x1, 0x3, -0x8, 0x11, -0x10, 0xa, 0x6b, -0x16d, 0x350, -0x623, 0xbcd, -0x1780, 0x6794,
      0x234c, -0xa78, 0x400, -0x10a, 0x9, 0x34, -0x54, 0x41, -0x22, 0xa, -0x2, 0x0 },
    { 0x0, 0x2, -0x8, 0x10, -0x23, 0x2b, 0x1a, -0xeb, 0x27b, -0x548, 0xafa, -0x16fa, 0x53e0,
      0x3c07, -0x1249, 0x80e, -0x347, 0x15b, -0x44, -0x17, 0x46, -0x23, 0x11, -0x5, 0x0 }
};

static s16 zigzag(const s16 *ring, u32 table, u32 p) {
    s32 sum = 0;
    u32 i;
    for (i = 0; i < 29; i++)
        sum += ((s32)ring[(p - i) & RING_MASK] * (s32)k_zigzag[table][i]) >> 15;
    return clamp16(sum);
}

static s16 half_rate(const s16 *ring, u32 table, u32 p) {
    s32 sum = 0;
    u32 i;
    for (i = 0; i < 25; i++)
        sum += (s32)ring[(p - 25u + i) & RING_MASK] * (s32)k_half[table][i];
    return clamp16(sum >> 15);
}

void cd_xa_resample(CdXaDecoder *d, const s16 *in, u32 frames, int stereo, int half,
                    CdXaEmit emit, void *ctx) {
    u32 p = d->ring_p, sixstep = d->sixstep, n = 0, j;
    if (!half) {
        for (n = 0; n < frames; n++) {
            d->ring[0][p] = *in++;
            if (stereo) d->ring[1][p] = *in++;
            p = (p + 1) & RING_MASK;
            if (--sixstep == 0) {
                sixstep = 6;
                for (j = 0; j < 7; j++) {
                    s16 l = zigzag(d->ring[0], j, p);
                    emit(ctx, l, stereo ? zigzag(d->ring[1], j, p) : l);
                }
            }
        }
    } else {
        while (n < frames) {
            s16 l;
            if (sixstep >= 7) {
                sixstep -= 7;
                p = (p + 1) & RING_MASK;
                d->ring[0][p] = *in++;
                if (stereo) d->ring[1][p] = *in++;
                n++;
            }
            l = half_rate(d->ring[0], sixstep, p);
            emit(ctx, l, stereo ? half_rate(d->ring[1], sixstep, p) : l);
            sixstep += 3;
        }
    }
    d->ring_p = p;
    d->sixstep = sixstep;
}

static void emit_fifo(void *ctx, s16 l, s16 r) {
    (void)ctx;
    fifo_push(l, r);
}

/* The first checks of cd_audio_xa_sector, without their effects. */
int cd_audio_accepts(const u8 *raw, int filter) {
    u8 file = raw[0], channel = raw[1];
    if (filter && (file != g_filter_file || channel != g_filter_channel)) return 0;
    if (!g_cur_set) return !(channel == 255 && (!filter || g_filter_channel != 255));
    return file == g_cur_file && channel == g_cur_channel;
}

void cd_audio_xa_sector(const u8 *raw, int filter, u64 now) {
    static s16 samples[18 * 224];
    u8 file = raw[0], channel = raw[1], submode = raw[2], coding = raw[3];
    int stereo = (coding & CODING_STEREO) != 0;
    int eight_bit = (coding & CODING_8BIT) != 0;
    int half = (coding & CODING_HALF_RATE) != 0;
    u32 frames, consumed;
    s32 queued, pad;

    if (filter && (file != g_filter_file || channel != g_filter_channel)) { g_skipped++; return; }
    if (!g_cur_set) {
        /* junk audio sectors carry channel 255 (only a filter selects them) */
        if (channel == 255 && (!filter || g_filter_channel != 255)) { g_skipped++; return; }
        g_cur_file = file;
        g_cur_channel = channel;
        g_cur_set = 1;
    } else if (file != g_cur_file || channel != g_cur_channel) {
        g_skipped++;
        return;
    }
    if (submode & SUBMODE_EOF) reset_current_file();

    lock();
    consumed = consumed_by(now);
    queued = (s32)g_fifo_count - (s32)consumed;
    if (g_have_window ? queued > (s32)FIFO_LOW_WATERMARK : g_fifo_count > RELAXED_FIFO_LIMIT) {
        g_dropped++;
        if (xa_debug()) fprintf(stderr, "[CD/XA t%llu] sector %u/%u dropped: %d frames still queued\n",
                                (unsigned long long)now, file, channel, (int)queued);
        unlock();
        return;
    }
    frames = cd_xa_adpcm(&g_dec, raw + 8, stereo, eight_bit, samples);
    if (g_muted) { unlock(); return; }
    /* the SPU ran dry before this sector: that stretch was silence */
    pad = queued < 0 ? -queued : 0;
    while (queued < 0) { fifo_push(0, 0); queued++; }
    if (!g_fifo_count && g_dry_frames > 0 && g_dry_frames < 13230u && g_gap_stream &&
        file == g_gap_file && channel == g_gap_channel) {
        g_underruns++;
        g_underrun_frames += g_dry_frames;
        if (xa_debug()) fprintf(stderr, "[CD/XA t%llu] gap: the FIFO ran dry %u frames before this sector\n",
                                (unsigned long long)now, g_dry_frames);
    }
    cd_xa_resample(&g_dec, samples, frames, stereo, half, emit_fifo, NULL);
    g_played++;
    g_gap_stream = 1;
    g_gap_file = file;
    g_gap_channel = channel;
    if (xa_debug())
        fprintf(stderr, "[CD/XA t%llu] sector %u/%u submode 0x%02X coding 0x%02X: %u frames, pad %d, fifo %u\n",
                (unsigned long long)now, file, channel, submode, coding, frames,
                (int)pad, g_fifo_count);
    unlock();
}
