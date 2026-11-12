/* SPDX-License-Identifier: GPL-3.0-or-later */
/* spu.c -- Sound HAL driving the PS1 SPU core.
 *
 * Game audio (the default path since 2026-10-30): the game's own libsnd --
 * the matched C for SsSeqOpen/SsSeqPlay/SsSeqSetVol/SsSeqStop, the
 * sequencer behind SsSeqCalledTbyT and the SpuVm voice manager -- plays
 * the SEQ/VAB bytes the game loaded.  This file provides the device:
 *
 *   VSync callback (installed by _SsStart below)
 *     -> spu_mmio_sync / SsSeqCalledTbyT / spu_mmio_sync
 *        libsnd's register writes to the mapped 0x1F801C00 block reach
 *        the core; hardware-owned registers are read back from it
 *   VBlank (pc/hal/system.c)
 *     -> spu_mmio_sync, then the VBlank's samples are mixed
 *   audio callback / spu_parity_mix
 *     -> spu_psx_mix(stream, num_samples)      (pc/hal/spu_psx.c)
 *        ADPCM + ADSR + reverb via the SPU core
 *        the core takes the CD-ROM controller's XA audio as its CD input
 *        (pc/hal/cd_audio.c, fed by the drive model in pc/hal/cd.c) and
 *        writes it to the capture buffers SD_CDLevelRead reads
 *     -> audio_xa_tick(stream, num_samples)    (this file)
 *        the PC-only XA file player (sound test, PC_SCENE_MUSIC=1) on top
 *   SpuWrite / SpuIsTransferCompleted / SpuClearReverbWorkArea
 *     -> pc/hal/spu_transfer.c timed DMA into SPU RAM
 *
 * PC-only player (sound test, PC_SCENE_MUSIC=1): seq_spu_advance()
 * (pc/hal/seq_spu.c) emits voice writes from a SEQ file it parses itself,
 * with VAB files uploaded at SPU RAM 0x1010 by vab_loader.c.  It is not a
 * retail path and is not driven by game requests.
 *
 * External API:
 *   spu_hal_init() / spu_hal_shutdown()
 *   audio_seq_load_and_play() / audio_seq_stop() / audio_preview_vab()
 *   audio_xa_start() / audio_xa_stop()
 *   SpuInit / SpuQuit / SpuWrite / SpuSetTransfer* / SpuIsTransferCompleted
 *   _SsStart / SsStart / SsStart2 / SsSetMVol / SsVabOpenHead
 *   spu_mmio_sync()
 */
#include "hal.h"
#include "pc_data_root.h"
#include "cpu_cost.h"
#include "parity.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

/* Uncomment -DUSE_TSF_FALLBACK on the compiler command line to keep the
 * legacy TinySoundFont renderer as a drop-in. The new SPU core
 * path is the default. Keep TSF scaffolding so we can A/B if needed. */
#ifdef USE_TSF_FALLBACK
#  define TSF_IMPLEMENTATION
#  include "tsf.h"
#endif

#include "spu_psx.h"
#include "spu_transfer.h"
#include "spu_reset.h"
#include "seq_spu.h"
#include "vab_loader.h"
#include "cd_audio.h"
#include "audio_resample.h"

#define SAMPLE_RATE     44100
#define AUDIO_CHANNELS  2
#define AUDIO_SAMPLES   1024
#define VAB_SPU_BASE    0x1010u  /* Avoid reserved low region used by XA/CD. */

/* --------------------------------------------------------------------- */
/* Audio subsystem                                                       */
/* --------------------------------------------------------------------- */

static SDL_AudioDeviceID g_audio_dev = 0;
static int g_audio_initialized = 0;

/* Host gain applied to the SPU output: 1.0 (none), as on the console --
 * the game sets the SPU main volume itself through SsSetMVol. */
static float g_master_vol = 1.0f;

/* The player's volumes (settings [audio], spu_hal_apply_volumes), split
 * over the two sources the mixer can tell apart:
 *   g_gain_spu  the SPU's 24 voices: sequenced music AND sound effects
 *               (master x music x sfx -- one bus, they cannot be split);
 *   g_gain_cd   the CD input: XA voices, streamed music, movie sound
 *               (master x voice).
 * 1.0 / 1.0 (the defaults, and always in automated runs) leaves every
 * sample untouched. */
static float g_gain_spu = 1.0f, g_gain_cd = 1.0f;
/* While a music pack replaces the song (pc/hal/music_pack.c), the SPU's
 * output carries the sound effects only, so the bus takes master x sfx; the
 * replacement takes master x music itself. */
static float g_gain_sfx = 1.0f;
/* master x music: a SoundFont's song (pc/hal/soundfont.c) is mixed inside
 * the SPU output, which then carries master x sfx; its frames are scaled by
 * the ratio */
static float g_gain_music = 1.0f;
void spu_hal_apply_volumes(void);

/* pc/hal/music_pack.c; weak, so the SPU harnesses that link this file alone
 * keep the plain mixer. */
extern int  music_pack_mixing(void) __attribute__((weak));
extern int  music_pack_replacing(void) __attribute__((weak));
extern void music_pack_mix(s16 *out, const s16 *music, int frames) __attribute__((weak));
extern void music_pack_tick(void) __attribute__((weak));
extern void music_pack_voice_sync(void) __attribute__((weak));
/* The enhanced backlog's voice replay (pc/hal/voice_replay.c); weak:
 * harnesses link this file without it.
 * g_held: the game thread is held in the presenter's pause loop
 * (spu_hal_set_paused); the device then runs, if at all, for the replay
 * alone and the game's mix is not touched. */
extern void voice_replay_mix(s16 *out, int frames, float gain) __attribute__((weak));
extern int voice_replay_active(void) __attribute__((weak));
extern int backlog_enabled(void) __attribute__((weak));
static volatile int g_held;
/* The level the game plays its CD input at: SPU CD volume x current main
 * volume, as the core applies them.  Sampled (pure register reads) after
 * every mix while the backlog is on -- never under parity -- and only
 * while the game has the CD input enabled (it turns it on for its voice
 * lines), so a replay plays at the level of the game's last line. */
static float g_cd_level = 0.5f;
static void sample_cd_level(void) {
    s32 cdl = (s16)spu_psx_read_reg(0x1F801DB0), cdr = (s16)spu_psx_read_reg(0x1F801DB2);
    s32 mvl = (s16)spu_psx_read_reg(0x1F801DB8), mvr = (s16)spu_psx_read_reg(0x1F801DBA);
    float cd, mv;
    if (!(spu_psx_read_reg(0x1F801DAA) & 1)) return;
    cd = (float)((cdl < 0 ? -cdl : cdl) + (cdr < 0 ? -cdr : cdr)) / 65536.0f;
    mv = (float)((mvl < 0 ? -mvl : mvl) + (mvr < 0 ? -mvr : mvr)) / 65536.0f;
    if (cd > 0.0f && mv > 0.0f) g_cd_level = cd * mv;
}
static void replay_mix(s16 *out, int frames) {
    if (!voice_replay_mix || !voice_replay_active || !voice_replay_active()) return;
    voice_replay_mix(out, frames, g_cd_level * g_gain_cd * g_master_vol);
}

#ifdef USE_TSF_FALLBACK
static tsf *g_tsf = NULL;
static char g_current_sf2[256] = {0};
#endif

/* Current SEQ driver state + VAB bank. Ownership is transferred between
 * the audio-callback thread and the game thread through SDL audio-device
 * locking; both pointers are only updated while the lock is held. */
static SeqSpuState *g_seq = NULL;
/* SPU register bridge state (see spu_mmio_sync). */
#define SPU_MMIO_REGS_EARLY 0x100
#define SPU_MMIO_BASE_ADDR 0x1F801C00u
static u16 g_mmio_last[SPU_MMIO_REGS_EARLY];
static int g_mmio_live;
static void spu_mmio_start(void);
static VabBank     *g_bank = NULL;

/* Volume applied to the SEQ output (0..127). Propagates on next
 * audio_seq_load_and_play / SsSetMVol. */
static u8 g_seq_volume = 112;

/* WAV dump for headless verification. Always on in dev builds. */
static FILE *g_wav_file = NULL;
static u32   g_wav_samples = 0;
#define WAV_DUMP_SECONDS 25

/* --------------------------------------------------------------------- */
/* Helpers                                                               */
/* --------------------------------------------------------------------- */

/* Replace ".SEQ" (case-insensitive) with the supplied extension.
 * Writes a full new path into out. */
static void path_with_ext(const char *seq_path, const char *ext,
                          char *out, size_t cap) {
    size_t len = strlen(seq_path);
    size_t keep = len;
    if (len >= 4) {
        const char *tail = seq_path + len - 4;
        if (tail[0] == '.' &&
            toupper((unsigned char)tail[1]) == 'S' &&
            toupper((unsigned char)tail[2]) == 'E' &&
            toupper((unsigned char)tail[3]) == 'Q') {
            keep = len - 4;
        }
    }
    snprintf(out, cap, "%.*s%s", (int)keep, seq_path, ext);
}

/* Read a whole file into a malloc'd buffer. Caller frees. */
static u8 *read_file(const char *path, u32 *out_size) {
    FILE *f;
    long sz;
    u8 *buf;
    f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    buf = (u8 *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    if (out_size) *out_size = (u32)sz;
    return buf;
}

/* --------------------------------------------------------------------- */
/* XA ADPCM streaming (unchanged from TSF path)                          */
/* --------------------------------------------------------------------- */

#include "cdxa_decode.h"

#define XA_RING_FRAMES  16384
#define XA_RING_MASK    (XA_RING_FRAMES - 1)
#define XA_DECODE_BURST 2048

static CdxaStream *g_xa_stream = NULL;
static s16   g_xa_ring[XA_RING_FRAMES * 2];
static u32   g_xa_ring_head = 0;
static u32   g_xa_ring_tail = 0;
static int   g_xa_loop = 1;
static int   g_xa_active = 0;

static void xa_refill(void) {
    s16 scratch[XA_DECODE_BURST * 2];
    u32 head, tail, free_frames;
    int got_samples, got_frames, i;

    if (!g_xa_stream || !g_xa_active) return;

    head = g_xa_ring_head;
    tail = g_xa_ring_tail;
    free_frames = XA_RING_FRAMES - ((head - tail) & XA_RING_MASK) - 1;
    if (free_frames < XA_DECODE_BURST) return;

    got_samples = cdxa_decode(g_xa_stream, scratch, XA_DECODE_BURST * 2);
    if (got_samples <= 0) {
        if (g_xa_loop) {
            cdxa_rewind(g_xa_stream);
            got_samples = cdxa_decode(g_xa_stream, scratch, XA_DECODE_BURST * 2);
        }
        if (got_samples <= 0) { g_xa_active = 0; return; }
    }
    got_frames = got_samples / 2;
    for (i = 0; i < got_frames; i++) {
        u32 pos = (head + (u32)i) & XA_RING_MASK;
        g_xa_ring[pos * 2 + 0] = scratch[i * 2 + 0];
        g_xa_ring[pos * 2 + 1] = scratch[i * 2 + 1];
    }
    g_xa_ring_head = (head + (u32)got_frames) & XA_RING_MASK;
}

static s16 xa_sat_add(int a, int b) {
    int s = a + b;
    if (s < -32768) return -32768;
    if (s >  32767) return  32767;
    return (s16)s;
}

static void xa_mix_into(s16 *stream, int frames) {
    int i;
    int scale = g_gain_cd < 0.999f;
    u32 head = g_xa_ring_head;
    u32 tail = g_xa_ring_tail;
    u32 available = (head - tail) & XA_RING_MASK;
    int mix_frames = (int)available;
    if (mix_frames > frames) mix_frames = frames;
    for (i = 0; i < mix_frames; i++) {
        u32 pos = (tail + (u32)i) & XA_RING_MASK;
        int xl = g_xa_ring[pos * 2 + 0];
        int xr = g_xa_ring[pos * 2 + 1];
        if (scale) { xl = (int)(xl * g_gain_cd); xr = (int)(xr * g_gain_cd); }
        stream[i * 2 + 0] = xa_sat_add(stream[i * 2 + 0], xl);
        stream[i * 2 + 1] = xa_sat_add(stream[i * 2 + 1], xr);
    }
    g_xa_ring_tail = (tail + (u32)mix_frames) & XA_RING_MASK;
}

void audio_xa_tick(s16 *stream, int frames) {
    xa_refill();
    xa_mix_into(stream, frames);
}

/* --------------------------------------------------------------------- */
/* SDL audio callback                                                    */
/* --------------------------------------------------------------------- */

/* Smooth slice-boundary discontinuities.
 *
 * the DuckStation SPU core latches KEY_ON / KEY_OFF / volume writes at the start
 * of each GenerateSamples batch. Splitting the callback into SPU_SLICE_FRAMES
 * sub-batches buys sub-millisecond event granularity but also turns every
 * slice boundary into a potential amplitude step: a voice's adsr_volume,
 * volume_left/right register, or pitch can change between slice N's last
 * sample and slice N+1's first sample, producing an audible click.
 *
 * Rather than back-port DuckStation's internal smoothing, post-process the
 * output: at each slice boundary, if the s16 amplitude jump exceeds the
 * threshold, blend the 3 samples before and 3 after with a linear ramp
 * anchored at (frame[-4], frame[+3]). 6-sample window = ~0.14ms at 44100
 * Hz, below the ear's time resolution for individual events. */
static void deglitch_slice_boundaries(s16 *stream, int frames, int slice) {
    const int RADIUS = 3;
    const int THRESH = 800;  /* below most note transients, above noise */
    int b;
    for (b = slice; b < frames; b += slice) {
        int ch;
        for (ch = 0; ch < AUDIO_CHANNELS; ch++) {
            int idx_prev = (b - 1) * AUDIO_CHANNELS + ch;
            int idx_curr =  b      * AUDIO_CHANNELS + ch;
            int jump = stream[idx_curr] - stream[idx_prev];
            int abs_j = jump < 0 ? -jump : jump;
            if (abs_j < THRESH) continue;
            {
                int lo_idx = b - RADIUS - 1;
                int hi_idx = b + RADIUS;
                if (lo_idx < 0 || hi_idx >= frames) continue;
                {
                    int lo = stream[lo_idx * AUDIO_CHANNELS + ch];
                    int hi = stream[hi_idx * AUDIO_CHANNELS + ch];
                    int span = hi_idx - lo_idx;  /* = 2*RADIUS + 1 */
                    int k;
                    for (k = 1; k < span; k++) {
                        int interp = lo + ((hi - lo) * k) / span;
                        if (interp < -32768) interp = -32768;
                        if (interp >  32767) interp =  32767;
                        stream[(lo_idx + k) * AUDIO_CHANNELS + ch] = (s16)interp;
                    }
                }
            }
        }
    }
}

/* Apply float master gain to s16 stereo stream. Skipped when master_vol
 * is >= 1.0 to avoid needless work. */
static void apply_master_gain(s16 *stream, int frames, float gain) {
    int i, n = frames * 2;
    if (gain >= 0.999f) return;
    for (i = 0; i < n; i++) {
        int v = (int)((float)stream[i] * gain);
        if (v < -32768) v = -32768;
        if (v >  32767) v =  32767;
        stream[i] = (s16)v;
    }
}

/* Re-weight one mixed slice: out = spu * g_gain_spu + cd * g_gain_cd, where
 * `cd` (the tapped CD input frames) is the CD's share of the core's output.
 * The core adds the CD input at the SPU's CD volume (0x1F801DB0/2, SPUCNT
 * bit 0) before the main volume (current level 0x1F801DB8/BA) -- the same
 * ApplyVolume steps are repeated here.  Approximate where the sum clipped
 * or the CD feeds reverb (CD reverb is not re-weighted). */
static s16 clamp_s16(float v) {
    return (s16)(v < -32768.0f ? -32768 : v > 32767.0f ? 32767 : (int)v);
}
static void split_cd_gain(s16 *out, const s16 *cd, int frames, float gain_spu) {
    s32 cdl = (s16)spu_psx_read_reg(0x1F801DB0), cdr = (s16)spu_psx_read_reg(0x1F801DB2);
    s32 mvl = (s16)spu_psx_read_reg(0x1F801DB8), mvr = (s16)spu_psx_read_reg(0x1F801DBA);
    int on = spu_psx_read_reg(0x1F801DAA) & 1, i;
    for (i = 0; i < frames; i++) {
        s32 cl = on ? ((((s32)cd[i * 2] * cdl) >> 15) * mvl) >> 15 : 0;
        s32 cr = on ? ((((s32)cd[i * 2 + 1] * cdr) >> 15) * mvr) >> 15 : 0;
        out[i * 2]     = clamp_s16((out[i * 2] - cl) * gain_spu + cl * g_gain_cd);
        out[i * 2 + 1] = clamp_s16((out[i * 2 + 1] - cr) * gain_spu + cr * g_gain_cd);
    }
}

/* PC_AUDIO_HASH=PATH (testing): one line per mixer call, "N FRAMES HASH",
 * FNV-1a over the game's mixed samples -- before the backlog's voice
 * replay and the fast-forward mute -- so two runs can be compared sample
 * for sample without storing the audio.  With the deterministic mixer
 * (PC_PARITY_DIR) a call is a VBlank's samples.  Observation only. */
static unsigned long calls;
/* lines written so far (pc/hal/rewind.c's log, so a test can find the
 * lines of the frames it rewinds over) */
unsigned long spu_audio_hash_calls(void) { return calls; }

/* ---- Files the mixer feeds: written by the game thread ----------------
 *
 * PC_AUDIO_HASH, PC_AUDIO_DUMP and the developer WAV dump record what the
 * mixer produced.  The real-time mixer runs on the audio device's thread,
 * where a file write can block for as long as the disk (or, on Windows, a
 * virus scanner or a synced folder) takes -- and the device starves.  So
 * the device thread only copies into a ring (TapRing; under the device
 * lock, which SDL holds around the callback) and the game thread writes
 * the files (spu_tap_drain, every spu_mmio_sync and at exit).  The
 * deterministic mixer runs on the game thread and writes directly.  The
 * files are opened by spu_hal_init (audio_files_open), never by the
 * callback. */
typedef struct { u8 *buf; u32 cap, head, count, lost; } TapRing;
static TapRing g_tap_out, g_tap_hash;   /* raw output; hash lines */
static FILE *g_hash_log, *g_raw_dump;

static void tap_put(TapRing *r, const void *src, u32 n) {
    const u8 *p = (const u8 *)src;
    u32 i;
    if (!r->buf) return;
    if (n > r->cap - r->count) { r->lost += n; return; }
    for (i = 0; i < n; i++) r->buf[(r->head + r->count + i) % r->cap] = p[i];
    r->count += n;
}

static u32 tap_take(TapRing *r, u8 *dst, u32 max) {
    u32 n = r->count < max ? r->count : max, i;
    for (i = 0; i < n; i++) dst[i] = r->buf[(r->head + i) % r->cap];
    r->head = (r->head + n) % r->cap;
    r->count -= n;
    return n;
}

static void tap_alloc(TapRing *r, u32 cap) {
    if (!r->buf && (r->buf = (u8 *)malloc(cap)) != NULL) r->cap = cap;
}

static void audio_hash_record(const s16 *out, int frames, int realtime) {
    const unsigned char *b = (const unsigned char *)out;
    u32 h = 2166136261u;
    size_t i, n = (size_t)frames * AUDIO_CHANNELS * sizeof(s16);
    char line[64];
    int k;
    if (!g_hash_log) return;
    for (i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    k = snprintf(line, sizeof line, "%lu %d %08x\n", calls++, frames, (unsigned)h);
    if (k <= 0) return;
    if (realtime) {
        tap_put(&g_tap_hash, line, (u32)k);
    } else {
        fwrite(line, 1, (size_t)k, g_hash_log);
        fflush(g_hash_log);
    }
}

static void wav_dump_write(const u8 *data, u32 len);

/* the device output (after the fast-forward mute): PC_AUDIO_DUMP and the
 * developer WAV dump */
static void audio_out_record(const u8 *stream, u32 len, int realtime) {
    if (!g_raw_dump && !g_wav_file) return;
    if (realtime) {
        tap_put(&g_tap_out, stream, len);
        return;
    }
    if (g_raw_dump) fwrite(stream, 1, len, g_raw_dump);
    wav_dump_write(stream, len);
}

/* Game thread: write what the device thread queued. */
static void spu_tap_drain(void) {
    static u8 chunk[65536];
    u32 n;
    if (!g_tap_out.buf && !g_tap_hash.buf) return;
    for (;;) {
        if (g_audio_dev) SDL_LockAudioDevice(g_audio_dev);
        n = g_tap_out.buf ? tap_take(&g_tap_out, chunk, sizeof chunk) : 0;
        if (g_audio_dev) SDL_UnlockAudioDevice(g_audio_dev);
        if (!n) break;
        if (g_raw_dump) fwrite(chunk, 1, n, g_raw_dump);
        wav_dump_write(chunk, n);
    }
    for (;;) {
        if (g_audio_dev) SDL_LockAudioDevice(g_audio_dev);
        n = g_tap_hash.buf ? tap_take(&g_tap_hash, chunk, sizeof chunk) : 0;
        if (g_audio_dev) SDL_UnlockAudioDevice(g_audio_dev);
        if (!n) break;
        fwrite(chunk, 1, n, g_hash_log);
        fflush(g_hash_log);
    }
    if (g_raw_dump) fflush(g_raw_dump);
}

/* Game thread, from spu_hal_init: open the files the environment asks for
 * (the callback never opens a file). */
static void audio_files_open(void) {
    const char *p;
    static int done;
    if (done) return;
    done = 1;
    if ((p = getenv("PC_AUDIO_HASH")) && *p) {
        g_hash_log = fopen(p, "w");
        if (g_hash_log) tap_alloc(&g_tap_hash, 1u << 18);
    }
    if ((p = getenv("PC_AUDIO_DUMP")) && *p) g_raw_dump = fopen(p, "wb");
    if (g_raw_dump || g_wav_file) tap_alloc(&g_tap_out, SAMPLE_RATE * AUDIO_CHANNELS * 2u * 4u);
}

/* Frames the real-time device has pulled (diagnostics, PC_DEBUG=sound). */
static u64 g_cb_frames;

/* ---- Audio health (the player log) ------------------------------------
 *
 * Counters the player log reports (audio_health_report, game thread, at
 * most one line per 10 s and only when something new happened), so a
 * report of "skipping" audio comes with the evidence:
 *   device thread: callbacks, late callbacks (the gap since the previous
 *     one more than 1.5 periods + 5 ms: the device may have starved), the
 *     longest gap, the longest time spent mixing, output samples at the
 *     rail (clipping);
 *   game thread: key-on / key-off bits the SPU core had not yet applied
 *     when the next KON/KOFF write replaced them (spu_mmio_sync).
 * The device thread only stores numbers: no file I/O, no printf, no
 * allocation.  The deterministic mixer (spu_parity_mix) is not counted. */
static int g_realtime_tag;              /* the SDL device's userdata */
static struct {
    volatile u32 callbacks, late, max_gap_us, max_mix_us, clipped;
    volatile u32 period_us;             /* a callback's frames at 44.1 kHz */
    volatile u32 min_frames, max_frames;  /* the sizes the device asked for */
    u64 last_pc;
    u32 kon_writes, kon_lost, koff_lost, kon_then_koff;
} g_hl;

static void audio_health_report(int final);
/* the run's totals, once: at exit, or from the trace harness's _exit path
 * (pc/hal/trace.c), which skips atexit */
void spu_audio_health_final(void) {
    static int done;
    if (done) return;
    done = 1;
    spu_tap_drain();
    if (g_raw_dump) fflush(g_raw_dump);
    audio_health_report(1);
}
static void audio_health_final(void) { spu_audio_health_final(); }

static void health_callback_begin(int frames, u64 now) {
    u64 freq = SDL_GetPerformanceFrequency();
    u32 period = (u32)((u64)frames * 1000000u / SAMPLE_RATE);
    g_hl.period_us = period;
    if (!g_hl.min_frames || (u32)frames < g_hl.min_frames) g_hl.min_frames = (u32)frames;
    if ((u32)frames > g_hl.max_frames) g_hl.max_frames = (u32)frames;
    if (g_hl.last_pc && freq) {
        u64 gap = (now - g_hl.last_pc) * 1000000u / freq;
        if (gap > g_hl.max_gap_us) g_hl.max_gap_us = (u32)gap;
        if (gap > (u64)period * 3 / 2 + 5000u) g_hl.late++;
    }
    g_hl.last_pc = now;
    g_hl.callbacks++;
}

static void health_callback_end(const s16 *out, int frames, u64 start) {
    u64 freq = SDL_GetPerformanceFrequency();
    int i, n = frames * AUDIO_CHANNELS;
    u32 clip = 0;
    if (freq) {
        u64 us = (SDL_GetPerformanceCounter() - start) * 1000000u / freq;
        if (us > g_hl.max_mix_us) g_hl.max_mix_us = (u32)us;
    }
    for (i = 0; i < n; i++)
        if (out[i] >= 32767 || out[i] <= -32768) clip++;
    g_hl.clipped += clip;
}

/* ---- The real-time register queue -------------------------------------
 *
 * The SPU core latches KON/KOFF and applies them at the start of its next
 * sample batch (spu.cpp, the voice loop's i == 0); a second write to the
 * same register before that batch REPLACES the first.  The deterministic
 * mixer (spu_parity_mix) mixes each VBlank right after the VBlank's
 * register writes, so that never happens there.  The real-time device
 * mixes 1,024 frames (23.2 ms) per callback on its own thread while the
 * game thread forwards a VBlank's writes every 16.7 ms -- in bursts when
 * the host is late -- so two VBlanks' writes regularly met between two
 * callbacks and the first one's notes were never keyed (measured,
 * 2026-11-02, title to name entry in real time: 27 of 159 key-on writes
 * lost voices in enhanced, 127 of 420 in parity; key-offs 19 and 253,
 * which leave a note sounding until its voice is reused).  Heard as notes
 * and drum hits missing from the music, "as if a channel were missing".
 *
 * So in real time the voice registers (0x000-0x17F) and the key / mode
 * registers (KON, KOFF, PMON, NON, EON: 0x188-0x19A) are not written to
 * the core by spu_mmio_sync; they are queued with the emulated tick of the
 * sync.  The device thread replays them on the emulated time line: it keeps
 * `g_rt.play`, the emulated tick of the next sample it mixes, a fixed
 * latency (TOKI_SPU_LATENCY_MS, default 50) behind the game's clock, and
 * splits its batch at each queued write's tick, so every write lands at
 * its own sample position and a KON never meets an unapplied KON.  A write
 * that arrives after its tick (the host stalled longer than the latency)
 * steps the cursor back (see rt_apply_due), never dropped.  The global
 * registers (main and CD volume, SPUCNT, reverb, IRQ, transfer) stay
 * immediate: the game reads some of them back and none is edge-triggered.
 * TOKI_SPU_LATENCY_MS=0 is the previous behaviour (direct writes; the
 * negative control), and so are PC_TURBO runs and the deterministic mixer. */
#define RTQ_CAP 16384u
#define TICKS_PER_SAMPLE_RT 768u        /* 33,868,800 / 44,100 */
#define SPU_SLICE_LATE 64u              /* frames: a write later than this is "late" */
#define RT_EXTRA_MAX_MS 150u            /* the adaptive latency's ceiling */
#define RT_LATE_JITTER_MS 60u           /* later than this is a stall: not adapted to */
/* tag: a KON's SoundFont snapshot (pc/hal/soundfont.c), applied with it */
typedef struct { u64 tick; u16 off, value; u32 tag; } RtWrite;
static struct {
    RtWrite q[RTQ_CAP];
    u32 head, count;                /* under the device lock */
    u64 pub_tick, pub_pc;           /* the game's clock at its last sync */
    int pub_valid;
    u64 play;                       /* emulated tick of the next sample mixed */
    int play_valid;
    u64 extra;                      /* adaptive latency on top (ticks) */
    u64 max_extra;
    u64 resync_floor;               /* writes before the last resync's cursor */
    u64 last_tick;                  /* the previous sync's tick (a load goes back) */
    /* health */
    u32 applied, late, resyncs, flushed, max_late_us, overflow;
    u32 flush_cancel;               /* a flush's KOFF cancelled a pending KON (C7) */
    u32 dropped_restore;            /* writes dropped by a state restore (C1) */
} g_rt;
extern u64 hal_clock_ticks(void) __attribute__((weak));
extern u32 hal_clock_ticks_per_second(void) __attribute__((weak));
static u32 rt_rate(void) {
    return hal_clock_ticks_per_second ? hal_clock_ticks_per_second() : 33868800u;
}
static long rt_latency_ms(void) {
    static long ms = -1;
    if (ms < 0) {
        const char *e = getenv("TOKI_SPU_LATENCY_MS");
        ms = e && *e ? atol(e) : 50;
        if (ms < 0) ms = 0;
        if (ms > 500) ms = 500;
    }
    return ms;
}
static int rt_queued_reg(u32 off) { return off < 0x180 || (off >= 0x188 && off <= 0x19A); }

/* SoundFont mode (pc/hal/soundfont.c; weak: the SPU harnesses link this
 * file alone).  A KON's snapshot of the keyed voices' notes is taken when
 * the bridge forwards it and travels with the write (RtWrite.tag); the
 * renderer starts the notes when the core takes the write. */
extern u32  soundfont_kon_snapshot(u16 kon_lo, u16 kon_hi) __attribute__((weak));
extern void soundfont_key_write(u32 off, u16 value, u32 tag) __attribute__((weak));
extern int  soundfont_rendering(void) __attribute__((weak));
extern void soundfont_render(s32 *frames, int n, float gain) __attribute__((weak));
extern void soundfont_tick(void) __attribute__((weak));
/* Enhanced sound quality (pc/hal/spu_quality.c; weak, as above). */
extern void spu_quality_tick(void) __attribute__((weak));
extern int  spu_quality_output_rate(void) __attribute__((weak));

/* One write reaching the core; KON/KOFF are checked against what the core
 * still holds unapplied (the audio-health counters). */
static void spu_core_write_tagged(u32 off, u16 value, u32 tag);
static void spu_core_write(u32 off, u16 value) { spu_core_write_tagged(off, value, 0); }
static void spu_core_write_tagged(u32 off, u16 value, u32 tag) {
    if (off >= 0x188 && off <= 0x18E && value) {
        u32 hi = off & 2;
        u16 pon = spu_psx_read_reg(SPU_MMIO_BASE_ADDR + 0x188 + hi);
        u16 poff = spu_psx_read_reg(SPU_MMIO_BASE_ADDR + 0x18C + hi);
        if (off < 0x18C) {
            g_hl.kon_writes++;
            g_hl.kon_lost += (u32)__builtin_popcount(pon & (u16)~value);
        } else {
            g_hl.koff_lost += (u32)__builtin_popcount(poff & (u16)~value);
            g_hl.kon_then_koff += (u32)__builtin_popcount(pon & value);
        }
    }
    spu_psx_write_reg(SPU_MMIO_BASE_ADDR + off, value);
    if (off >= 0x188 && off <= 0x18E && soundfont_key_write) soundfont_key_write(off, value, tag);
}

/* Would this write replace (or, a KOFF, cancel) keys the core has not
 * applied yet?  Then the core must mix a sample first. */
static int rt_key_conflict(const RtWrite *w) {
    u32 hi;
    if (w->off < 0x188 || w->off > 0x18E || !w->value) return 0;
    hi = w->off & 2;
    if (w->off < 0x18C) return spu_psx_read_reg(SPU_MMIO_BASE_ADDR + 0x188 + hi) != 0;
    return (spu_psx_read_reg(SPU_MMIO_BASE_ADDR + 0x18C + hi) |
            spu_psx_read_reg(SPU_MMIO_BASE_ADDR + 0x188 + hi)) != 0;
}

/* Apply every queued write, now (device lock held): before a direct core
 * access that must see them, a clock jump, the device closing. */
/* A queued write applied without mixing in between (a flush): the keys are
 * merged as the hardware would have seen them one after the other -- a
 * second KON adds its voices to the pending ones, a KOFF cancels a pending
 * KON of the same voice (keyed and released at once: silent) -- instead of
 * replacing them. */
static void rt_write_merged(u32 off, u16 value, u32 tag) {
    if (off >= 0x188 && off <= 0x18E && value) {
        u16 own = value;
        u32 hi = off & 2;
        u16 pon = spu_psx_read_reg(SPU_MMIO_BASE_ADDR + 0x188 + hi);
        u16 poff = spu_psx_read_reg(SPU_MMIO_BASE_ADDR + 0x18C + hi);
        if (off < 0x18C) {
            value |= pon;
        } else {
            if (pon & value) {
                spu_psx_write_reg(SPU_MMIO_BASE_ADDR + 0x188 + hi, (u16)(pon & ~value));
                g_rt.flush_cancel += (u32)__builtin_popcount(pon & value);
            }
            value |= poff;
        }
        spu_psx_write_reg(SPU_MMIO_BASE_ADDR + off, value);
        if (off < 0x18C) g_hl.kon_writes++;
        /* the SoundFont sees this write's own keys, in order */
        if (soundfont_key_write) soundfont_key_write(off, own, tag);
        return;
    }
    spu_core_write_tagged(off, value, tag);
}

static void rt_flush_locked(void) {
    while (g_rt.count) {
        RtWrite *w = &g_rt.q[g_rt.head];
        rt_write_merged(w->off, w->value, w->tag);
        g_rt.head = (g_rt.head + 1) % RTQ_CAP;
        g_rt.count--;
        g_rt.flushed++;
    }
}

/* Device thread: apply the writes due by `upto`.  Returns 1 when the next
 * due write has to wait for the core to mix a sample (rt_key_conflict). */
static int rt_apply_due(u64 upto) {
    while (g_rt.count) {
        RtWrite *w = &g_rt.q[g_rt.head];
        if (w->tick > upto) return 0;
        if (w->tick < g_rt.resync_floor) {
            /* from before a resync (a stall or a burst of emulated time):
             * the time line was cut there, apply it now */
        } else if (upto - w->tick > (u64)TICKS_PER_SAMPLE_RT * SPU_SLICE_LATE) {
            /* The write arrived after the cursor passed its tick: the host
             * (a late game frame, a device that pulls in bursts) was later
             * than the latency covers.  Applying it now would squeeze it
             * against the writes after it (a key-on and its key-off a
             * VBlank later would land together: the note never sounds).
             * Instead the cursor steps back to it -- the following writes
             * keep their spacing -- and the latency grows by as much
             * (g_rt.extra, decaying over ~30 s, at most RT_EXTRA_MAX_MS).
             * Later than RT_LATE_JITTER_MS is a stall, not jitter: applied
             * at once. */
            u64 by = upto - w->tick;
            u32 us = (u32)(by * 1000000u / rt_rate());
            g_rt.late++;
            if (us > g_rt.max_late_us) g_rt.max_late_us = us;
            if (by <= (u64)rt_rate() * RT_LATE_JITTER_MS / 1000u &&
                g_rt.extra + by <= (u64)rt_rate() * RT_EXTRA_MAX_MS / 1000u) {
                g_rt.extra += by;
                if (g_rt.extra > g_rt.max_extra) g_rt.max_extra = g_rt.extra;
                g_rt.play = upto = w->tick;
            }
        }
        if (rt_key_conflict(w)) return 1;
        spu_core_write_tagged(w->off, w->value, w->tag);
        g_rt.head = (g_rt.head + 1) % RTQ_CAP;
        g_rt.count--;
        g_rt.applied++;
    }
    return 0;
}

/* Device thread, at the start of a callback: where the play cursor should
 * be -- the game's clock (its last sync, plus the wall time since, at most
 * 50 ms) minus the latency.  Small differences are followed gently (the
 * cursor only decides when queued writes apply, never the audio's rate);
 * more than 250 ms -- a stall, a load, the first callback -- resyncs. */
static void rt_clock_update(void) {
    u64 rate = rt_rate(), freq = SDL_GetPerformanceFrequency();
    u64 wall_ticks, est;
    s64 target, diff;
    if (!g_rt.pub_valid || !freq) return;
    wall_ticks = (SDL_GetPerformanceCounter() - g_rt.pub_pc) * rate / freq;
    if (wall_ticks > rate / 20) wall_ticks = rate / 20;
    est = g_rt.pub_tick + wall_ticks;
    g_rt.extra -= g_rt.extra / 1024u;   /* the adaptive part decays (~30 s) */
    target = (s64)est - (s64)((u64)rt_latency_ms() * rate / 1000u) - (s64)g_rt.extra;
    if (target < 0) target = 0;
    diff = target - (s64)g_rt.play;
    if (!g_rt.play_valid || diff > (s64)(rate / 4) || diff < -(s64)(rate / 4)) {
        g_rt.play = (u64)target;
        g_rt.resync_floor = (u64)target;
        if (g_rt.play_valid) g_rt.resyncs++;
        g_rt.play_valid = 1;
    } else {
        g_rt.play = (u64)((s64)g_rt.play + diff / 16);
    }
}

/* Game thread, from spu_mmio_sync (device lock held): 1 when this sync's
 * voice/key writes go to the queue. */
extern int trace_turbo(void) __attribute__((weak));
extern int hal_ff_active(void) __attribute__((weak));
static int rt_queue_on(void) {
    /* not with PC_TURBO (automated runs: the game outruns the device and
     * nobody listens; they keep the direct writes), nor while fast-forward
     * is held (STATE-MACHINES.md C6: emulated time runs ahead of the device,
     * the queue would grow towards its cap and burst on the release; the
     * output is muted meanwhile) */
    return g_audio_dev && !parity_enabled() && rt_latency_ms() > 0 && hal_clock_ticks &&
           !(trace_turbo && trace_turbo()) && !(hal_ff_active && hal_ff_active());
}

/* After a quickload, a toolkit load or a rewind (dbg_state_after_restore):
 * the queue holds the old time line's voice and key writes; applied to the
 * restored SPU they would key ghost notes or leave notes on
 * (STATE-MACHINES.md C1).  Drop them -- the restored core is the machine as
 * it was -- and let the next sync start a new time line.  A SoundFont's
 * notes belong to the old time line too (pc/hal/soundfont.c). */
extern void soundfont_after_restore(void) __attribute__((weak));
void spu_hal_after_restore(void) {
    if (g_audio_dev) SDL_LockAudioDevice(g_audio_dev);
    g_rt.dropped_restore += g_rt.count;
    g_rt.count = g_rt.head = 0;
    g_rt.pub_valid = g_rt.play_valid = 0;
    g_rt.extra = 0;
    g_rt.last_tick = 0;
    if (soundfont_after_restore) soundfont_after_restore();
    if (g_audio_dev) SDL_UnlockAudioDevice(g_audio_dev);
}

static void rt_publish_locked(void) {
    u64 now = hal_clock_ticks();
    if (g_rt.pub_valid && (now < g_rt.last_tick || now - g_rt.last_tick > (u64)rt_rate())) {
        /* the emulated clock went back or jumped more than a second (a
         * state was loaded; the queue is not part of a state): the queue
         * is another time line's, apply it and start over */
        rt_flush_locked();
        g_rt.play_valid = 0;
    }
    g_rt.last_tick = now;
    g_rt.pub_tick = now;
    g_rt.pub_pc = SDL_GetPerformanceCounter();
    g_rt.pub_valid = 1;
}

static void rt_push_locked(u32 off, u16 value, u32 tag) {
    RtWrite *w;
    if (g_rt.count == RTQ_CAP) {        /* never expected: apply the oldest now */
        g_rt.overflow++;
        w = &g_rt.q[g_rt.head];
        rt_write_merged(w->off, w->value, w->tag);
        g_rt.head = (g_rt.head + 1) % RTQ_CAP;
        g_rt.count--;
    }
    w = &g_rt.q[(g_rt.head + g_rt.count) % RTQ_CAP];
    w->tick = g_rt.pub_tick;
    w->off = (u16)off;
    w->value = value;
    w->tag = tag;
    g_rt.count++;
}

static void mix_callback(void *userdata, u8 *stream, int len);

/* [audio] output_rate = 48000 (enhanced, pc/hal/spu_quality.c): the device
 * runs at 48 kHz and audio_resample.c turns the 44.1 kHz mix into its frames
 * (the mixer, the register queue, the hashes and the dumps stay at 44.1 kHz;
 * the deterministic mixer never resamples).  0 = the device runs at 44.1. */
static int g_device_rate = SAMPLE_RATE;
static void rs_pull(void *ctx, s16 *in, int frames) {
    mix_callback(ctx, (u8 *)in, frames * AUDIO_CHANNELS * (int)sizeof(s16));
}

static void audio_callback(void *userdata, u8 *stream, int len) {
    if (userdata == &g_realtime_tag && g_device_rate == AUDIO_RS_OUT_RATE) {
        int frames = len / ((int)sizeof(s16) * AUDIO_CHANNELS), done = 0;
        while (done < frames) {
            int n = frames - done > AUDIO_RS_MAX_OUT ? AUDIO_RS_MAX_OUT : frames - done;
            audio_rs_run((s16 *)stream + done * AUDIO_CHANNELS, n, rs_pull, userdata);
            done += n;
        }
        return;
    }
    mix_callback(userdata, stream, len);
}

static void mix_callback(void *userdata, u8 *stream, int len) {
    s16 *out = (s16 *)stream;
    int samples = len / ((int)sizeof(s16) * AUDIO_CHANNELS);
    int realtime = userdata == &g_realtime_tag;
    u64 t_start = realtime ? SDL_GetPerformanceCounter() : 0;
    if (realtime) health_callback_begin(samples, t_start);
    g_cb_frames += (u64)samples;

    memset(stream, 0, (size_t)len);

    /* The game is held (the backlog is open): the replay alone, nothing
     * of the game's mix runs (it would advance the SPU and drain the CD
     * FIFO), nothing is dumped. */
    if (g_held) {
        replay_mix(out, samples);
        return;
    }

#ifdef USE_TSF_FALLBACK
    if (g_tsf) {
        /* Legacy path: TSF handles both event processing and mixing. */
        tsf_render_short(g_tsf, (short *)stream, samples, 0);
    }
#else
    /* the DuckStation SPU core processes KEY_ON/KEY_OFF only at i==0 of each
     * GenerateSamples batch (spu.cpp:2457). If we advance the whole 1024-
     * frame callback in one shot, every SEQ event that should land at a
     * different sub-sample tick gets coalesced to batch-start, producing
     * audible crackling, blurred drum hits, and voice-reuse collisions.
     *
     * Slice the callback into SPU_SLICE_FRAMES chunks: for each slice,
     * advance the SEQ driver (emitting SPU register writes scheduled
     * during the slice window) and then mix exactly that many frames.
     * 64 frames = ~1.45ms at 44100Hz — close enough to real-hardware
     * tick granularity without the per-slice overhead of a full SPU
     * stream teardown. */
    {
        /* Slice at the next event boundary, capped at SPU_SLICE_MAX so a
         * silent stretch still gets periodic SPU register housekeeping.
         * Minimum slice is SPU_SLICE_MIN to bound per-callback overhead;
         * events scheduled closer than that still process within the
         * current slice, which is fine (they only coalesce with at most
         * SPU_SLICE_MIN-1 frames of lag — below the ~1ms audible tick). */
        enum { SPU_SLICE_MAX = 256, MUSIC_MAX = 16384 };
        const int SPU_SLICE_MIN = 16;
        int done = 0;
        /* a music pack's split (music_pack.c): the core also reports the
         * song's own share (`music`, for the dump), and while the song is
         * replaced the SPU bus carries the sound effects only */
        int music = music_pack_mixing && music_pack_mixing() && samples <= MUSIC_MAX;
        float gain_spu = music && music_pack_replacing() ? g_gain_sfx : g_gain_spu;
        int split = gain_spu != g_gain_cd;
        static s16 cd_tap[SPU_SLICE_MAX * 2];
        static s16 music_buf[MUSIC_MAX * 2];
        /* SoundFont mode: its frames for each slice join the split's rest
         * mix (spu_psx_inject); the bus then carries master x sfx */
        static s32 sf_frames[SPU_SLICE_MAX * 4];
        int sfr = music && soundfont_rendering && soundfont_rendering() && soundfont_render;
        float sf_gain = gain_spu > 0.0f ? g_gain_music / gain_spu : 0.0f;
        /* the real-time register queue (above): this device's batch is cut
         * at the queued writes' ticks */
        int rt = realtime && g_rt.pub_valid;
        if (rt) rt_clock_update();
        while (done < samples) {
            int slice = samples - done;
            if (slice > SPU_SLICE_MAX) slice = SPU_SLICE_MAX;
            if (rt) {
                u64 rate_tick = (u64)TICKS_PER_SAMPLE_RT;
                if (rt_apply_due(g_rt.play)) {
                    slice = 1;              /* the core takes the pending keys first */
                } else if (g_rt.count) {
                    u64 next = g_rt.q[g_rt.head].tick;
                    u64 frames = (next - g_rt.play + rate_tick - 1) / rate_tick;
                    if (frames < (u64)slice) slice = frames ? (int)frames : 1;
                }
            }
            if (g_seq) {
                u32 to_evt = seq_spu_frames_to_next_event(g_seq);
                if (to_evt < (u32)slice && to_evt >= (u32)SPU_SLICE_MIN) {
                    slice = (int)to_evt;
                }
                seq_spu_advance(g_seq, (u32)slice);
            }
            if (split) cd_audio_tap(cd_tap, slice);
            if (sfr) {
                soundfont_render(sf_frames, slice, sf_gain);
                spu_psx_inject(sf_frames);
            }
            if (music)
                spu_psx_mix_split(out + done * AUDIO_CHANNELS, music_buf + done * AUDIO_CHANNELS, slice);
            else
                spu_psx_mix(out + done * AUDIO_CHANNELS, slice);
            if (sfr) spu_psx_inject(NULL);
            if (split) {
                if (cd_audio_tap_count() == slice)
                    split_cd_gain(out + done * AUDIO_CHANNELS, cd_tap, slice, gain_spu);
                cd_audio_tap(NULL, 0);
            }
            done += slice;
            if (rt) g_rt.play += (u64)slice * TICKS_PER_SAMPLE_RT;
        }
        /* The ramp smoothing is for the PC-only SEQ player's slice-latched
         * key-ons.  The game's own audio is the core's continuous output:
         * smoothing it rewrote every steep edge of real waveforms (an XA
         * voice line differed from retail by up to 16,933 on those ramps). */
        if (g_seq) deglitch_slice_boundaries(out, samples, SPU_SLICE_MIN);
        if (!split) apply_master_gain(out, samples, gain_spu);
        /* the replacement song, after the bus gain (it carries its own) */
        if (music) music_pack_mix(out, music_buf, samples);
    }
    apply_master_gain(out, samples, g_master_vol);
#endif

    /* XA streaming mixes on top of whatever SPU produced. */
    audio_xa_tick(out, samples);

    /* The backlog's voice replay goes on top of the game's mix; the hash
     * log sees the game's mix without it. */
    audio_hash_record(out, samples, realtime);
    if (backlog_enabled && backlog_enabled()) sample_cd_level();
    replay_mix(out, samples);
    if (realtime) health_callback_end(out, samples, t_start);

    /* Fast-forward (pc/hal/enhance.c, enhanced profile only): the game
     * keys notes and streams voice N times faster than this callback plays
     * them, so the output is silenced while it is held.  The mixer still
     * ran, so the SPU stays in step; the CD-audio FIFO filled towards its
     * cap, so on the release the real-time mixer keeps only its newest
     * margin (STATE-MACHINES.md C6: the voice ran up to 0.5 s late). */
    {
        static int ff_was;
        int ff = hal_ff_active && hal_ff_active();
        if (ff) memset(stream, 0, (size_t)len);
        if (realtime && ff_was && !ff) cd_audio_trim(cd_audio_margin_frames() + 10u);
        if (realtime) ff_was = ff;
    }

    /* PC_AUDIO_DUMP=PATH (diagnostics): every frame this mixer hands the
     * device, raw s16le stereo 44.1 kHz, for the whole run; and the
     * developer WAV dump.  Written by the game thread (spu_tap_drain). */
    audio_out_record(stream, (u32)len, realtime);
}

/* Game thread: the developer WAV dump's first WAV_DUMP_SECONDS. */
static void wav_dump_write(const u8 *data, u32 len) {
    u32 room, frames;
    if (!g_wav_file || g_wav_samples >= WAV_DUMP_SECONDS * SAMPLE_RATE) return;
    room = (WAV_DUMP_SECONDS * SAMPLE_RATE - g_wav_samples) * AUDIO_CHANNELS * (u32)sizeof(s16);
    if (len > room) len = room;
    frames = len / (AUDIO_CHANNELS * (u32)sizeof(s16));
    fwrite(data, 1, len, g_wav_file);
    g_wav_samples += frames;
    if (g_wav_samples >= WAV_DUMP_SECONDS * SAMPLE_RATE) {
        u32 data_size = g_wav_samples * AUDIO_CHANNELS * (u32)sizeof(s16);
        u32 file_size = data_size + 36;
        fseek(g_wav_file, 4, SEEK_SET);
        fwrite(&file_size, 4, 1, g_wav_file);
        fseek(g_wav_file, 40, SEEK_SET);
        fwrite(&data_size, 4, 1, g_wav_file);
        fclose(g_wav_file);
        g_wav_file = NULL;
        printf("[SPU] WAV dump complete: audio_dump.wav (%u samples)\n",
               g_wav_samples);
    }
}

/* Deterministic opt-in path: SDL is opened paused and never unpaused.
 * Drive the exact normal mixer once per emulated VBlank, under its lock. */
void spu_parity_mix(s16 *samples, int frames) {
    extern u64 hal_clock_ticks(void);
    if (g_audio_dev) SDL_LockAudioDevice(g_audio_dev);
    /* this VBlank's samples: the CD audio the drive queued during it */
    cd_audio_mix_window(hal_clock_ticks(), (u32)frames);
    audio_callback(NULL, (u8 *)samples, frames * AUDIO_CHANNELS * (int)sizeof(s16));
    if (g_audio_dev) SDL_UnlockAudioDevice(g_audio_dev);
}

/* The audio-device lock, for the CD-ROM controller's FIFO (cd_audio.c),
 * which the drive model fills from the game thread. */
void spu_hal_audio_lock(void)   { if (g_audio_dev) SDL_LockAudioDevice(g_audio_dev); }
void spu_hal_audio_unlock(void) { if (g_audio_dev) SDL_UnlockAudioDevice(g_audio_dev); }

/* --------------------------------------------------------------------- */
/* Init / shutdown                                                       */
/* --------------------------------------------------------------------- */

/* Open the output device: 44.1 kHz s16 stereo, 1,024-frame callbacks.
 * allowed_changes 0: SDL converts whatever the hardware runs at (a 48 kHz
 * WASAPI mix format, a different period) to exactly this, so the mixer's
 * rate and slicing never depend on the device.  `log`: the player log
 * gets the driver, the hardware's own format and what was obtained. */
static SDL_AudioDeviceID audio_open_device(SDL_AudioSpec *have, int log) {
    SDL_AudioSpec want;
    SDL_AudioDeviceID dev;
    memset(&want, 0, sizeof(want));
    want.freq     = g_device_rate;
    want.format   = AUDIO_S16;
    want.channels = AUDIO_CHANNELS;
    want.samples  = AUDIO_SAMPLES;
    want.callback = audio_callback;
    want.userdata = &g_realtime_tag;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, have, 0);
    if (log) {
        const char *drv = SDL_GetCurrentAudioDriver();
        char *name = NULL;
        SDL_AudioSpec hw;
        int hw_ok;
        memset(&hw, 0, sizeof hw);
#if SDL_VERSION_ATLEAST(2, 24, 0)
        hw_ok = SDL_GetDefaultAudioInfo(&name, &hw, 0) == 0;
#else
        hw_ok = 0;
#endif
        if (hw_ok)
            printf("[SPU] audio driver %s, default device \"%s\": %d Hz %d ch, %d-frame buffer\n",
                   drv ? drv : "(none)", name ? name : "?", hw.freq, hw.channels, hw.samples);
        else
            printf("[SPU] audio driver %s (default device format unknown)\n", drv ? drv : "(none)");
        if (dev > 0)
            printf("[SPU] audio device: mixer %d Hz %d ch %d frames (%.1f ms) -> obtained %d Hz %d ch "
                   "%d frames, format 0x%04X%s\n", want.freq, want.channels, want.samples,
                   want.samples * 1000.0 / want.freq, have->freq, have->channels, have->samples,
                   (unsigned)have->format,
                   hw_ok && hw.freq && hw.freq != want.freq ? " (SDL resamples to the device)" : "");
        SDL_free(name);
    }
    return dev;
}

void spu_hal_init(void) {
    SDL_AudioSpec have;

    if (g_audio_initialized) return;
    if (spu_quality_output_rate && spu_quality_output_rate() == AUDIO_RS_OUT_RATE) {
        g_device_rate = AUDIO_RS_OUT_RATE;
        audio_rs_reset();
        printf("[SPU] output rate 48000 Hz: the 44.1 kHz mix is resampled (band-limited, 160/147)\n");
    }

    /* Always bring up the SPU core — the SEQ driver writes
     * SPU registers regardless of whether TSF is the mixing output, so
     * the core must exist. spu_psx_mix() is only called on the non-TSF
     * path below (see audio_callback). */
    spu_psx_init();
    cd_audio_init();   /* the CD-ROM controller's audio output -> CD input */
    spu_psx_write_reg(0x1F801DAA, 0xC000);   /* SPUCNT: enable + unmute */
    spu_psx_write_reg(0x1F801D80, 0x3FFF);   /* main vol L */
    spu_psx_write_reg(0x1F801D82, 0x3FFF);   /* main vol R */
    printf("[SPU] SPU core initialized\n");

#ifdef USE_TSF_FALLBACK
    {
        const char *sf2_path = "/usr/share/sounds/sf2/TimGM6mb.sf2";
        g_tsf = tsf_load_filename(sf2_path);
        if (!g_tsf) {
            sf2_path = "/usr/share/sounds/sf2/default-GM.sf2";
            g_tsf = tsf_load_filename(sf2_path);
        }
        if (g_tsf) {
            int i;
            tsf_set_output(g_tsf, TSF_STEREO_INTERLEAVED, SAMPLE_RATE, 0.0f);
            tsf_set_volume(g_tsf, g_master_vol);
            for (i = 0; i < 16; i++)
                tsf_channel_set_presetnumber(g_tsf, i, 0, (i == 9) ? 1 : 0);
            printf("[SPU] TSF fallback loaded: %s\n", sf2_path);
        } else {
            printf("[SPU] TSF fallback: no SoundFont found\n");
        }
    }
#endif

    g_audio_dev = audio_open_device(&have, 1);
    if (g_audio_dev > 0) {
        g_audio_initialized = 1;
        printf("[SPU] Audio initialized: %dHz %dch %d samples\n",
               have.freq, have.channels, have.samples);
        if (!parity_enabled()) atexit(audio_health_final);

        /* Write WAV header (sizes patched when dump finishes). */
        if (!parity_enabled()) g_wav_file = pc_dev_artifacts() ? fopen("audio_dump.wav", "wb") : NULL;
        if (g_wav_file) {
            u32 sample_rate = SAMPLE_RATE;
            u16 channels    = AUDIO_CHANNELS;
            u16 bits        = 16;
            u32 byte_rate   = sample_rate * channels * (u32)bits / 8u;
            u16 block_align = channels * bits / 8;
            u32 placeholder = 0;
            u32 fmt_size    = 16;
            u16 audio_format = 1;
            fwrite("RIFF", 1, 4, g_wav_file);
            fwrite(&placeholder, 4, 1, g_wav_file);
            fwrite("WAVEfmt ", 1, 8, g_wav_file);
            fwrite(&fmt_size, 4, 1, g_wav_file);
            fwrite(&audio_format, 2, 1, g_wav_file);
            fwrite(&channels, 2, 1, g_wav_file);
            fwrite(&sample_rate, 4, 1, g_wav_file);
            fwrite(&byte_rate, 4, 1, g_wav_file);
            fwrite(&block_align, 2, 1, g_wav_file);
            fwrite(&bits, 2, 1, g_wav_file);
            fwrite("data", 1, 4, g_wav_file);
            fwrite(&placeholder, 4, 1, g_wav_file);
            g_wav_samples = 0;
            printf("[SPU] WAV dump started: audio_dump.wav\n");
        }
        audio_files_open();
        if (!parity_enabled()) SDL_PauseAudioDevice(g_audio_dev, 0);
    } else {
        printf("[SPU] Audio init failed: %s\n", SDL_GetError());
    }
    spu_hal_apply_volumes();
}

/* The player's volumes from the settings file (pc/hal/settings.c); the
 * presenter's volume hotkeys call it again after a change.  Weak: harness
 * binaries that build this file without the settings module keep 1.0. */
#include "pc_settings.h"
extern const PcSettings *pc_settings_get(void) __attribute__((weak));
void spu_hal_apply_volumes(void) {
    const PcSettings *s;
    float spu, cd;
    if (!pc_settings_get) return;
    s = pc_settings_get();
    spu = (float)s->vol_master * s->vol_music * s->vol_sfx / 1000000.0f;
    cd = (float)s->vol_master * s->vol_voice / 10000.0f;
    if (g_audio_dev) SDL_LockAudioDevice(g_audio_dev);
    g_gain_spu = spu >= 0.999f ? 1.0f : spu;
    g_gain_cd = cd >= 0.999f ? 1.0f : cd;
    g_gain_sfx = (float)s->vol_master * s->vol_sfx / 10000.0f;
    if (g_gain_sfx >= 0.999f) g_gain_sfx = 1.0f;
    g_gain_music = (float)s->vol_master * s->vol_music / 10000.0f;
    if (g_gain_music >= 0.999f) g_gain_music = 1.0f;
    if (g_audio_dev) SDL_UnlockAudioDevice(g_audio_dev);
    if (g_gain_spu != 1.0f || g_gain_cd != 1.0f)
        printf("[SPU] player volume: music/sfx bus %.2f, CD/voice %.2f\n", g_gain_spu, g_gain_cd);
}

/* Host pause (pc/hal/present.c): silence the device while the game thread
 * is held.  The deterministic (parity) device is never running anyway. */
void spu_hal_set_paused(int paused) {
    if (g_audio_dev) SDL_LockAudioDevice(g_audio_dev);
    g_held = paused != 0;
    g_hl.last_pc = 0;                   /* a pause is not a late callback */
    if (g_audio_dev) SDL_UnlockAudioDevice(g_audio_dev);
    if (g_audio_dev && !parity_enabled()) SDL_PauseAudioDevice(g_audio_dev, paused ? 1 : 0);
}

/* The backlog started a voice replay (pc/hal/voice_replay.c): while the
 * game is held, run the device for it (the callback then mixes the replay
 * only).  Nothing to do while the game runs, or for the deterministic
 * mixer, whose device never plays. */
void spu_hal_side_audio(int on) {
    if (on && g_held && g_audio_dev && !parity_enabled()) SDL_PauseAudioDevice(g_audio_dev, 0);
}

void spu_hal_shutdown(void) {
    spu_reset_cancel();
    spu_transfer_shutdown();
    if (g_audio_dev > 0) {
        SDL_CloseAudioDevice(g_audio_dev);
        g_audio_dev = 0;
    }
    if (g_seq) { seq_spu_stop(g_seq); g_seq = NULL; }
    if (g_bank) { vab_free(g_bank); g_bank = NULL; }
#ifdef USE_TSF_FALLBACK
    if (g_tsf) { tsf_close(g_tsf); g_tsf = NULL; }
#else
    spu_psx_shutdown();
#endif
    g_audio_initialized = 0;
}

/* --------------------------------------------------------------------- */
/* SEQ loader / player                                                   */
/* --------------------------------------------------------------------- */

/* Try to open <STEM>.VH + <STEM>.VB next to the SEQ file; if that fails,
 * fall back to pc/assets/music/<STEM>.VH. Returns a VabBank on success
 * or NULL on any failure. */
static VabBank *load_matching_vab(const char *seq_path) {
    char vh[512], vb[512];
    VabBank *bank;
    /* Same directory as the SEQ. */
    path_with_ext(seq_path, ".VH", vh, sizeof(vh));
    path_with_ext(seq_path, ".VB", vb, sizeof(vb));
    bank = vab_load(vh, vb, VAB_SPU_BASE);
    if (bank) return bank;

    /* Fallback: pc/assets/music/<STEM>.VH */
    {
        const char *slash = strrchr(seq_path, '/');
        const char *stem  = slash ? slash + 1 : seq_path;
        char stem_buf[128];
        size_t slen = strlen(stem);
        if (slen >= sizeof(stem_buf)) slen = sizeof(stem_buf) - 1;
        memcpy(stem_buf, stem, slen);
        stem_buf[slen] = '\0';
        {
            char *dot = strrchr(stem_buf, '.');
            if (dot) *dot = '\0';
        }
        snprintf(vh, sizeof(vh), "%s/music/%s.VH", pc_asset_dir(), stem_buf);
        snprintf(vb, sizeof(vb), "%s/music/%s.VB", pc_asset_dir(), stem_buf);
        bank = vab_load(vh, vb, VAB_SPU_BASE);
    }
    return bank;
}

/* Shared conversion for the existing mono host-volume adapters. */
static u8 spu_average_volume(s16 left, s16 right) {
    int average = ((int)left + (int)right) / 2;
    if (average < 0) average = 0;
    if (average > 127) average = 127;
    return (u8)average;
}

/* Load + start a SEQ. Holds the SDL audio lock while swapping pointers
 * so the audio callback never touches a half-initialized state. Missing
 * VH/VB logs a warning but still loads the SEQ (notes will be silent
 * until a bank is available — not worse than TSF's GM fallback). */
void audio_seq_load_and_play(const char *path, int loop, s16 l_vol, s16 r_vol) {
    u8 *seq_data = NULL;
    u32 seq_size = 0;
    VabBank *new_bank = NULL;
    SeqSpuState *new_seq = NULL;
    u8 vol;

    seq_data = read_file(path, &seq_size);
    if (!seq_data) {
        printf("[SPU] audio_seq_load_and_play: cannot open %s\n", path);
        return;
    }

    new_bank = load_matching_vab(path);
    if (!new_bank) {
        printf("[SPU] audio_seq_load_and_play: no VAB for %s (silent)\n", path);
    }

    vol = spu_average_volume(l_vol, r_vol);

    SDL_LockAudioDevice(g_audio_dev);
    {
        SeqSpuState *old_seq = g_seq;
        VabBank     *old_bank = g_bank;
        g_seq  = NULL;   /* callback sees no SEQ during swap */
        g_bank = NULL;

        if (old_seq)  seq_spu_stop(old_seq);
        if (old_bank) vab_free(old_bank);

        /* seq_spu_start takes ownership of neither — seq_data must stay
         * valid for the SEQ lifetime, so stash it in a side pointer. */
        {
            static u8 *g_seq_data_owned = NULL;  /* freed on next swap */
            if (g_seq_data_owned) { free(g_seq_data_owned); }
            g_seq_data_owned = seq_data;
        }

        new_seq = seq_spu_start(seq_data, seq_size, new_bank, loop);
        if (!new_seq) {
            printf("[SPU] seq_spu_start failed for %s\n", path);
            if (new_bank) vab_free(new_bank);
            SDL_UnlockAudioDevice(g_audio_dev);
            return;
        }
        seq_spu_set_volume(new_seq, vol);
        g_seq_volume = vol;
        g_seq = new_seq;
        g_bank = new_bank;
    }
    SDL_UnlockAudioDevice(g_audio_dev);

#ifdef USE_TSF_FALLBACK
    /* When the fallback is active we don't use SPU for this SEQ, but we
     * keep the state machine alive so the sound-test UI can still toggle
     * between tracks. TSF path below would need its own loader; leave
     * silent for now. */
    (void)seq_data;
#endif

    printf("[SPU] SEQ play: %s (vol=%u, loop=%d, bank=%s)\n",
           path, (unsigned)vol, loop, new_bank ? "yes" : "no");
}

/* Preview a VAB: load VH/VB, allocate voice 0, key on first tone at the
 * given MIDI note + velocity. Used by the sound-test menu. */
int audio_preview_vab(const char *vh_path, int note, int velocity) {
    char vb_path[512];
    VabBank *new_bank;
    VabTone tone;
    u32 pitch;
    size_t vh_len;
    /* Derive .VB from .VH (swap extension). */
    vh_len = strlen(vh_path);
    if (vh_len < 3) return 0;
    memcpy(vb_path, vh_path, vh_len);
    vb_path[vh_len] = '\0';
    if (vh_len >= 2 && vb_path[vh_len - 2] == 'V' &&
        (vb_path[vh_len - 1] == 'H' || vb_path[vh_len - 1] == 'h')) {
        vb_path[vh_len - 1] = (vb_path[vh_len - 1] == 'H') ? 'B' : 'b';
    } else {
        snprintf(vb_path, sizeof(vb_path), "%s.VB", vh_path);
    }

    if (note <= 0) note = 60;
    if (velocity <= 0) velocity = 110;

    new_bank = vab_load(vh_path, vb_path, VAB_SPU_BASE);
    if (!new_bank) {
        printf("[SPU] VAB preview: load failed (%s / %s)\n", vh_path, vb_path);
        return 0;
    }

    /* Probe: does the bank have any program/tone? Try program 0, the
     * requested note; fall back to any tone that covers the note. */
    if (!vab_find_tone(new_bank, 0, (u8)note, &tone)) {
        /* Scan all 128 programs for any match. */
        int p;
        int found = 0;
        for (p = 0; p < 128 && !found; p++) {
            if (vab_find_tone(new_bank, (u8)p, (u8)note, &tone)) found = 1;
        }
        if (!found) {
            printf("[SPU] VAB preview: no tone for note %d in %s\n",
                   note, vh_path);
            vab_free(new_bank);
            return 0;
        }
    }

    SDL_LockAudioDevice(g_audio_dev);
    /* Stop any running SEQ to avoid stepping on voice 0. */
    {
        SeqSpuState *old_seq = g_seq;
        VabBank     *old_bank = g_bank;
        g_seq = NULL;
        g_bank = NULL;
        if (old_seq)  seq_spu_stop(old_seq);
        if (old_bank) vab_free(old_bank);
        g_bank = new_bank;
    }

#ifndef USE_TSF_FALLBACK
    {
        /* Voice-0 direct write. Pitch = 2^((note - center_note + fine/128)/12) * 0x1000. */
        float semitones = (float)note - (float)tone.center_note
                        + (float)tone.pitch_fine / 128.0f;
        float ratio = powf(2.0f, semitones / 12.0f);
        s32 p = (s32)(ratio * 4096.0f + 0.5f);
        if (p < 1)     p = 1;
        if (p > 0x3FFF) p = 0x3FFF;
        pitch = (u16)p;

        /* Per-voice volume scaled by note velocity. */
        {
            u32 v_scaled = (u32)velocity * 0x3FFF / 127;
            if (v_scaled > 0x3FFF) v_scaled = 0x3FFF;
            spu_psx_write_reg(0x1F801C00 + 0x00, (u16)v_scaled);  /* VOLL */
            spu_psx_write_reg(0x1F801C00 + 0x02, (u16)v_scaled);  /* VOLR */
        }
        spu_psx_write_reg(0x1F801C00 + 0x04, (u16)pitch);         /* PITCH */
        spu_psx_write_reg(0x1F801C00 + 0x06,
                          (u16)(tone.spu_addr >> 3));             /* START_ADDR */
        spu_psx_write_reg(0x1F801C00 + 0x08, tone.adsr1);
        spu_psx_write_reg(0x1F801C00 + 0x0A, tone.adsr2);
        spu_psx_write_reg(0x1F801C00 + 0x0E,
                          (u16)(tone.spu_loop_addr >> 3));        /* LOOP_ADDR */
        /* KEY_ON low word, bit 0 -> voice 0. */
        spu_psx_write_reg(0x1F801D88, 0x0001);
    }
#endif

    SDL_UnlockAudioDevice(g_audio_dev);
    printf("[SPU] VAB preview: %s note=%d vel=%d\n", vh_path, note, velocity);
    return 1;
}

/* --------------------------------------------------------------------- */
/* PSY-Q SPU API stubs                                                   */
/* --------------------------------------------------------------------- */

/* Register writes update the core and the legacy mapped MMIO view. */
void spu_reset_write(u32 offset, u16 value) {
    if (g_audio_dev) SDL_LockAudioDevice(g_audio_dev);
    /* the queued writes came first (STATE-MACHINES.md C12): applied after
     * this one they would undo the reset */
    rt_flush_locked();
    spu_psx_write_reg(0x1f801c00u + offset, value);
    if (offset < 0x200) g_mmio_last[offset / 2] = offset == 0x1aa ? (u16)(value & ~0x30u) : value;
    if (g_audio_dev) SDL_UnlockAudioDevice(g_audio_dev);
    if (pc_ptr_is_mapped((void *)(unsigned long)(0x1f801c00u + offset), 2))
        *(u16 *)(unsigned long)(0x1f801c00u + offset) = value;
}

u16 spu_reset_read(u32 offset) {
    u16 value;
    if (g_audio_dev) SDL_LockAudioDevice(g_audio_dev);
    rt_flush_locked();
    value = spu_psx_read_reg(0x1f801c00u + offset);
    if (g_audio_dev) SDL_UnlockAudioDevice(g_audio_dev);
    return value;
}

void spu_reset_enable_dma(void) {
    volatile u32 *priority = (volatile u32 *)0x1f8010f0ul;
    if (pc_ptr_is_mapped((const void *)priority, 4)) *priority |= 0xb0000u;
}

void SpuInit(void) {
    extern u8 libspu_init_io_buf[];
    extern u16 spu_tsa;
    extern s32 spu_transMode;
    extern void ResetCallback(void);
    printf("[SPU] SpuInit\n");
    spu_hal_init();
    spu_transfer_shutdown();
    ResetCallback();
    if (!spu_reset_initialize(libspu_init_io_buf)) return;
    {
        extern void spu_start_net_spend(unsigned long long mark);
        unsigned long long mark = hal_cost_mark();
        spu_transfer_init();
        spu_start_net_spend(mark);
    }
    /* _SpuInit's final ioctl selects the reverb base and low-level PIO
     * mode; SpuSetTransferMode(0) explicitly selects DMA later. */
    spu_tsa = 512;
    spu_transMode = 0;
    {
        extern u32 _spu_fd;
        extern s32 libspu_started_flag;
        extern u32 _spu_rev_startaddr[];
        extern s32 _spu_rev_offsetaddr;
        extern u32 _spu_rev_flag, _spu_rev_reserve_wa;
        extern u32 libspu_rev_mode, libspu_rev_delay, libspu_rev_feedback;
        extern s16 libspu_rev_depth_left, libspu_rev_depth_right;
        extern u32 _spu_keystat, _spu_keyon_bit, _spu_keyoff_bit;
        _spu_fd = 3;
        libspu_started_flag = 1;
        _spu_rev_flag = _spu_rev_reserve_wa = 0;
        libspu_rev_mode = libspu_rev_delay = libspu_rev_feedback = 0;
        libspu_rev_depth_left = libspu_rev_depth_right = 0;
        _spu_rev_offsetaddr = (s32)_spu_rev_startaddr[0];
        spu_reset_write(0x1a2, (u16)_spu_rev_offsetaddr);
        _spu_keystat = _spu_keyon_bit = _spu_keyoff_bit = 0;
    }
    spu_mmio_start();
}

void SpuQuit(void) { spu_hal_shutdown(); }

/* Stop the PC-only SEQ player (pc/hal/audio_scene.c PC_SCENE_MUSIC=1 and the
 * sound-test menu).  The game's own sequences go through the matched libsnd
 * SsSeqStop/SsEnd, which never touch this player. */
void audio_seq_stop(void) {
    SDL_LockAudioDevice(g_audio_dev);
    if (g_seq) { seq_spu_all_off(g_seq); seq_spu_stop(g_seq); g_seq = NULL; }
    SDL_UnlockAudioDevice(g_audio_dev);
}

s16 SsVabOpenHead(u8 *addr, s16 vab_id) {
    (void)addr; (void)vab_id;
    static int logged = 0;
    if (!logged) {
        printf("[SPU] SsVabOpenHead: deferred to audio_seq_load_and_play\n");
        logged = 1;
    }
    return vab_id;
}

/* SsVabTransBody / SsVabTransCompleted are the matched libsnd functions:
 * they DMA the VAB body through SpuWrite below and wait on its completion
 * event (src/main/pc-port/SD_VabLoad.c passes the retail $a1 vab id). */

/* --------------------------------------------------------------------- */
/* libsnd on the HAL                                                     */
/* --------------------------------------------------------------------- */

/* The game's sequences run on the matched libsnd: SsSeqOpen / SsSeqPlay /
 * SsSeqStop / SsSeqSetVol / SsSeqClose, the sequencer (SsSeqCalledTbyT ->
 * SeqPlay -> GetSeqData -> NoteOn -> SpuVmKeyOn ...) and the voice manager
 * (SpuVm*), reading the SEQ and VAB bytes the game loaded into RAM.  Two
 * things are the HAL's: the tick that drives the sequencer, and the SPU
 * register block those functions write.
 *
 * libsnd's current-VAB pointer globals (_svm_vh/_svm_pg/_svm_tn, 4-byte
 * slots at 0x80131618..0x80131620) are host pointer globals on the PC; see
 * pc/Makefile SND_PTR_GLOBALS. */
void *hal_svm_vh, *hal_svm_pg, *hal_svm_tn;

/* ---- SPU register block ------------------------------------------------
 *
 * The matched libsnd/libspu code writes the SPU registers through PS1
 * pointers to 0x1F801C00 (libsnd_spu_regs_ptr, _spu_RXX), which land in the mapped
 * 4 KiB MMIO page (pc/src/main.c).  spu_mmio_sync() is the device side of
 * that page: it forwards every register the game changed to the SPU core,
 * in the order the voice manager's flush writes them (voice registers, the
 * other global registers, KOFF, then KON), and refreshes the registers the
 * hardware owns (ADSR current volume, ENDX, SPUSTAT, current main volume)
 * from the core so the game reads what the SPU did.
 *
 * KON/KOFF are write-triggered: a non-zero write is forwarded once and the
 * page reads back 0, so the same mask written again keys the voices again.
 * Transfer control (0x1A6 address, 0x1A8 FIFO, 0x1AA SPUCNT, 0x1AC) is the
 * HAL's DMA model (spu_transfer.c / spu_reset.c), not this bridge.
 *
 * It runs around the sequencer tick (the VSync callback, before and after
 * SsSeqCalledTbyT) and once per VBlank before the audio for that VBlank is
 * mixed, so a register written anywhere in a frame reaches the core no
 * later than the next VBlank.  Sub-frame write timing is not modelled. */
#define SPU_MMIO_BASE 0x1F801C00ul
#define SPU_MMIO_REGS SPU_MMIO_REGS_EARLY  /* 0x200 bytes of 16-bit registers */

static int spu_mmio_owned_by_device(u32 off) {
    if (off < 0x180) return (off & 0xF) == 0xC;   /* ADSR current volume */
    switch (off) {
    case 0x188: case 0x18A: case 0x18C: case 0x18E:     /* KON/KOFF */
    case 0x19C: case 0x19E:                             /* ENDX */
    case 0x1A6: case 0x1A8: case 0x1AC:                 /* transfer control */
    case 0x1AE:                                         /* SPUSTAT */
    case 0x1B8: case 0x1BA:                             /* current main volume */
        return 1;
    }
    return 0;
}

/* SPUCNT (0x1AA) is shared: its transfer mode (bits 4-5) belongs to the
 * HAL's DMA model (spu_transfer.c reads the page), everything else -- SPU
 * enable, mute, noise clock, reverb master enable (bit 7, _spu_ioctl 96),
 * IRQ enable, CD/external audio -- is the device's and is forwarded with
 * the transfer mode cleared.
 *
 * Reverb: the matched libsnd/libspu chain (SsUtSetReverbType ->
 * SpuSetReverbModeParam -> _spu_ioctl 97/99 -> _spu_setReverbAttr;
 * SsUtReverbOn -> SpuSetReverb -> _spu_ioctl 96; SsUtSetReverbDepth)
 * writes the work-area base (mBASE 0x1A2), the configuration block
 * 0x1C0..0x1FF, the output volume (vLOUT/vROUT 0x184/0x186) and the voice
 * enables (EON 0x198/0x19A, SpuVmFlush), and SpuClearReverbWorkArea zeroes
 * the work area (spu_transfer.c).  They are forwarded like any register.
 * (Until 2026-10-31 the configuration calls were HAL stubs and these
 * registers were held back, after forwarding the output volume alone was
 * recorded as a constant tone in scene 64.) */
#define SPUCNT_OFF 0x1AA
#define SPUCNT_TRANSFER_MODE 0x0030u

static void spu_mmio_refresh_locked(volatile u16 *page) {
    u32 v;
    static const u16 status_regs[] = { 0x19C, 0x19E, 0x1AE, 0x1B8, 0x1BA };
    u32 i;
    for (v = 0; v < 24; v++) {
        u32 off = v * 0x10 + 0xC;
        page[off / 2] = g_mmio_last[off / 2] = spu_psx_read_reg(SPU_MMIO_BASE + off);
    }
    for (i = 0; i < sizeof(status_regs) / sizeof(status_regs[0]); i++) {
        u32 off = status_regs[i];
        page[off / 2] = g_mmio_last[off / 2] = spu_psx_read_reg(SPU_MMIO_BASE + off);
    }
}

/* PC_DEBUG=sound: every 2 s of emulated time, the three clocks the audio
 * depends on (emulated VBlanks, wall time, frames the device pulled) and the
 * XA FIFO.  `lead` is how far the device's consumption is ahead of the
 * emulated clock since the first line: it grows when the game thread runs
 * slower than real time (the sequencer and the XA sectors fall behind what
 * is being played). */
/* PC_DEBUG=sound (or all / 1).  Local, not pc_debug.h: the SPU harnesses
 * (pc/tests/spu_mmio_harness.c) link this file with unresolved symbols
 * ignored, and the clock below is weak for the same reason. */
static int sound_debug(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("PC_DEBUG");
        on = e && (strstr(e, "sound") || !strcmp(e, "all") || !strcmp(e, "1")) ? 1 : 0;
    }
    return on;
}
extern u64 hal_clock_ticks(void) __attribute__((weak));
extern u32 hal_clock_vblanks(void) __attribute__((weak));
static void audio_stats(void) {
    static u32 last_vbl, n;
    static u64 t0_ticks, t0_frames;
    static u32 t0_wall;
    static u64 prev_pc;
    static u32 burst, burst_max, burst_vbl;
    static double stall_max;
    u32 vbl, played, dropped, skipped, voices = 0, v, gaps, gap_frames;
    double emu, wall, dev;
    if (!sound_debug() || !hal_clock_vblanks || !hal_clock_ticks) return;
    vbl = hal_clock_vblanks();
    if (vbl == last_vbl) return;
    /* bursts: VBlanks the game thread delivered less than 2 ms of wall time
     * after the previous one (their sequencer ticks reach the SPU at once) */
    {
        u64 pc = SDL_GetPerformanceCounter();
        double gap = prev_pc ? (double)(pc - prev_pc) / SDL_GetPerformanceFrequency() : 0.0;
        if (gap > stall_max) stall_max = gap;
        if (prev_pc && gap < 0.002) {
            burst++;
            burst_vbl++;
            if (burst > burst_max) burst_max = burst;
        } else {
            burst = 0;
        }
        prev_pc = pc;
    }
    /* the sound driver's song state (pc/src/globals.c, read only) */
    {
        extern s32 gSDInfo[] __attribute__((weak));
        extern s32 gSDSerialInfo __attribute__((weak));
        extern s16 cur_seq_num __attribute__((weak)), gSDSeqNo __attribute__((weak));
        extern s16 seq_req __attribute__((weak)), seq_vol_l __attribute__((weak));
        static s32 last_info = -1, last_serial = -1;
        static s16 last_cur = -2, last_no = -2, last_req = -2;
        s32 info = &gSDInfo[0] ? gSDInfo[0] & 0x101500 : 0, serial = &gSDSerialInfo ? gSDSerialInfo & 3 : 0;
        if (&gSDInfo[0] && &gSDSerialInfo && &cur_seq_num && &gSDSeqNo && &seq_req && &seq_vol_l &&
            (info != last_info || serial != last_serial || cur_seq_num != last_cur ||
            gSDSeqNo != last_no || seq_req != last_req)) {
            u32 sounding = 0;
            for (v = 0; v < 24; v++)
                if (spu_psx_read_reg(0x1F801C0C + v * 16)) sounding++;
            fprintf(stderr, "[AUDIO] vbl %u song: info 0x%06X serial %u cur %d no %d req %d vol %d, "
                    "%u voices sounding\n", vbl, (unsigned)info, (unsigned)serial, cur_seq_num,
                    gSDSeqNo, seq_req, seq_vol_l, sounding);
            last_info = info; last_serial = serial; last_cur = cur_seq_num;
            last_no = gSDSeqNo; last_req = seq_req;
        }
    }
    if (vbl % 120u) { last_vbl = vbl; return; }
    last_vbl = vbl;
    if (!n++) { t0_ticks = hal_clock_ticks(); t0_frames = g_cb_frames; t0_wall = SDL_GetTicks(); }
    for (v = 0; v < 24; v++)
        if (spu_psx_read_reg(0x1F801C0C + v * 16)) voices++;
    cd_audio_stats(&played, &dropped, &skipped);
    cd_audio_underruns(&gaps, &gap_frames);
    emu = (double)(hal_clock_ticks() - t0_ticks) / 33868800.0;
    wall = (SDL_GetTicks() - t0_wall) / 1000.0;
    dev = (double)(g_cb_frames - t0_frames) / SAMPLE_RATE;
    fprintf(stderr, "[AUDIO] vbl %u emu %.2fs wall %.2fs device %.2fs lead %+.3fs "
            "xa_fifo %u xa %u/%u dropped, %u gaps %u frames, voices %u burst %u (longest %u) stall %.0f ms\n",
            vbl, emu, wall, dev, dev - emu, cd_audio_fifo_frames(), played, dropped,
            gaps, gap_frames, voices, burst_vbl, burst_max, stall_max * 1000.0);
    burst_vbl = burst_max = 0;
    stall_max = 0.0;
}

/* The audio-health line (see g_hl above).  Game thread; at most one line
 * per 10 s of wall time, and only when a problem counter moved since the
 * last line -- or `final` (shutdown): the run's totals, always. */
static void audio_health_report(int final) {
    static u32 last_ms, last_sig;
    u32 now = SDL_GetTicks(), sig, played, dropped, skipped, gaps, gap_frames;
    if (!g_audio_dev && !final) return;
    if (!final && now - last_ms < 10000u) return;
    last_ms = now;
    cd_audio_stats(&played, &dropped, &skipped);
    cd_audio_underruns(&gaps, &gap_frames);
    sig = g_hl.late + g_hl.kon_lost + g_hl.koff_lost + g_hl.kon_then_koff + g_hl.clipped + dropped + gaps +
          g_rt.late + g_rt.resyncs + g_rt.overflow + g_tap_out.lost + g_tap_hash.lost;
    if (!final && sig == last_sig) return;
    last_sig = sig;
    printf("[AUDIO] health%s: callbacks %u (frames %u-%u, late %u, longest gap %.1f ms, period %.1f ms, longest mix %.1f ms), "
           "clipped %u; key-on writes %u, key-on lost %u, key-off lost %u, key-off over a pending key-on %u; "
           "voice writes queued %u (latency %ld ms + adaptive %.1f ms (at most %.1f), late %u, latest %.1f ms, resyncs %u, "
           "flushed %u (key-ons cancelled %u), overflow %u, dropped by a restore %u); "
           "xa sectors %u, dropped %u, gaps %u (%u frames)\n",
           final ? " (run total)" : "", g_hl.callbacks, g_hl.min_frames, g_hl.max_frames, g_hl.late, g_hl.max_gap_us / 1000.0,
           g_hl.period_us / 1000.0, g_hl.max_mix_us / 1000.0, g_hl.clipped, g_hl.kon_writes,
           g_hl.kon_lost, g_hl.koff_lost, g_hl.kon_then_koff,
           g_rt.applied, rt_queue_on() ? rt_latency_ms() : 0L, g_rt.extra * 1000.0 / rt_rate(),
           g_rt.max_extra * 1000.0 / rt_rate(), g_rt.late, g_rt.max_late_us / 1000.0,
           g_rt.resyncs, g_rt.flushed, g_rt.flush_cancel, g_rt.overflow, g_rt.dropped_restore, played, dropped, gaps,
           gap_frames);
    if (g_tap_out.lost || g_tap_hash.lost)
        printf("[AUDIO] audio dump ring overflowed: %u / %u bytes not written\n", g_tap_out.lost, g_tap_hash.lost);
    fflush(stdout);
}

void spu_mmio_sync(void) {
    volatile u16 *page = (volatile u16 *)SPU_MMIO_BASE;
    u32 off;
    u16 kon_lo, kon_hi, koff_lo, koff_hi;
    u32 tag;
    int rt;
    audio_stats();
    audio_health_report(0);
    spu_tap_drain();
    if (!g_mmio_live) return;
    if (g_audio_dev) SDL_LockAudioDevice(g_audio_dev);
    /* real time: voice and key registers go through the queue (g_rt) */
    rt = rt_queue_on();
    if (rt) rt_publish_locked();
    else if (g_rt.count) rt_flush_locked();
#define SYNC_WRITE_T(o, v, t) (rt && rt_queued_reg(o) ? rt_push_locked((o), (v), (t)) \
                                                    : spu_core_write_tagged((o), (v), (t)))
#define SYNC_WRITE(o, v) SYNC_WRITE_T(o, v, 0)
    for (off = 0; off < 0x200; off += 2) {
        u16 value = page[off / 2];
        if (off == SPUCNT_OFF) value &= (u16)~SPUCNT_TRANSFER_MODE;
        if (spu_mmio_owned_by_device(off) || value == g_mmio_last[off / 2])
            continue;
        g_mmio_last[off / 2] = value;
        SYNC_WRITE(off, value);
    }
    koff_lo = page[0x18C / 2]; koff_hi = page[0x18E / 2];
    kon_lo = page[0x188 / 2]; kon_hi = page[0x18A / 2];
    page[0x18C / 2] = page[0x18E / 2] = page[0x188 / 2] = page[0x18A / 2] = 0;
    if (koff_lo) SYNC_WRITE(0x18C, koff_lo);
    if (koff_hi) SYNC_WRITE(0x18E, koff_hi);
    /* SoundFont mode: the keyed voices' notes, read now (the libsnd records
     * are this sync's), applied with the KON */
    tag = (kon_lo || kon_hi) && soundfont_kon_snapshot ? soundfont_kon_snapshot(kon_lo, kon_hi) : 0;
    if (kon_lo) SYNC_WRITE_T(0x188, kon_lo, tag);
    if (kon_hi) SYNC_WRITE_T(0x18A, kon_hi, tag);
#undef SYNC_WRITE
#undef SYNC_WRITE_T
    spu_mmio_refresh_locked(page);
    /* a music pack's voice mask follows the voices just keyed */
    if (music_pack_voice_sync) music_pack_voice_sync();
    if (g_audio_dev) SDL_UnlockAudioDevice(g_audio_dev);
}

/* Start forwarding: the page's current contents are what the game already
 * wrote before the SPU existed (pre-SpuInit), and SpuInit's reset has
 * defined the device state, so they are the baseline, not new writes. */
static void spu_mmio_start(void) {
    volatile u16 *page = (volatile u16 *)SPU_MMIO_BASE;
    u32 i;
    if (!pc_ptr_is_mapped((const void *)page, 0x200)) return;
    if (g_audio_dev) SDL_LockAudioDevice(g_audio_dev);
    rt_flush_locked();
    for (i = 0; i < SPU_MMIO_REGS; i++) {
        page[i] = spu_psx_read_reg(SPU_MMIO_BASE + i * 2);
        g_mmio_last[i] = page[i];
    }
    g_mmio_last[SPUCNT_OFF / 2] &= (u16)~SPUCNT_TRANSFER_MODE;
    page[0x188 / 2] = page[0x18A / 2] = page[0x18C / 2] = page[0x18E / 2] = 0;
    g_mmio_live = 1;
    if (g_audio_dev) SDL_UnlockAudioDevice(g_audio_dev);
}

/* ---- Sequencer tick ------------------------------------------------------
 *
 * _SsStart (src/main/matched/_SsStart.c, 0x8008BB24) is the matched body
 * with one divergence: the tick callback.  Retail passes libsnd's code
 * address 0x80090DA0 (SsSeqCalledTbyT) to VSyncCallback/InterruptCallback;
 * here the native function is installed, wrapped by the register bridge.
 * This game selects SsSetTickMode(1) (SD_Init) on NTSC, i.e. SsFCALL 5,
 * and starts with SsStart2: the VSync-callback tick, one call per VBlank.
 * The root-counter tick modes (SsFCALL 1..4) program RCnt 2 on retail; the
 * HAL has no root-counter interrupt, so they are reported, not faked. */
extern s32 SsFCALL, _snd_seq_no_tick, _snd_use_vsync_cb, _snd_1per2, _snd_vsync_cb;
extern volatile s32 _snd_use_interrupt_id;
extern void SsSeqCalledTbyT(void);
extern void *VSyncCallback(void (*)(void));

/* The sequencer tick's retail CPU time is part of the VBlank exception's
 * (pc/hal/cpu_cost.c, pc/hal/system.c deliver_sdk).  Weak for harnesses. */
extern void hal_cpu_cost_seq_begin(void) __attribute__((weak));
extern void hal_cpu_cost_seq_end(void) __attribute__((weak));
static void hal_snd_tick(void) {
    spu_mmio_sync();
    if (hal_cpu_cost_seq_begin) hal_cpu_cost_seq_begin();
    SsSeqCalledTbyT();
    if (hal_cpu_cost_seq_end) hal_cpu_cost_seq_end();
    spu_mmio_sync();
    /* music packs / the song dump: reads libsnd's state, never writes it */
    if (music_pack_tick) music_pack_tick();
    /* SoundFont mode (loads / unloads the SF2) and the enhanced sound
     * quality (interpolation, reverb): settings, never game state */
    if (soundfont_tick) soundfont_tick();
    if (spu_quality_tick) spu_quality_tick();
}

void _SsStart(s32 arg0) {
    s32 i;
    for (i = 0; i != 1000; i += 4) {
    }
    switch (SsFCALL) {
    case 5:
        if (arg0 == 0) {
            _snd_use_vsync_cb++;
        } else {
            _snd_use_interrupt_id = 0;
        }
        break;
    case 0:
        return;
    default:
        if ((SsFCALL == 1 || SsFCALL == 4 || SsFCALL > 5) && _snd_seq_no_tick != 0)
            return;
        printf("[SPU] _SsStart: tick mode %d needs a root-counter interrupt; "
               "not modelled, sequencer not started\n", (int)SsFCALL);
        return;
    }
    if (_snd_use_vsync_cb != 0) {
        VSyncCallback(hal_snd_tick);
        return;
    }
    printf("[SPU] _SsStart: VSync tick through a root-counter interrupt is "
           "not modelled, sequencer not started\n");
}

void SsStart(void)  { _SsStart(1); }
/* SsStart2's retail CPU (with _SsStart, VSyncCallback, setIntrVSync folded
 * in): the boot's row (cpu_cost.h HAL_SDK_COST, cpu_cost.c BOOT_ROWS). */
void SsStart2(void) { HAL_SDK_COST("SsStart2"); _SsStart(0); }

/* Low-level register helpers — most game code never reaches these, but
 * wire them to the real SPU core so any direct libspu user still works. */
void SpuSetKey(s32 on_off, s32 voice_mask) {
#ifdef USE_TSF_FALLBACK
    (void)on_off; (void)voice_mask;
#else
    u32 reg_lo = (on_off ? 0x1F801D88 : 0x1F801D8C);
    u32 reg_hi = reg_lo + 2;
    spu_psx_write_reg(reg_lo, (u16)(voice_mask & 0xFFFF));
    spu_psx_write_reg(reg_hi, (u16)((voice_mask >> 16) & 0xFFFF));
#endif
}
void SpuSetVoiceAttr(void *attr) { (void)attr; }
void SpuFlush(void) { }
/* Transfer commits happen on the game clock; hold the audio lock only
 * while copying RAM, never while advancing time or delivering callbacks. */
void spu_transfer_commit(const u8 *data, u32 address, u32 size) {
    if (g_audio_dev) SDL_LockAudioDevice(g_audio_dev);
    /* The queue is NOT flushed here (STATE-MACHINES.md C7).  It used to be,
     * "so queued voice writes precede the upload"; but a queued write is a
     * register write -- no voice decodes a sample until the mixer runs, so
     * the RAM is the new data for it either way -- and a flush applies a
     * VBlank's key-on and the next VBlank's key-off together, which cancels
     * the note (measured: 1 key-on lost at a bank upload in the real-time
     * title -> name entry, parity).  The queue keeps its time line. */
    /* PC_DEBUG=sound: an upload into SPU RAM, and how many sounding voices
     * (ADSR level != 0) play a sample that starts inside it */
    if (sound_debug() && size >= 2048 && hal_clock_vblanks) {
        u32 v, sounding = 0, hit = 0;
        for (v = 0; v < 24; v++) {
            u32 start = (u32)spu_psx_read_reg(0x1F801C06 + v * 16) * 8u;
            if (!spu_psx_read_reg(0x1F801C0C + v * 16)) continue;
            sounding++;
            if (start >= address && start < address + size) hit++;
        }
        fprintf(stderr, "[AUDIO] vbl %u upload 0x%05X+%u: %u voices sounding, %u from inside it\n",
                hal_clock_vblanks(), address, size, sounding, hit);
    }
    spu_psx_dma_write(data, address, size);
    if (g_audio_dev) SDL_UnlockAudioDevice(g_audio_dev);
}
void spu_transfer_readback(u8 *dst, u32 address, u32 size) {
    if (g_audio_dev) SDL_LockAudioDevice(g_audio_dev);
    spu_psx_dma_read(dst, address, size);
    if (g_audio_dev) SDL_UnlockAudioDevice(g_audio_dev);
}
long SpuClearReverbWorkArea(long mode) {
    if (mode < 0 || mode >= 10) return -1;
    return spu_transfer_clear((s32)mode);
}
s32 SpuSetTransferMode(s32 mode) { return spu_transfer_mode(mode); }
s32 SpuWrite(u8 *addr, u32 size) { return spu_transfer_write(addr, size); }
u32 SpuSetTransferStartAddr(u32 addr) { return spu_transfer_address(addr); }
s32 SpuIsTransferCompleted(s32 wait) { return spu_transfer_completed(wait); }

/* SsSetMVol (src/main/matched/SsSetMVol.c): _spu_ioctl's common-attribute
 * write of the main volume, (vol * 16383) / 127 per side, fixed mode. */
void SsSetMVol(s16 l_vol, s16 r_vol) {
    HAL_SDK_COST("SsSetMVol");   /* _spu_ioctl, _spu_setCommonAttr folded in */
    spu_reset_write(0x180, (u16)(((s32)l_vol * 16383 / 127) & 0x7FFF));
    spu_reset_write(0x182, (u16)(((s32)r_vol * 16383 / 127) & 0x7FFF));
}

/* --------------------------------------------------------------------- */
/* XA external API                                                       */
/* --------------------------------------------------------------------- */

int audio_xa_start(const char *path) {
    CdxaStream *new_stream;

    SDL_LockAudioDevice(g_audio_dev);
    if (g_xa_stream) {
        cdxa_close(g_xa_stream);
        g_xa_stream = NULL;
    }
    g_xa_ring_head = g_xa_ring_tail = 0;
    g_xa_active = 0;

    if (!path || !*path) {
        SDL_UnlockAudioDevice(g_audio_dev);
        return 1;
    }

    new_stream = cdxa_open(path);
    if (!new_stream) {
        SDL_UnlockAudioDevice(g_audio_dev);
        printf("[SPU] XA open failed: %s\n", path);
        return 0;
    }
    g_xa_stream = new_stream;
    g_xa_active = 1;
    g_xa_loop = 1;
    /* Pre-roll: fill the ring before the callback first touches it. */
    xa_refill(); xa_refill(); xa_refill(); xa_refill();
    printf("[SPU] XA started: %s (ring=%d frames)\n", path, XA_RING_FRAMES);
    SDL_UnlockAudioDevice(g_audio_dev);
    return 1;
}

void audio_xa_stop(void) {
    SDL_LockAudioDevice(g_audio_dev);
    if (g_xa_stream) {
        cdxa_close(g_xa_stream);
        g_xa_stream = NULL;
    }
    g_xa_ring_head = g_xa_ring_tail = 0;
    g_xa_active = 0;
    SDL_UnlockAudioDevice(g_audio_dev);
}
