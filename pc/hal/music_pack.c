/* SPDX-License-Identifier: GPL-3.0-or-later */
/* music_pack.c -- music packs and the song dump (music_pack.h).
 *
 * ENHANCED-ONLY REPLACEMENT, READ-ONLY OBSERVER.  The game's own sound
 * driver keeps playing every song through the matched libsnd; nothing here
 * writes PS1 memory, calls a game or libsnd function, or changes emulated
 * time.  Per VBlank, after the sequencer tick, music_pack_tick() reads:
 *   _snd_openflag      which sequence access numbers are open (SsSeqOpen)
 *   _ss_score[acc]     the score record: +8 the data after the SEQ header
 *                      (InitSoundSeq; the 'pQES' header sits just before
 *                      it), +114/+116 the sequence volume (Snd_SetVol, i.e.
 *                      SsSeqSetVol and the driver's fades), +144 the play
 *                      flags (1 play, 2 pause, 4 stop, 0x200 ended:
 *                      Snd_SetPlayMode, SsSeqPause, SsSeqReplay, SsSeqStop,
 *                      GetMetaEvent's end of track, SsSeqClose)
 *   _svm_voice[v]+10   the voice's owner: (track << 8) | acc for a sequence
 *                      note (NoteOn -> SpuVmKeyOn), 32 for a sound effect
 *                      (SpuVmSeKeyOn), -1 never used (SpuVmInit)
 * and the SPU core's current main volume (SsSetMVol, the driver's
 * main-volume fades).
 *
 * A song's id is "seq-" + FNV-1a 32 of its SEQ bytes ('pQES' .. FF 2F).
 * When a song starts and <music dir>/packs/<pack>/<id>.{ogg,flac,wav}
 * exists (enhanced profile, MUSIC_PACK switch, a pack selected), the core's
 * music split is turned on with that sequence's voices as the mask
 * (spu_psx_music_split): every voice is still sampled, keyed, enveloped and
 * reverberated exactly as before -- ADSR, ENDX, capture buffers, SPU RAM and
 * the libsnd state the game polls are unchanged -- but the output is the mix
 * of the other voices (sound effects) plus the CD input, and the file is
 * mixed in instead.  The file follows the game: it starts at the song's
 * first sequencer tick, pauses and resumes with SsSeqPause / SsSeqReplay,
 * stops on SsSeqStop / SsSeqClose (a 20 ms ramp), scales with the sequence
 * volume relative to the song's starting volume and with the main volume,
 * and loops (its loop points, else the whole file) for as long as the
 * sequence is still playing; after the sequence ends by itself it plays to
 * its end.  No file: the original song plays.
 *
 * The dump ([audio] music_dump, TOKI_MUSIC_DUMP=1; any profile -- the
 * output is not changed) writes the song's own share of the SPU output (the
 * full mix minus the mix without its voices, before the main volume) to
 * <music dir>/dump/<id>.wav while it plays, keeping the longest take.
 *
 * Events (start, pause, resume, volume, stop, end, close, file loops, file
 * end) go to stdout as [MUSIC] lines (TOKI_MUSIC_EVENTS: by default the song
 * events only, in every run and profile -- the player log) and, with
 * TOKI_MUSIC_LOG=PATH or while
 * dumping (<music dir>/dump/heard.jsonl), as JSON lines -- the record
 * pc/tools/music_tracks.py turns into the track list.
 */
#include "types.h"
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <dirent.h>

#include "music_decode.h"
#include "music_pack.h"
#include "profile.h"
#include "pc_settings.h"
#include "pc_menu.h"
#include "spu_psx.h"

/* SoundFont mode (pc/hal/soundfont.c; weak: harnesses).  It replaces the
 * music voices through the same split; while it renders, the song dump is
 * suspended (the split's music share would carry the SoundFont's reverb). */
extern int soundfont_split_on(void) __attribute__((weak));
extern u32 soundfont_split_mask(void) __attribute__((weak));
extern int soundfont_rendering(void) __attribute__((weak));

/* ---- the game state read here (PS1 RAM; read only) -------------------- */
extern s32 _snd_openflag;
extern u32 _ss_score[];          /* 0x80131380: a 4-byte PS1 pointer per sequence */
extern u8 _svm_voice[];          /* 0x800EB8E8: 24 records of 0x18 bytes */
extern s32 frame_counter __attribute__((weak));
extern u8 now_step __attribute__((weak)), now_sub_step __attribute__((weak));
extern u32 hal_clock_vblanks(void) __attribute__((weak));
extern const char *hal_overlay_loaded(void) __attribute__((weak));
extern void spu_hal_audio_lock(void);
extern void spu_hal_audio_unlock(void);
extern const PcSettings *pc_settings_get(void) __attribute__((weak));
extern PcSettings *pc_settings_mut(void) __attribute__((weak));
extern const char *hal_setting_str(const char *name, const char *dflt) __attribute__((weak));
extern long hal_setting_long(const char *name, long dflt) __attribute__((weak));

#define RAM_BASE 0x80000000u
#define RAM_MASK 0x1FFFFFu
#define PS1_PTR(h) ((const u8 *)(uintptr_t)(RAM_BASE | ((u32)(h) & RAM_MASK)))

#define SCORE_SIZE 168
#define SCORE_VAB_ID 0x4A       /* s16: the VAB the sequence plays (InitSoundSeq) */
#define SCORE_LOOP_START 8
#define SCORE_VOL_L 114
#define SCORE_VOL_R 116
#define SCORE_FLAGS 144
#define FLAG_PLAY 0x1
#define FLAG_PAUSE 0x2
#define FLAG_STOP 0x4
#define FLAG_ENDED 0x200
#define VOICE_STRIDE 0x18
#define VOICE_OWNER 10
#define NUM_VOICES 24
#define MAX_ACC 16
#define OUT_RATE 44100
#define RAMP_FRAMES 882          /* 20 ms: pause / resume / stop ramps */
#define DUMP_TAIL_VBLANKS 240    /* keep dumping the release tail after the end */
#define SRC_RING 65536           /* source frames decoded ahead (1.4-1.5 s) */
#define DUMP_RING 131072         /* dump frames waiting for the game thread (3 s) */

/* ---- track identity -------------------------------------------------- */

uint32_t music_pack_seq_span(const uint8_t *p, uint32_t max) {
    uint32_t pos = 15;
    uint8_t status = 0;
    if (!p || max < 16 || memcmp(p, "pQES", 4)) return 0;
    while (pos < max) {
        int k;
        uint8_t b, hi;
        for (k = 0; k < 4; k++) {          /* delta time (variable length) */
            if (pos >= max) return 0;
            if (!(p[pos++] & 0x80)) break;
        }
        if (k == 4 || pos >= max) return 0;
        b = p[pos];
        if (b & 0x80) {
            status = b;
            pos++;
        } else if (!status) {
            return 0;
        }
        hi = status & 0xF0;
        if (status == 0xFF) {               /* meta: libsnd knows tempo and end */
            uint8_t type;
            if (pos >= max) return 0;
            type = p[pos++];
            if (type == 0x2F) return pos;
            if (type != 0x51) return 0;
            pos += 3;
        } else if (hi == 0xC0 || hi == 0xD0) {
            pos += 1;
        } else if (hi >= 0x80 && hi <= 0xE0) {
            pos += 2;
        } else {
            return 0;
        }
    }
    return 0;
}

int music_pack_track_id(const uint8_t *p, uint32_t max, char *out, int n) {
    uint32_t len = music_pack_seq_span(p, max), h = 2166136261u, i;
    if (!len || n < 13) return -1;
    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    snprintf(out, (size_t)n, "seq-%08x", (unsigned)h);
    return 0;
}

/* ---- configuration ---------------------------------------------------- */

static const PcSettings *settings(void) {
    return pc_settings_get ? pc_settings_get() : NULL;
}

static const char *pack_name(void) {
    const PcSettings *s = settings();
    const char *d = s ? s->music_pack : "";
    const char *v = hal_setting_str ? hal_setting_str("TOKI_MUSIC_PACK", d) : getenv("TOKI_MUSIC_PACK");
    if (!v) v = d;
    if (!strcasecmp(v, "off") || !strcasecmp(v, "none")) return "";
    return v;
}

static int dump_on(void) {
    const PcSettings *s = settings();
    long d = s ? s->music_dump : 0;
    if (hal_setting_long) return hal_setting_long("TOKI_MUSIC_DUMP", d) != 0;
    return getenv("TOKI_MUSIC_DUMP") ? atoi(getenv("TOKI_MUSIC_DUMP")) != 0 : (int)d;
}

static void music_dir(char *out, size_t n);
/* The music directory (SoundFonts live in its soundfonts/, soundfont.c). */
void music_pack_dir(char *out, size_t n) { music_dir(out, n); }

static void music_dir(char *out, size_t n) {
    const char *v = hal_setting_str ? hal_setting_str("TOKI_MUSIC_DIR", NULL) : getenv("TOKI_MUSIC_DIR");
    const PcSettings *s = settings();
    if (v && *v) snprintf(out, n, "%s", v);
    else if (s && s->data_dir[0]) snprintf(out, n, "%s/music", s->data_dir);
    else snprintf(out, n, "music");
}

static int path_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static void mkdirs(const char *dir) {
    char tmp[1024];
    size_t i;
    snprintf(tmp, sizeof tmp, "%s", dir);
    for (i = 1; tmp[i]; i++) {
        if (PC_MKDIR_SPLIT(tmp + i)) {
            char c = tmp[i];
            tmp[i] = 0;
            mkdir(tmp, 0755);
            tmp[i] = c;
        }
    }
    mkdir(tmp, 0755);
}

/* ---- the event log ---------------------------------------------------- */

static FILE *g_log;
static int g_log_init;
static char g_log_path[1024];

static FILE *event_log(void) {
    if (!g_log_init) {
        const char *p = getenv("TOKI_MUSIC_LOG");
        g_log_init = 1;
        if (p && *p) snprintf(g_log_path, sizeof g_log_path, "%s", p);
        else if (dump_on()) {
            char dir[900];
            music_dir(dir, sizeof dir);
            snprintf(g_log_path, sizeof g_log_path, "%s/dump/heard.jsonl", dir);
            mkdirs(dir);
            {
                char d2[1000];
                snprintf(d2, sizeof d2, "%s/dump", dir);
                mkdirs(d2);
            }
        }
        if (g_log_path[0]) g_log = fopen(g_log_path, "a");
    }
    return g_log;
}

/* The song events in the player log ([MUSIC] lines on stdout, i.e.
 * tokimeki-pc.log), in both profiles, so a player's log shows what played:
 * TOKI_MUSIC_EVENTS=1 (the default) one line per start / stop / end / close
 * / pause / resume; 2 also every open and fade step (vol, mvol); 0 none.
 * Observing changes nothing: with no pack and no dump, song_start opens no
 * stream and the split stays off. */
static int events_level(void) {
    static int level = -1;
    if (level < 0) {
        const char *e = getenv("TOKI_MUSIC_EVENTS");
        level = e && *e ? atoi(e) : 1;
        if (level < 0) level = 0;
    }
    return level;
}

static void event(const char *ev, int acc, const char *id, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
#include <stdarg.h>
static void event(const char *ev, int acc, const char *id, const char *fmt, ...) {
    char extra[600];
    va_list ap;
    FILE *f;
    unsigned vbl = hal_clock_vblanks ? hal_clock_vblanks() : 0;
    long frame = &frame_counter ? (long)frame_counter : -1;
    const char *ovl = hal_overlay_loaded ? hal_overlay_loaded() : NULL;
    extra[0] = 0;
    if (fmt) {
        va_start(ap, fmt);
        vsnprintf(extra, sizeof extra, fmt, ap);
        va_end(ap);
    }
    /* the player log (events_level, above): song events; the per-frame
     * volume steps of a fade (vol, mvol) and the open that precedes every
     * start only at level 2 */
    if (events_level() >= 2 ||
        (events_level() == 1 && strcmp(ev, "vol") && strcmp(ev, "mvol") && strcmp(ev, "open"))) {
        printf("[MUSIC] vbl %u %s acc %d %s step %d/%d %s%s%s\n", vbl, ev, acc, id ? id : "-",
               &now_step ? now_step : -1, &now_sub_step ? now_sub_step : -1,
               ovl && *ovl ? ovl : "-", extra[0] ? " " : "", extra);
        fflush(stdout);
    }
    f = event_log();
    if (!f) return;
    fprintf(f, "{\"ev\":\"%s\",\"vbl\":%u,\"frame\":%ld,\"step\":%d,\"sub\":%d,\"overlay\":\"%s\","
            "\"acc\":%d,\"id\":\"%s\"%s%s}\n",
            ev, vbl, frame, &now_step ? now_step : -1, &now_sub_step ? now_sub_step : -1,
            ovl && *ovl ? ovl : "-", acc, id ? id : "", extra[0] ? "," : "", extra);
    fflush(f);
}

/* ---- the replacement stream (mixer side under the audio lock) -------- */

typedef struct {
    MusicDecoder *dec;
    MusicFileInfo info;
    char id[16];
    int acc;
    int64_t loop_start, loop_end;   /* source frames; loop_end exclusive, -1 = end of file */
    int loop_allowed;               /* the sidecar's `loop = false` turns looping off */
    float gain_file;                /* the sidecar's gain_db */
    /* the decoder side (game thread only: stream_fill) */
    int16_t buf[1024 * 2];
    uint64_t src_pos;               /* source frames read so far */
    /* the source ring: the game thread decodes ahead into it (stream_fill,
     * with the loop points), the audio thread only reads it -- no file I/O
     * on the audio thread (STATE-MACHINES.md C5).  w under the lock by the
     * game thread, r by the mixer. */
    int16_t ring[SRC_RING * 2];
    uint32_t ring_pos[SRC_RING];    /* each ring frame's position in the file */
    uint32_t ring_r, ring_w;
    int eof;                        /* the decoder is done (no loop): finish at the ring's end */
    unsigned underruns;             /* the ring ran dry before the file ended */
    /* resampler (audio thread) */
    float a[2], b[2];
    double frac, step;
    int primed;
    /* set by the game thread */
    int may_loop;                   /* the sequence is still playing */
    int paused;
    int stopping;
    float target[2];                /* sequence-volume x player gains */
    /* mixer state */
    float cur[2];
    float env;                      /* 0..1 pause / stop envelope */
    int finished;                   /* the file ran out */
    unsigned loops;                 /* loop jumps made (reported by the game thread) */
    unsigned loops_reported;
    int done;                       /* stopped and silent: the game thread frees it */
} Stream;

static Stream *g_stream;            /* the current song's file */
static Stream *g_fading;            /* the previous one, ramping out */

/* Game thread: up to 1,024 source frames into s->buf, following the loop
 * points while the song plays; 0 at the end of the file. */
static int decode_chunk(Stream *s) {
    int want = 1024, n, tries;
    for (tries = 0; tries < 2; tries++) {
        int64_t end = (s->may_loop && s->loop_allowed) ? s->loop_end : -1;
        if (end > 0) {
            if ((int64_t)s->src_pos >= end) {
                if (music_decoder_seek(s->dec, (uint64_t)s->loop_start)) return 0;
                s->src_pos = (uint64_t)s->loop_start;
                s->loops++;
            }
            if (end - (int64_t)s->src_pos < want) want = (int)(end - (int64_t)s->src_pos);
        }
        n = music_decoder_read(s->dec, s->buf, want);
        if (n > 0) {
            s->src_pos += (uint64_t)n;
            return n;
        }
        /* end of the file: loop to the loop start while the song plays */
        if (!(s->may_loop && s->loop_allowed) || music_decoder_seek(s->dec, (uint64_t)s->loop_start))
            return 0;
        s->src_pos = (uint64_t)s->loop_start;
        s->loops++;
    }
    return 0;
}

/* Game thread (stream_open, every VBlank): top the source ring up. */
static void stream_fill(Stream *s) {
    uint32_t room;
    if (!s || s->eof) return;
    spu_hal_audio_lock();
    room = SRC_RING - (s->ring_w - s->ring_r);
    spu_hal_audio_unlock();
    while (room >= 1024) {
        int n = decode_chunk(s), i;
        if (n <= 0) {
            spu_hal_audio_lock();
            s->eof = 1;
            spu_hal_audio_unlock();
            return;
        }
        spu_hal_audio_lock();
        for (i = 0; i < n; i++) {
            uint32_t k = (s->ring_w + (uint32_t)i) % SRC_RING;
            s->ring[k * 2] = s->buf[i * 2];
            s->ring[k * 2 + 1] = s->buf[i * 2 + 1];
            s->ring_pos[k] = (uint32_t)(s->src_pos - (uint64_t)n + (uint64_t)i);
        }
        s->ring_w += (uint32_t)n;
        spu_hal_audio_unlock();
        room -= (uint32_t)n;
    }
}

/* Game thread: the song ended by itself (no more looping): what was decoded
 * ahead with the loop is dropped and decoding resumes from the frame the
 * mixer plays next, so the file plays out from there to its end. */
static void stream_replan(Stream *s) {
    uint64_t p;
    if (!s) return;
    spu_hal_audio_lock();
    if (s->ring_r != s->ring_w) {
        p = s->ring_pos[s->ring_r % SRC_RING];
        s->ring_w = s->ring_r;
    } else {
        p = s->src_pos;
    }
    s->eof = 0;
    spu_hal_audio_unlock();
    if (music_decoder_seek(s->dec, p) == 0) s->src_pos = p;
    stream_fill(s);
}

/* Audio thread: the next source frame from the ring. */
static int src_next(Stream *s, float out[2]) {
    uint32_t k;
    if (s->ring_r == s->ring_w) {
        if (s->eof) {
            s->finished = 1;
            return 0;
        }
        s->underruns++;                 /* the game thread fell behind: silence, not the end */
        out[0] = out[1] = 0.0f;
        return 1;
    }
    k = s->ring_r % SRC_RING;
    out[0] = s->ring[k * 2] / 32768.0f;
    out[1] = s->ring[k * 2 + 1] / 32768.0f;
    s->ring_r++;
    return 1;
}

static float g_main_gain[2] = { 1.0f, 1.0f };

static void stream_mix(Stream *s, int16_t *out, int frames) {
    int i;
    const float env_step = 1.0f / RAMP_FRAMES;
    if (!s || s->done) return;
    for (i = 0; i < frames; i++) {
        float want = (s->paused || s->stopping) ? 0.0f : 1.0f;
        float smp[2];
        int c;
        if (s->env < want) { s->env += env_step; if (s->env > want) s->env = want; }
        else if (s->env > want) { s->env -= env_step; if (s->env < want) s->env = want; }
        if (s->env <= 0.0f && want <= 0.0f) {
            if (s->stopping) s->done = 1;
            if (s->stopping || s->paused) return;   /* paused: the position is held */
        }
        if (s->finished) { s->done = s->stopping || s->finished; return; }
        for (c = 0; c < 2; c++) s->cur[c] += (s->target[c] - s->cur[c]) * (1.0f / 256.0f);
        if (!s->primed) {
            if (!src_next(s, s->a) || !src_next(s, s->b)) return;
            s->primed = 1;
            s->frac = 0.0;
        }
        for (c = 0; c < 2; c++) smp[c] = s->a[c] + (s->b[c] - s->a[c]) * (float)s->frac;
        s->frac += s->step;
        while (s->frac >= 1.0) {
            s->frac -= 1.0;
            s->a[0] = s->b[0];
            s->a[1] = s->b[1];
            if (!src_next(s, s->b)) { s->b[0] = s->b[1] = 0.0f; break; }
        }
        for (c = 0; c < 2; c++) {
            float v = out[i * 2 + c] + smp[c] * s->cur[c] * s->env * g_main_gain[c] * 32767.0f;
            out[i * 2 + c] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (int)v);
        }
    }
}

/* ---- the dump --------------------------------------------------------- */

static FILE *g_dump;                 /* game thread only */
static uint32_t g_dump_frames, g_dump_limit;
static char g_dump_part[1100], g_dump_final[1100];
/* The audio thread hands the song's frames over through a ring (no file I/O
 * on it, STATE-MACHINES.md C5); the game thread writes them (dump_drain). */
static int16_t g_dring[DUMP_RING * 2];
static uint32_t g_dring_r, g_dring_w;  /* under the audio lock */
static int g_dump_active;              /* the mixer feeds the ring */
static uint32_t g_dump_lost;

static void wav_header(FILE *f, uint32_t frames) {
    uint32_t data = frames * 4, riff = data + 36, fmt = 16, rate = OUT_RATE, br = OUT_RATE * 4;
    uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&align, 2, 1, f);
    fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
}

static uint32_t wav_frames(const char *path) {
    FILE *f = fopen(path, "rb");
    uint32_t data = 0;
    if (!f) return 0;
    if (fseek(f, 40, SEEK_SET) == 0 && fread(&data, 4, 1, f) != 1) data = 0;
    fclose(f);
    return data / 4;
}

/* Game thread, not holding the audio lock: the ring into the file. */
static void dump_drain(void) {
    static int16_t tmp[4096 * 2];
    for (;;) {
        uint32_t n, i;
        spu_hal_audio_lock();
        n = g_dring_w - g_dring_r;
        if (n > 4096) n = 4096;
        for (i = 0; i < n; i++) {
            uint32_t k = (g_dring_r + i) % DUMP_RING;
            tmp[i * 2] = g_dring[k * 2];
            tmp[i * 2 + 1] = g_dring[k * 2 + 1];
        }
        g_dring_r += n;
        spu_hal_audio_unlock();
        if (!n) return;
        if (g_dump) {
            uint32_t m = g_dump_frames + n > g_dump_limit ? g_dump_limit - g_dump_frames : n;
            if (m) fwrite(tmp, 4, m, g_dump);
            g_dump_frames += m;
        }
    }
}

/* Game thread, not holding the audio lock: stop feeding, write what is
 * left, finish the file. */
static void dump_close(void) {
    FILE *f = g_dump;
    if (!f) return;
    spu_hal_audio_lock();
    g_dump_active = 0;
    spu_hal_audio_unlock();
    dump_drain();
    g_dump = NULL;
    if (g_dump_lost) printf("[MUSIC] dump: %u frames lost (the game thread fell behind)\n", g_dump_lost);
    g_dump_lost = 0;
    wav_header(f, g_dump_frames);
    fclose(f);
    if (g_dump_frames > wav_frames(g_dump_final)) {
        rename(g_dump_part, g_dump_final);
        printf("[MUSIC] dump %s: %.1f s\n", g_dump_final, g_dump_frames / (double)OUT_RATE);
    } else {
        remove(g_dump_part);
    }
}

/* ---- per-sequence state (game thread) --------------------------------- */

typedef struct {
    int open;
    u32 handle;            /* the score's +8 (data after the header) */
    u32 seq_addr;          /* PS1 address of 'pQES' */
    u32 seq_len;
    char id[16];
    u32 flags;
    s16 vol_l, vol_r;
    s16 start_l, start_r;  /* the volume when it started playing */
    int vab;
} Acc;

static Acc g_acc[MAX_ACC];
static int g_cur = -1;               /* the sequence whose voices the split masks */
static int g_split_on, g_split_replace;
static u32 g_core_mask;
static int g_core_on = -1, g_core_replace = -1;
static int g_dump_acc = -1;
static int g_dump_end_vbl = -1;      /* VBlanks of release tail left after the song ended */
static char g_cur_pack[64];
static int g_engaged;                /* anything configured at all this tick */
static int16_t g_mainvol_last[2] = { -1, -1 };

static int locate_seq(u32 handle, u32 *addr) {
    u32 h = RAM_BASE | (handle & RAM_MASK), a;
    for (a = h - 15; a + 32 >= h && a >= h - 32; a--) {
        const u8 *p = PS1_PTR(a);
        if (p[0] == 'p' && p[1] == 'Q' && p[2] == 'E' && p[3] == 'S') {
            *addr = a;
            return 0;
        }
    }
    return -1;
}

static void read_sidecar(const char *path, Stream *s) {
    FILE *f = fopen(path, "r");
    char line[256];
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *k = line, *v, *e;
        while (*k == ' ' || *k == '\t') k++;
        if (*k == '#' || *k == ';' || *k == '[') continue;
        v = strchr(k, '=');
        if (!v) continue;
        *v++ = 0;
        for (e = v + strlen(v); e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '); e--) e[-1] = 0;
        for (e = k + strlen(k); e > k && (e[-1] == ' ' || e[-1] == '\t'); e--) e[-1] = 0;
        while (*v == ' ' || *v == '\t') v++;
        if (!strcasecmp(k, "loop_start")) s->loop_start = strtoll(v, NULL, 10);
        else if (!strcasecmp(k, "loop_end")) s->loop_end = strtoll(v, NULL, 10);
        else if (!strcasecmp(k, "loop")) s->loop_allowed = !(!strcasecmp(v, "false") || !strcmp(v, "0") || !strcasecmp(v, "no"));
        else if (!strcasecmp(k, "gain_db")) s->gain_file = powf(10.0f, (float)atof(v) / 20.0f);
    }
    fclose(f);
}

static Stream *stream_open(const char *pack, const char *id, int acc) {
    static const char *const exts[] = { "ogg", "flac", "wav" };
    char dir[1024], path[1200];
    unsigned i;
    Stream *s;
    if (!pack[0]) return NULL;
    music_dir(dir, sizeof dir);
    for (i = 0; i < 3; i++) {
        snprintf(path, sizeof path, "%s/packs/%s/%s.%s", dir, pack, id, exts[i]);
        if (path_exists(path)) break;
    }
    if (i == 3) return NULL;
    s = (Stream *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->dec = music_decoder_open(path, &s->info);
    if (!s->dec) {
        event("file_error", acc, id, "\"file\":\"%s\"", path);
        free(s);
        return NULL;
    }
    snprintf(s->id, sizeof s->id, "%s", id);
    s->acc = acc;
    s->loop_allowed = 1;
    s->gain_file = 1.0f;
    s->loop_start = s->info.loop_start >= 0 ? s->info.loop_start : 0;
    s->loop_end = s->info.loop_end > 0 ? s->info.loop_end : -1;
    snprintf(path + strlen(path) - strlen(exts[i]) - 1, 8, ".ini");
    read_sidecar(path, s);
    if (s->info.frames && (s->loop_end < 0 || (uint64_t)s->loop_end > s->info.frames))
        s->loop_end = (int64_t)s->info.frames;
    if (s->loop_start < 0 || (s->loop_end > 0 && s->loop_start >= s->loop_end)) s->loop_start = 0;
    s->step = (double)s->info.rate / OUT_RATE;
    s->may_loop = 1;
    snprintf(path + strlen(path) - 4, 8, ".%s", exts[i]);
    event("file", acc, id, "\"file\":\"%s\",\"rate\":%u,\"channels\":%d,\"frames\":%llu,"
          "\"loop_start\":%lld,\"loop_end\":%lld,\"loop\":%d",
          path, (unsigned)s->info.rate, s->info.channels, (unsigned long long)s->info.frames,
          (long long)s->loop_start, (long long)s->loop_end, s->loop_allowed);
    return s;
}

static float player_music_gain(void) {
    const PcSettings *s = settings();
    if (!s) return 1.0f;
    return (float)s->vol_master * (float)s->vol_music / 10000.0f;
}

static void stream_targets_locked(Stream *s, const Acc *a) {
    float pg = player_music_gain() * s->gain_file;
    float rl = a->start_l > 0 ? (float)a->start_l : 127.0f;
    float rr = a->start_r > 0 ? (float)a->start_r : 127.0f;
    float gl = a->vol_l / rl, gr = a->vol_r / rr;
    if (gl < 0) gl = 0;
    if (gl > 2) gl = 2;
    if (gr < 0) gr = 0;
    if (gr > 2) gr = 2;
    s->target[0] = gl * pg;
    s->target[1] = gr * pg;
}

/* caller holds the lock; returns a stream to close after unlocking */
static Stream *stream_stop_locked(void) {
    Stream *dead = NULL;
    if (!g_stream) return NULL;
    if (g_fading) dead = g_fading;
    g_stream->stopping = 1;
    g_fading = g_stream;
    g_stream = NULL;
    return dead;
}

static void stream_free(Stream *s) {
    if (!s) return;
    music_decoder_close(s->dec);
    free(s);
}

static void free_done(void) {
    Stream *dead = NULL;
    spu_hal_audio_lock();
    if (g_fading && g_fading->done) { dead = g_fading; g_fading = NULL; }
    spu_hal_audio_unlock();
    if (dead) {
        music_decoder_close(dead->dec);
        free(dead);
    }
}

static void song_start(int acc) {
    Acc *a = &g_acc[acc];
    const char *pack = pack_name();
    int want_pack = hal_model_on("MUSIC_PACK") && pack[0];
    Stream *s = want_pack ? stream_open(pack, a->id, acc) : NULL;
    int sf = soundfont_rendering && soundfont_rendering();
    int dump = dump_on() && !sf;
    FILE *df = NULL;
    Stream *dead;
    char part[1100], final_[1100];
    a->start_l = a->vol_l;
    a->start_r = a->vol_r;
    event("start", acc, a->id, "\"addr\":\"0x%08X\",\"bytes\":%u,\"vab\":%d,\"vol\":[%d,%d],"
          "\"pack\":\"%s\",\"file\":%s,\"soundfont\":%s",
          (unsigned)a->seq_addr, (unsigned)a->seq_len, a->vab, a->vol_l, a->vol_r,
          want_pack ? pack : "", s ? "true" : "false", sf && !s ? "true" : "false");
    if (sf && dump_on()) event("dump_suspended", acc, a->id, NULL);
    if (dump) {
        char dir[1000];
        music_dir(dir, sizeof dir);
        snprintf(final_, sizeof final_, "%s/dump", dir);
        mkdirs(final_);
        snprintf(final_, sizeof final_, "%s/dump/%s.wav", dir, a->id);
        snprintf(part, sizeof part, "%s/dump/%s.wav.part", dir, a->id);
        df = fopen(part, "wb");
        if (df) wav_header(df, 0);
    }
    dump_close();
    if (df) {
        g_dump = df;
        g_dump_frames = 0;
        g_dump_limit = (uint32_t)(hal_setting_long ? hal_setting_long("TOKI_MUSIC_DUMP_SECONDS", 600) : 600) * OUT_RATE;
        snprintf(g_dump_part, sizeof g_dump_part, "%s", part);
        snprintf(g_dump_final, sizeof g_dump_final, "%s", final_);
        g_dump_acc = acc;
        g_dump_end_vbl = -1;
    }
    if (s) stream_fill(s);           /* decoded ahead before it plays */
    spu_hal_audio_lock();
    dead = stream_stop_locked();
    if (s) {
        stream_targets_locked(s, a);
        s->cur[0] = s->target[0];
        s->cur[1] = s->target[1];
        s->env = 1.0f;               /* in step with the first sequencer tick: no fade-in */
        g_stream = s;
    }
    g_dring_r = g_dring_w = 0;
    g_dump_active = df != NULL;
    g_cur = acc;
    g_split_on = s != NULL || df != NULL;
    g_split_replace = s != NULL;
    snprintf(g_cur_pack, sizeof g_cur_pack, "%s", want_pack ? pack : "");
    spu_hal_audio_unlock();
    stream_free(dead);
}

/* The song on `acc` stopped (SsSeqStop / SsSeqClose / a new song). */
static void song_stop(int acc) {
    Stream *dead;
    if (acc != g_cur) return;
    if (g_stream) event("file_stop", acc, g_stream->id, NULL);   /* g_stream: the game thread's */
    spu_hal_audio_lock();
    dead = stream_stop_locked();
    g_cur = -1;
    g_split_on = 0;
    g_split_replace = 0;
    spu_hal_audio_unlock();
    stream_free(dead);
    if (g_dump_acc == acc) { dump_close(); g_dump_acc = -1; }
}

/* A PLAY edge: the song at the handle may not be the one opened there (a
 * SEQ in another bank loaded at the same address, or a state load that
 * restored libsnd under this kept observer): identify it again by its
 * bytes (STATE-MACHINES.md C4). */
static void reidentify(int acc, Acc *a, const u8 *rec, u32 handle) {
    u32 addr, room;
    char id[16];
    if (locate_seq(handle, &addr)) return;
    room = (RAM_BASE | (RAM_MASK + 1)) - addr;
    if (music_pack_track_id(PS1_PTR(addr), room, id, sizeof id) || !strcmp(id, a->id)) return;
    event("close", acc, a->id, NULL);
    snprintf(a->id, sizeof a->id, "%s", id);
    a->seq_addr = addr;
    a->seq_len = music_pack_seq_span(PS1_PTR(addr), room);
    a->vab = *(const s16 *)(rec + SCORE_VAB_ID);
    event("open", acc, a->id, "\"addr\":\"0x%08X\",\"bytes\":%u,\"vab\":%d",
          (unsigned)a->seq_addr, (unsigned)a->seq_len, a->vab);
}

/* After a quickload, a toolkit load or a rewind (dbg_state_after_restore):
 * libsnd is the restored one, this observer is the session's.  The dump's
 * take ends here; on the next VBlank every open sequence is identified
 * again by its bytes (g_reverify, observe): a different song at the same
 * handle closes the old one (and its pack file), the same song still
 * playing keeps its file (a rewind does not restart the music).
 * STATE-MACHINES.md C4. */
static int g_reverify;
void music_pack_after_load(void) {
    if (g_dump_acc >= 0) {
        dump_close();
        g_dump_acc = -1;
        g_dump_end_vbl = -1;
    }
    g_reverify = 1;
}

static void observe(void) {
    int acc;
    u32 open = (u32)_snd_openflag;
    if (g_reverify) {
        g_reverify = 0;
        for (acc = 0; acc < MAX_ACC; acc++) {
            Acc *a = &g_acc[acc];
            u32 h = (open >> acc) & 1u ? _ss_score[acc] : 0;
            const u8 *rec = h ? PS1_PTR(h) : NULL;
            u32 handle = rec ? *(const u32 *)(rec + SCORE_LOOP_START) : 0;
            char was[16];
            if (!a->open || !rec || handle != a->handle) continue;   /* the loop below sees it */
            snprintf(was, sizeof was, "%s", a->id);
            reidentify(acc, a, rec, handle);
            if (strcmp(was, a->id)) {
                song_stop(acc);
                a->flags = 0;                /* the restored flags are the new song's edges */
            }
        }
    }
    for (acc = 0; acc < MAX_ACC; acc++) {
        Acc *a = &g_acc[acc];
        u32 h = (open >> acc) & 1u ? _ss_score[acc] : 0;
        const u8 *rec = h ? PS1_PTR(h) : NULL;
        u32 handle = rec ? *(const u32 *)(rec + SCORE_LOOP_START) : 0;
        u32 flags;
        s16 vl, vr;
        if (!rec || !handle) {
            if (a->open) {
                event("close", acc, a->id, NULL);
                song_stop(acc);
                a->open = 0;
            }
            continue;
        }
        if (!a->open || handle != a->handle) {
            u32 addr;
            if (a->open) {                      /* reopened with another song */
                event("close", acc, a->id, NULL);
                song_stop(acc);
            }
            memset(a, 0, sizeof *a);
            a->open = 1;
            a->handle = handle;
            if (locate_seq(handle, &addr) == 0) {
                u32 room = (RAM_BASE | (RAM_MASK + 1)) - addr;
                a->seq_addr = addr;
                a->seq_len = music_pack_seq_span(PS1_PTR(addr), room);
                if (music_pack_track_id(PS1_PTR(addr), room, a->id, sizeof a->id))
                    snprintf(a->id, sizeof a->id, "unparsed");
            } else {
                snprintf(a->id, sizeof a->id, "unknown");
            }
            a->vab = *(const s16 *)(rec + SCORE_VAB_ID);
            a->vol_l = *(const s16 *)(rec + SCORE_VOL_L);
            a->vol_r = *(const s16 *)(rec + SCORE_VOL_R);
            event("open", acc, a->id, "\"addr\":\"0x%08X\",\"bytes\":%u,\"vab\":%d",
                  (unsigned)a->seq_addr, (unsigned)a->seq_len, a->vab);
        }
        flags = *(const u32 *)(rec + SCORE_FLAGS);
        vl = *(const s16 *)(rec + SCORE_VOL_L);
        vr = *(const s16 *)(rec + SCORE_VOL_R);
        if (vl != a->vol_l || vr != a->vol_r) {
            a->vol_l = vl;
            a->vol_r = vr;
            if (acc == g_cur) {
                event("vol", acc, a->id, "\"vol\":[%d,%d]", vl, vr);
                spu_hal_audio_lock();
                if (g_stream) stream_targets_locked(g_stream, a);
                spu_hal_audio_unlock();
            }
        }
        if (flags != a->flags) {
            u32 was = a->flags;
            a->flags = flags;
            if ((flags & FLAG_PLAY) && !(was & FLAG_PLAY)) {
                if ((was & FLAG_PAUSE) && acc == g_cur) {
                    event("resume", acc, a->id, NULL);
                    spu_hal_audio_lock();
                    if (g_stream) g_stream->paused = 0;
                    spu_hal_audio_unlock();
                } else {
                    reidentify(acc, a, rec, handle);
                    song_start(acc);
                }
            } else if ((flags & FLAG_PAUSE) && !(was & FLAG_PAUSE) && acc == g_cur) {
                event("pause", acc, a->id, NULL);
                spu_hal_audio_lock();
                if (g_stream) g_stream->paused = 1;
                spu_hal_audio_unlock();
            } else if ((flags & FLAG_STOP) && !(was & FLAG_STOP) && acc == g_cur) {
                if (flags & FLAG_ENDED) {
                    /* the song reached its end by itself: the file plays out */
                    event("end", acc, a->id, NULL);
                    spu_hal_audio_lock();
                    if (g_stream) g_stream->may_loop = 0;
                    spu_hal_audio_unlock();
                    if (g_stream && !g_stream->may_loop) stream_replan(g_stream);
                    if (g_dump_acc == acc) g_dump_end_vbl = DUMP_TAIL_VBLANKS;
                } else {
                    event("stop", acc, a->id, NULL);
                    song_stop(acc);
                }
            }
        }
    }
}

static void main_volume(void) {
    int16_t l = (int16_t)spu_psx_read_reg(0x1F801DB8), r = (int16_t)spu_psx_read_reg(0x1F801DBA);
    if (g_cur >= 0 && (l != g_mainvol_last[0] || r != g_mainvol_last[1]) && g_stream)
        event("mvol", g_cur, g_acc[g_cur].id, "\"mvol\":[%d,%d]", l, r);
    g_mainvol_last[0] = l;
    g_mainvol_last[1] = r;
}

/* The run ends (PC_TRACE_FRAMES, quit): finish the dump being written. */
void music_pack_flush(void) {
    dump_close();
}

void music_pack_tick(void) {
    const char *pack = pack_name();
    int engaged = (hal_model_on("MUSIC_PACK") && pack[0]) || dump_on() || getenv("TOKI_MUSIC_LOG") ||
                  events_level() > 0;
    if (!engaged && !g_engaged) return;
    if (!engaged) {                      /* switched off (settings menu): back to the original */
        int acc;
        if (g_cur >= 0) song_stop(g_cur);
        for (acc = 0; acc < MAX_ACC; acc++) g_acc[acc].open = 0;
        g_engaged = 0;
        return;
    }
    if (!g_engaged) {
        static int registered;
        if (!registered) { registered = 1; atexit(music_pack_flush); }
    }
    g_engaged = 1;
    observe();
    /* the pack changed in the settings menu: restart the current song's lookup */
    if (g_cur >= 0 && strcmp(g_cur_pack, hal_model_on("MUSIC_PACK") ? pack : "") &&
        (g_acc[g_cur].flags & FLAG_PLAY)) {
        int acc = g_cur;
        song_stop(acc);
        song_start(acc);
    }
    if (g_dump_end_vbl > 0 && --g_dump_end_vbl == 0) {
        dump_close();
        g_dump_acc = -1;
        spu_hal_audio_lock();
        if (!g_stream && g_cur >= 0) {
            g_split_on = 0;
            g_cur = -1;
        }
        spu_hal_audio_unlock();
    }
    dump_drain();
    if (g_dump && g_dump_frames >= g_dump_limit) dump_close();
    stream_fill(g_stream);
    main_volume();                       /* logs; reads the core's registers */
    {
        unsigned loops = 0;
        int ended = 0;
        spu_hal_audio_lock();
        if (g_stream) {
            stream_targets_locked(g_stream, &g_acc[g_stream->acc]);
            if (g_stream->loops != g_stream->loops_reported) loops = g_stream->loops_reported = g_stream->loops;
            if (g_stream->finished && !g_stream->stopping) {
                ended = 1;
                g_stream->stopping = 1;
            }
        }
        music_pack_voice_sync();
        spu_hal_audio_unlock();
        /* the log after the lock (file I/O) */
        if (loops) event("file_loop", g_stream->acc, g_stream->id, "\"loops\":%u", loops);
        if (ended) event("file_end", g_stream ? g_stream->acc : -1, g_stream ? g_stream->id : "-", NULL);
    }
    free_done();
}

/* The core's split is the union of this module's (a pack's song, the dump)
 * and the SoundFont's (soundfont.c: the voices whose last key-on it plays,
 * updated when the core takes the key write).  Audio lock held. */
static u32 g_pack_mask;
static void split_apply_locked(void) {
    int sf = soundfont_split_on && soundfont_split_on();
    int on = g_split_on || sf, replace = g_split_replace || sf;
    u32 mask = g_pack_mask | (sf ? soundfont_split_mask() : 0u);
    if (on != g_core_on || replace != g_core_replace || mask != g_core_mask) {
        if (on || g_core_on > 0) spu_psx_music_split(on, mask, replace);
        g_core_on = on;
        g_core_replace = replace;
        g_core_mask = mask;
    }
}

/* audio lock held */
void music_pack_voice_sync(void) {
    u32 mask = 0;
    int v;
    if (g_split_on && g_cur >= 0) {
        for (v = 0; v < NUM_VOICES; v++) {
            s16 owner = *(const s16 *)(_svm_voice + v * VOICE_STRIDE + VOICE_OWNER);
            if (owner != -1 && (owner & 0xFF) == g_cur) mask |= 1u << v;
        }
    }
    g_pack_mask = mask;
    split_apply_locked();
}

/* audio lock held (soundfont.c, after a key write changed its voices) */
void music_pack_split_refresh(void) { split_apply_locked(); }

/* The song id of sequence `acc` while it is open, else NULL (soundfont.c's
 * per-song map sections).  Read from libsnd's score when this module has
 * not observed it yet: a song's first notes are keyed by the sequencer tick
 * of the VBlank that opened it, before music_pack_tick runs.  Game thread. */
const char *music_pack_acc_id(int acc) {
    static char id[16];
    const u8 *rec;
    u32 h, handle, addr;
    if (acc < 0 || acc >= MAX_ACC || !(((u32)_snd_openflag >> acc) & 1u)) return NULL;
    h = _ss_score[acc];
    rec = h ? PS1_PTR(h) : NULL;
    handle = rec ? *(const u32 *)(rec + SCORE_LOOP_START) : 0;
    if (!handle) return NULL;
    if (g_acc[acc].open && g_acc[acc].handle == handle) return g_acc[acc].id;
    if (locate_seq(handle, &addr) ||
        music_pack_track_id(PS1_PTR(addr), (RAM_BASE | (RAM_MASK + 1)) - addr, id, sizeof id))
        return NULL;
    return id;
}

/* The sequence whose voices a pack file replaces, -1 none (the SoundFont
 * leaves it to the pack).  Game thread, audio lock held. */
int music_pack_replaced_acc(void) { return g_split_replace && g_cur >= 0 ? g_cur : -1; }

int music_pack_mixing(void) { return g_core_on > 0 || g_stream || g_fading; }
int music_pack_replacing(void) { return g_core_on > 0 && g_core_replace > 0; }

void music_pack_mix(int16_t *out, const int16_t *music, int frames) {
    int16_t l = (int16_t)spu_psx_read_reg(0x1F801DB8), r = (int16_t)spu_psx_read_reg(0x1F801DBA);
    g_main_gain[0] = l / 32767.0f;
    g_main_gain[1] = r / 32767.0f;
    if (g_dump_active && music) {
        uint32_t room = DUMP_RING - (g_dring_w - g_dring_r), n = (uint32_t)frames, i;
        if (n > room) { g_dump_lost += n - room; n = room; }
        for (i = 0; i < n; i++) {
            uint32_t k = (g_dring_w + i) % DUMP_RING;
            g_dring[k * 2] = music[i * 2];
            g_dring[k * 2 + 1] = music[i * 2 + 1];
        }
        g_dring_w += n;
    }
    stream_mix(g_fading, out, frames);
    stream_mix(g_stream, out, frames);
}

/* ---- the settings menu ------------------------------------------------ */

#define MAX_PACKS 24
static char g_pack_names[MAX_PACKS][64];
static const char *g_pack_choice[MAX_PACKS + 2];
static int g_pack_count;

static int get_pack(void *ctx) {
    const PcSettings *s = settings();
    int i;
    (void)ctx;
    if (!s || !s->music_pack[0]) return 0;
    for (i = 0; i < g_pack_count; i++)
        if (!strcmp(g_pack_names[i], s->music_pack)) return i + 1;
    return 0;
}
static void set_pack(void *ctx, int v) {
    PcSettings *s = pc_settings_mut ? pc_settings_mut() : NULL;
    (void)ctx;
    if (!s) return;
    snprintf(s->music_pack, sizeof s->music_pack, "%s", v > 0 && v <= g_pack_count ? g_pack_names[v - 1] : "");
}
static const char *pack_note(void *ctx) {
    const char *e = getenv("TOKI_MUSIC_PACK");
    (void)ctx;
    if (e && *e) return "ENV OVERRIDE";
    if (!hal_model_on("MUSIC_PACK")) return "ENHANCED ONLY";
    return g_pack_count ? "" : "NO PACKS FOUND";
}
static int get_dump(void *ctx) { const PcSettings *s = settings(); (void)ctx; return s ? s->music_dump : 0; }
static void set_dump(void *ctx, int v) {
    PcSettings *s = pc_settings_mut ? pc_settings_mut() : NULL;
    (void)ctx;
    if (s) s->music_dump = v != 0;
}
static const char *dump_note(void *ctx) { (void)ctx; return "TO MUSIC/DUMP"; }

static int name_cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

extern void soundfont_menu_register(int page) __attribute__((weak));
extern void spu_quality_menu_register(void) __attribute__((weak));

void music_pack_menu_register(void) {
    static int done;
    char dir[1024];
    DIR *d;
    struct dirent *e;
    PcMenuItem it;
    int page, i;
    if (done) return;
    done = 1;
    music_dir(dir, sizeof dir);
    strncat(dir, "/packs", sizeof dir - strlen(dir) - 1);
    g_pack_count = 0;
    if ((d = opendir(dir)) != NULL) {
        while ((e = readdir(d)) != NULL && g_pack_count < MAX_PACKS) {
            char p[1300];
            struct stat st;
            if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof g_pack_names[0]) continue;
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            if (stat(p, &st) || !S_ISDIR(st.st_mode)) continue;
            snprintf(g_pack_names[g_pack_count++], sizeof g_pack_names[0], "%s", e->d_name);
        }
        closedir(d);
    }
    qsort(g_pack_names, (size_t)g_pack_count, sizeof g_pack_names[0], name_cmp);
    g_pack_choice[0] = "OFF (ORIGINAL)";
    for (i = 0; i < g_pack_count; i++) g_pack_choice[i + 1] = g_pack_names[i];
    g_pack_choice[g_pack_count + 1] = NULL;

    page = pc_menu_page("MUSIC PACKS");
    if (page < 0) return;
    memset(&it, 0, sizeof it);
    it.type = PC_MENU_PAGE;
    it.label = "MUSIC PACKS";
    it.target = page;
    pc_menu_add(&it);
    memset(&it, 0, sizeof it);
    it.page = page;
    it.type = PC_MENU_CHOICE;
    it.label = "PACK";
    it.key = "audio.music_pack";
    it.choices = g_pack_choice;
    it.get = get_pack;
    it.set = set_pack;
    it.note = pack_note;
    pc_menu_add(&it);
    memset(&it, 0, sizeof it);
    it.page = page;
    it.type = PC_MENU_TOGGLE;
    it.label = "DUMP SONGS";
    it.key = "audio.music_dump";
    it.get = get_dump;
    it.set = set_dump;
    it.note = dump_note;
    pc_menu_add(&it);
    if (soundfont_menu_register) soundfont_menu_register(page);
    if (spu_quality_menu_register) spu_quality_menu_register();
}
