/* SPDX-License-Identifier: GPL-3.0-or-later */
/* voice_replay.c -- replays a backlog page's voice line (pc/include/pc_backlog.h).
 *
 * The line is read from the disc asset the drive read it from
 * (pc/assets/raw/CDROM/XA_G/<GIRL>.XA, raw 2,336-byte Mode 2 sectors), over
 * the window the game asked for (set_XAsector -> SD_XACmd; the sectors the
 * drive actually read when that is unknown), taking the sectors of the
 * line's file/channel the way the controller's filter does, until the
 * window ends or a sector carries EOF.  They are decoded by a private
 * instance of the controller's own XA-ADPCM decoder and 44.1 kHz
 * interpolator (pc/hal/cd_audio.c cd_xa_adpcm / cd_xa_resample, from a
 * decoder reset as at the start of every read), so the replay is the
 * PCM the console's CD input carried for that line -- not resampled or
 * filtered differently -- and test_backlog.py checks it is.
 *
 * Playback is PC output only: pc/hal/spu.c adds it to the mixed frames it
 * hands the audio device, after the game's mix, at the player's voice
 * volume and the SPU's CD and main volume.  The drive, the controller's
 * FIFO, the SPU core and its capture buffers (the lip-sync level reads)
 * never see it, so the game cannot notice a replay.  While the game is
 * held (the backlog open) spu.c runs the device for the replay alone.
 */
#include "types.h"
#include "pc_data_root.h"
#include "pc_backlog.h"
#include "cd_audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FRAMES (44100 * 60)     /* a minute: longer than any line */
#define SUBMODE_EOF 0x80

extern void spu_hal_audio_lock(void) __attribute__((weak));
extern void spu_hal_audio_unlock(void) __attribute__((weak));
extern void spu_hal_side_audio(int on) __attribute__((weak));

typedef struct {
    s16 *out;
    int n, cap;
} Sink;

static void emit(void *ctx, s16 l, s16 r)
{
    Sink *s = (Sink *)ctx;
    if (s->n >= s->cap) return;
    s->out[s->n * 2] = l;
    s->out[s->n * 2 + 1] = r;
    s->n++;
}

int voice_replay_decode(const BacklogVoice *v, const char *asset_root, s16 *out, int cap)
{
    static s16 samples[18 * 224];
    char path[600];
    u8 raw[CD_AUDIO_RAW_SECTOR];
    CdXaDecoder dec;
    Sink sink;
    FILE *f;
    u32 lba, first, last;
    if (!v || !v->path[0]) return 0;
    if (asset_root) snprintf(path, sizeof path, "%s/%s", asset_root, v->path);
    else snprintf(path, sizeof path, "%s/raw/%s", pc_asset_dir(), v->path);
    f = fopen(path, "rb");
    if (!f) return 0;
    first = v->req_last ? v->req_first : v->lba_first;
    last = v->req_last ? v->req_last : v->lba_last;
    if (first < v->file_lba) first = v->file_lba;
    sink.out = out;
    sink.n = 0;
    sink.cap = cap;
    cd_xa_decoder_reset(&dec);
    for (lba = first; lba <= last && sink.n < cap; lba++) {
        u8 coding;
        u32 frames;
        if (fseek(f, (long)(lba - v->file_lba) * CD_AUDIO_RAW_SECTOR, SEEK_SET) ||
            fread(raw, 1, sizeof raw, f) != sizeof raw)
            break;
        /* a realtime audio sector of the line's file and channel */
        if ((raw[2] & 0x44) != 0x44 || raw[0] != v->file || raw[1] != v->channel) continue;
        coding = raw[3];
        frames = cd_xa_adpcm(&dec, raw + 8, coding & 0x01, (coding & 0x10) != 0, samples);
        cd_xa_resample(&dec, samples, frames, coding & 0x01, (coding & 0x04) != 0, emit, &sink);
        if (raw[2] & SUBMODE_EOF) break;
    }
    fclose(f);
    return sink.n;
}

/* ---- playback: written by the game / pause-loop thread under the audio
 * lock, read by the mixer (the SDL callback, or spu_parity_mix) ---- */
static s16 *g_pcm;
static int g_frames, g_pos;
static volatile int g_active;

static void lock(void)   { if (spu_hal_audio_lock) spu_hal_audio_lock(); }
static void unlock(void) { if (spu_hal_audio_unlock) spu_hal_audio_unlock(); }

int voice_replay_start(const BacklogVoice *v, const char *asset_root)
{
    s16 *pcm = (s16 *)malloc((size_t)MAX_FRAMES * 2 * sizeof(s16));
    s16 *old;
    int n;
    if (!pcm) return 0;
    n = voice_replay_decode(v, asset_root, pcm, MAX_FRAMES);
    if (n <= 0) {
        free(pcm);
        return 0;
    }
    lock();
    old = g_pcm;
    g_pcm = pcm;
    g_frames = n;
    g_pos = 0;
    g_active = 1;
    unlock();
    free(old);
    if (spu_hal_side_audio) spu_hal_side_audio(1);
    printf("[BACKLOG] voice replay: %s lba %u..%u file %u channel %u, %d frames\n", v->path,
           v->req_last ? v->req_first : v->lba_first, v->req_last ? v->req_last : v->lba_last,
           v->file, v->channel, n);
    fflush(stdout);
    return n;
}

void voice_replay_stop(void)
{
    s16 *old;
    if (!g_active && !g_pcm) return;
    lock();
    old = g_pcm;
    g_pcm = NULL;
    g_frames = g_pos = 0;
    g_active = 0;
    unlock();
    free(old);
}

int voice_replay_active(void) { return g_active; }

static s16 sat(s32 v) { return (s16)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

/* In the mixer, under the audio lock. */
void voice_replay_mix(s16 *out, int frames, float gain)
{
    int i, n;
    if (!g_active || !g_pcm) return;
    n = g_frames - g_pos;
    if (n > frames) n = frames;
    for (i = 0; i < n; i++) {
        const s16 *s = &g_pcm[(g_pos + i) * 2];
        out[i * 2] = sat(out[i * 2] + (s32)((float)s[0] * gain));
        out[i * 2 + 1] = sat(out[i * 2 + 1] + (s32)((float)s[1] * gain));
    }
    g_pos += n;
    if (g_pos >= g_frames) g_active = 0;
}
