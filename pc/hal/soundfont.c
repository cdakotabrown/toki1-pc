/* SPDX-License-Identifier: GPL-3.0-or-later */
/* soundfont.c -- SoundFont mode (soundfont.h).
 *
 * ENHANCED-ONLY REPLACEMENT, READ-ONLY OBSERVER.  The game's libsnd keeps
 * keying the SPU's voices for every song; this module renders the same notes
 * with TinySoundFont (pc/3rd/tsf.h, MIT) from a user-supplied SF2, and the
 * SPU core's music split leaves the original voices out of the output.  It
 * writes no PS1 memory, calls no game or libsnd function and charges no
 * emulated time.
 *
 * The note events.  A note is the SPU key-on of a voice whose libsnd record
 * (_svm_voice[v], 0x18 bytes) names a sequence as its owner: +10 the owner
 * ((track << 8) | access number for a sequence note, 32 for a sound effect,
 * -1 never used), +8 the note, +0xE the program, +0xC the program's tone row,
 * +0x10 the tone, +0x12 the VAB slot, +0 the VAG (255: noise).  SpuVmKeyOn
 * fills it before SpuVmFlush writes KON, so when the register bridge
 * (spu_mmio_sync) forwards a KON the records of the keyed voices are the
 * notes it starts.  soundfont_kon_snapshot() reads them there, on the game
 * thread, resolves each to a preset / key / transpose / gain, and returns a
 * tag; the KON write carries the tag through the real-time queue
 * (pc/hal/spu.c g_rt), and soundfont_key_write() starts the notes when the
 * core takes the write -- at the write's own sample position in the
 * real-time mixer, before the VBlank's mix in the deterministic one.  KOFF
 * releases them.  Nothing is inferred from timing: tempo, pauses, fades,
 * stops and fast-forward reach the renderer exactly as they reach the SPU.
 *
 * Controls.  Before each mixed slice soundfont_render() reads every sounding
 * voice's registers from the core: the volume pair (libsnd's velocity x
 * channel x sequence x VAB volume and pan, i.e. every fade the driver makes)
 * becomes the TSF channel's gain and pan (constant-power law solved for the
 * exact left/right gains), the pitch register against its value at the
 * key-on becomes the channel's tuning (bends, vibrato, portamento).  A voice
 * the core no longer plays (its envelope ended) ends its note; a sound
 * effect keyed on a music voice releases the note and unmasks the voice.
 * Each SPU voice owns four TSF channels in turn, so a released note keeps
 * its own channel (its gain, pan, tuning) while the voice plays the next
 * three.  Notes on voices
 * with reverb (EON) render into the "wet" TSF instance, the others into the
 * "dry" one; the core adds both to the split's rest mix and the wet pair to
 * the rest's reverb input, so the game's own reverb, main volume and fades
 * apply to the SoundFont as they did to the voices.
 *
 * Mapping (soundfont_map below): a VAB is identified as "vab-" + FNV-1a 32 of
 * its header, program and tone tables (as music packs identify SEQs).  A
 * program maps to a General MIDI preset by, in order: the song's section of
 * <sf2 name>.map ([seq-xxxxxxxx]), the VAB's section ([vab-xxxxxxxx]), the
 * heuristic (classify_program: drum kit / percussion / bass / strings /
 * flute / brass / reed / mallet / piano / guitar from the tone table's key
 * ranges and envelopes and the keyed sample's loop, length, zero crossings
 * and periodicity).  Every heuristic choice is appended to
 * <sf2 name>.auto.map in the map syntax, for the player to copy and edit.
 */
#include "types.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <dirent.h>

#define TSF_STATIC
#define TSF_IMPLEMENTATION
#include "../3rd/tsf.h"

#include "soundfont.h"
#include "profile.h"
#include "pc_settings.h"
#include "pc_menu.h"
#include "spu_psx.h"

/* ---- the game state read here (PS1 RAM; read only) -------------------- */
extern u8 _svm_voice[];          /* 24 records of 0x18 bytes */
extern u32 _svm_vab_vh[];        /* 16 PS1 pointers: the VAB headers */
extern u32 _svm_vab_tn[];         /* 16 PS1 pointers: the tone tables (_svm_vab_tn) */
extern u8 _svm_vab_used[];
extern u32 hal_clock_vblanks(void) __attribute__((weak));
extern void spu_hal_audio_lock(void);
extern void spu_hal_audio_unlock(void);
extern const PcSettings *pc_settings_get(void) __attribute__((weak));
extern PcSettings *pc_settings_mut(void) __attribute__((weak));
extern const char *hal_setting_str(const char *name, const char *dflt) __attribute__((weak));
/* pc/hal/music_pack.c (weak: harnesses) */
extern void music_pack_dir(char *out, size_t n) __attribute__((weak));
extern const char *music_pack_acc_id(int acc) __attribute__((weak));
extern int music_pack_replaced_acc(void) __attribute__((weak));
extern void music_pack_split_refresh(void) __attribute__((weak));

#define RAM_BASE 0x80000000u
#define RAM_MASK 0x1FFFFFu
#define PS1_PTR(h) ((const u8 *)(uintptr_t)(RAM_BASE | ((u32)(h) & RAM_MASK)))
#define SPU_REG(off) (0x1F801C00u + (off))
#define SPU_PAGE ((volatile const u16 *)0x1F801C00ul)

#define NUM_VOICES 24
#define VOICE_STRIDE 0x18
#define SNAPS 1024
#define MAX_TSF_VOICES 96
#define CHANNELS_PER_VOICE 4     /* a released note keeps its channel for three more */
#define OUT_RATE 44100
#define DEFAULT_VELOCITY 100

static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }

/* ---- configuration ---------------------------------------------------- */

static const PcSettings *settings(void) { return pc_settings_get ? pc_settings_get() : NULL; }

/* The SF2 the player chose: TOKI_SOUNDFONT, else [audio] soundfont; a name
 * in <music dir>/soundfonts/ or a path.  "" = off. */
static const char *choice(void) {
    const PcSettings *s = settings();
    const char *d = s ? s->soundfont : "";
    const char *v = hal_setting_str ? hal_setting_str("TOKI_SOUNDFONT", d) : getenv("TOKI_SOUNDFONT");
    if (!v) v = d;
    if (!strcasecmp(v, "off") || !strcasecmp(v, "none")) return "";
    return v;
}

static void soundfont_dir(char *out, size_t n) {
    char m[900];
    if (music_pack_dir) music_pack_dir(m, sizeof m);
    else snprintf(m, sizeof m, "music");
    snprintf(out, n, "%s/soundfonts", m);
}

static void resolve_path(const char *name, char *out, size_t n) {
    char dir[1000];
    if (strchr(name, '/') || strchr(name, '\\')) { snprintf(out, n, "%s", name); return; }
    soundfont_dir(dir, sizeof dir);
    snprintf(out, n, "%s/%s", dir, name);
}

/* ---- the map file ------------------------------------------------------ */

enum { M_PRESET, M_OFF, M_TRANSPOSE, M_GAIN, M_KEY, M_VELOCITY };
typedef struct {
    char scope[16];                /* "vab-xxxxxxxx", "seq-xxxxxxxx", "" (global) */
    int prog, key, kind;
    int bank, preset;
    float value;
} MapEntry;
#define MAX_MAP 2048
static MapEntry g_map[MAX_MAP];
static int g_map_n;
static float g_gain_global = 1.0f;     /* [global] gain_db */
static int g_velocity = DEFAULT_VELOCITY;
static char g_auto_path[1100];

static char *trim(char *s) {
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

/* "B:P", "P", "drums", "drums:P", "off" */
static int parse_preset(const char *v, int *bank, int *preset) {
    char *end;
    long a;
    if (!strcasecmp(v, "off")) return M_OFF;
    if (!strncasecmp(v, "drums", 5)) {
        *bank = 128;
        *preset = v[5] == ':' ? atoi(v + 6) : 0;
        return M_PRESET;
    }
    a = strtol(v, &end, 10);
    if (end == v) return -1;
    if (*end == ':') { *bank = (int)a; *preset = atoi(end + 1); }
    else { *bank = 0; *preset = (int)a; }
    return M_PRESET;
}

static void map_load(const char *path) {
    FILE *f = fopen(path, "r");
    char line[512], scope[16] = "";
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *k, *v, *c;
        MapEntry e;
        if ((c = strchr(line, '#')) != NULL) *c = 0;
        if ((c = strchr(line, ';')) != NULL) *c = 0;
        k = trim(line);
        if (!*k) continue;
        if (*k == '[') {
            char *z = strchr(k, ']');
            if (z) *z = 0;
            k = trim(k + 1);
            snprintf(scope, sizeof scope, "%s", strcasecmp(k, "global") ? k : "");
            continue;
        }
        v = strchr(k, '=');
        if (!v) continue;
        *v++ = 0;
        k = trim(k);
        v = trim(v);
        memset(&e, 0, sizeof e);
        snprintf(e.scope, sizeof e.scope, "%s", scope);
        e.prog = e.key = -1;
        if (!strcasecmp(k, "gain_db") && !scope[0]) { g_gain_global = powf(10.0f, (float)atof(v) / 20.0f); continue; }
        if (!strcasecmp(k, "velocity") && !scope[0]) { g_velocity = atoi(v); continue; }
        if (!strncasecmp(k, "prog", 4)) {
            char *p = k + 4, *end;
            e.prog = (int)strtol(p, &end, 10);
            if (end == p) continue;
            p = trim(end);
            if (!*p) {
                e.kind = parse_preset(v, &e.bank, &e.preset);
                if (e.kind < 0) continue;
            } else if (!strcasecmp(p, "transpose")) {
                e.kind = M_TRANSPOSE; e.value = (float)atof(v);
            } else if (!strcasecmp(p, "gain_db")) {
                e.kind = M_GAIN; e.value = powf(10.0f, (float)atof(v) / 20.0f);
            } else if (!strcasecmp(p, "velocity")) {
                e.kind = M_VELOCITY; e.value = (float)atof(v);
            } else if (!strncasecmp(p, "key", 3)) {
                e.kind = M_KEY; e.key = atoi(p + 3); e.value = (float)atoi(v);
            } else continue;
        } else continue;
        if (g_map_n < MAX_MAP) g_map[g_map_n++] = e;
    }
    fclose(f);
}

/* The entry for (scope, prog, kind[, key]); the song's scope wins. */
static const MapEntry *map_find(const char *seq, const char *vab, int prog, int kind, int key) {
    int pass, i;
    for (pass = 0; pass < 2; pass++) {
        const char *sc = pass ? vab : seq;
        if (!sc || !sc[0]) continue;
        for (i = g_map_n - 1; i >= 0; i--) {
            const MapEntry *e = &g_map[i];
            if (e->prog != prog || strcmp(e->scope, sc)) continue;
            if (kind == M_PRESET ? (e->kind == M_PRESET || e->kind == M_OFF)
                                 : (e->kind == kind && (kind != M_KEY || e->key == key)))
                return e;
        }
    }
    return NULL;
}

/* ---- VAB identity ------------------------------------------------------ */

static u32 vab_hash(int slot) {
    const u8 *vh, *tn;
    u32 h = 2166136261u, i, ps, n;
    if (slot < 0 || slot >= 16 || _svm_vab_used[slot] != 1 || !_svm_vab_vh[slot]) return 0;
    vh = PS1_PTR(_svm_vab_vh[slot]);
    tn = PS1_PTR(_svm_vab_tn[slot]);
    ps = rd16(vh + 18);
    if (memcmp(vh, "pBAV", 4) || ps > 128) return 0;
    for (i = 0; i < 32 + 2048; i++) { h ^= vh[i]; h *= 16777619u; }
    n = ps * 16 * 32;
    for (i = 0; i < n; i++) { h ^= tn[i]; h *= 16777619u; }
    return h ? h : 1;
}

/* ---- sample analysis (SPU RAM, read on the game thread) ---------------- */

typedef struct {
    int n, looped, periodic;
    float dur;          /* seconds at the played pitch (one-shot) */
    float zcr_hz;       /* zero crossings per second / 2 */
    float harm;         /* brightness: RMS slope / (RMS level x 2 pi f0); 1 = a sine */
    float f0;           /* Hz, when periodic */
} SampleFeat;

#define MAX_DECODE 32768
static s16 g_dec[MAX_DECODE];

static int decode_vag(u32 addr_bytes, int *looped) {
    static const int fp[5] = { 0, 60, 115, 98, 122 }, fn[5] = { 0, 0, -52, -55, -60 };
    u8 blk[16];
    int s1 = 0, s2 = 0, n = 0;
    *looped = 0;
    while (n + 28 <= MAX_DECODE) {
        int shift, filt, i;
        spu_psx_dma_read(blk, addr_bytes & 0x7FFFF, 16);
        addr_bytes += 16;
        shift = blk[0] & 0xF;
        filt = (blk[0] >> 4) & 7;
        if (filt > 4) filt = 0;
        if (shift > 12) shift = 9;
        for (i = 0; i < 28; i++) {
            int nib = (blk[2 + i / 2] >> ((i & 1) * 4)) & 0xF;
            int s = (s16)(nib << 12) >> shift;
            s += (s1 * fp[filt]) >> 6;
            s += (s2 * fn[filt]) >> 6;
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            s2 = s1;
            s1 = s;
            g_dec[n++] = (s16)s;
        }
        if (blk[1] & 1) { *looped = (blk[1] & 2) != 0; break; }
    }
    return n;
}

static void analyze(u32 start_reg, u16 pitch, SampleFeat *f) {
    int n, a, len, i, lag, best_lag = 0, cross = 0;
    double best = 0.0, rate;
    memset(f, 0, sizeof *f);
    n = decode_vag((u32)start_reg * 8u, &f->looped);
    f->n = n;
    rate = 44100.0 * (pitch ? pitch : 0x1000) / 4096.0;
    f->dur = (float)(n / rate);
    a = n > 4096 ? n / 8 : 0;
    len = n - a < 2048 ? n - a : 2048;
    if (len < 16) return;
    for (i = a + 1; i < a + len; i++)
        if ((g_dec[i - 1] < 0) != (g_dec[i] < 0)) cross++;
    f->zcr_hz = (float)(cross * rate / len / 2.0);
    for (lag = 8; lag <= len / 2; lag++) {
        double xy = 0, xx = 0, yy = 0, r;
        int m = len - lag;
        for (i = 0; i < m; i++) {
            double x = g_dec[a + i], y = g_dec[a + i + lag];
            xy += x * y; xx += x * x; yy += y * y;
        }
        if (xx <= 0 || yy <= 0) continue;
        r = xy / sqrt(xx * yy);
        if (r > best) { best = r; best_lag = lag; }
    }
    f->periodic = best >= 0.6;
    if (best_lag) {
        double ex = 0, ed = 0;
        for (i = a + 1; i < a + len; i++) {
            double x = g_dec[i], d = (double)g_dec[i] - g_dec[i - 1];
            ex += x * x;
            ed += d * d;
        }
        f->f0 = (float)(rate / best_lag);
        /* a sine of period P has slope RMS = level RMS x 2 sin(pi / P) */
        f->harm = ex > 0 ? (float)(sqrt(ed / ex) / (2.0 * sin(3.14159265358979 / best_lag))) : 0.0f;
    }
}

/* ---- the heuristic ----------------------------------------------------- */

/* The SPU envelope from the tone's ADSR words: how a note behaves after
 * its attack. */
typedef struct { int slow_attack, sustained, fast_decay; } Env;
static Env envelope(u16 adsr1, u16 adsr2) {
    Env e;
    int attack_shift = (adsr1 >> 10) & 0x1F, sustain_level = adsr1 & 0xF;
    int sustain_dec = (adsr2 >> 14) & 1, sustain_shift = (adsr2 >> 8) & 0x1F;
    e.slow_attack = attack_shift >= 12;
    e.sustained = sustain_level >= 6 && (!sustain_dec || sustain_shift >= 0x1A);
    e.fast_decay = sustain_dec && sustain_shift <= 0x13;
    return e;
}

typedef struct { int bank, preset, drum; char why[96]; } ProgMap;

/* One program: its tones (the program's tone row) and the keyed sample. */
static ProgMap classify_program(const u8 *prog_atr, const u8 *tones, int ntones, const SampleFeat *sf,
                                const u8 *keyed_tone) {
    ProgMap m;
    int i, narrow = 0, distinct = 0, hi = 0, lo = 127, center_sum = 0;
    Env env = envelope(rd16(keyed_tone + 16), rd16(keyed_tone + 18));
    (void)prog_atr;
    memset(&m, 0, sizeof m);
    for (i = 0; i < ntones; i++) {
        const u8 *t = tones + i * 32;
        int mn = t[6], mx = t[7], j;
        if (mx - mn <= 2) narrow++;
        if (mx > hi) hi = mx;
        if (mn < lo) lo = mn;
        center_sum += t[4];
        for (j = 0; j < i && rd16(tones + j * 32 + 22) != rd16(t + 22); j++) {}
        if (j == i) distinct++;
    }
    /* a kit: one-key zones, nearly each its own sample (a multi-sampled
     * instrument spreads a few samples over many zones, e.g. per-key pan) */
    if (ntones >= 3 && narrow * 4 >= ntones * 3 && distinct * 10 >= ntones * 6) {
        m.bank = 128; m.preset = 0; m.drum = 1;
        snprintf(m.why, sizeof m.why, "drum kit: %d tones, %d one-key, %d samples", ntones, narrow, distinct);
        return m;
    }
    if (!sf->looped && (!sf->periodic || sf->dur < 0.35f)) {
        m.bank = 128; m.preset = 0; m.drum = 1;
        snprintf(m.why, sizeof m.why, "percussion: one-shot %.2f s, %s", sf->dur,
                 sf->periodic ? "pitched" : "noise");
        return m;
    }
    if (hi <= 60 && ntones && center_sum / ntones <= 52) {
        m.preset = 33;
        snprintf(m.why, sizeof m.why, "bass: keys %d-%d", lo, hi);
        return m;
    }
    if (sf->looped && env.sustained) {
        if (env.slow_attack) { m.preset = 48; snprintf(m.why, sizeof m.why, "strings: looped, sustained, slow attack"); }
        else if (sf->harm <= 1.6f) { m.preset = 73; snprintf(m.why, sizeof m.why, "flute: looped, sustained, pure (%.1f)", sf->harm); }
        else if (sf->harm >= 3.0f) { m.preset = 61; snprintf(m.why, sizeof m.why, "brass: looped, sustained, bright (%.1f)", sf->harm); }
        else { m.preset = 71; snprintf(m.why, sizeof m.why, "reed: looped, sustained (%.1f)", sf->harm); }
        return m;
    }
    if (env.fast_decay || (!sf->looped && sf->dur < 1.0f)) {
        if (sf->harm >= 2.5f) { m.preset = 11; snprintf(m.why, sizeof m.why, "mallet: decaying, bright (%.1f)", sf->harm); }
        else if (!sf->looped && sf->dur < 0.6f) { m.preset = 24; snprintf(m.why, sizeof m.why, "guitar: short pluck %.2f s", sf->dur); }
        else { m.preset = 12; snprintf(m.why, sizeof m.why, "marimba: decaying, soft (%.1f)", sf->harm); }
        return m;
    }
    m.preset = 0;
    snprintf(m.why, sizeof m.why, "piano: decaying%s", sf->looped ? ", looped" : "");
    return m;
}

/* A General MIDI drum key for one percussion sample at its played pitch. */
static int classify_drum(const SampleFeat *f, char *why, size_t n) {
    int key;
    const char *name;
    if (!f->periodic && f->zcr_hz >= 3000.0f && f->dur >= 0.8f) { key = 49; name = "crash"; }
    else if (!f->periodic && f->zcr_hz >= 5000.0f) {
        if (f->dur < 0.15f) { key = 42; name = "closed hi-hat"; }
        else if (f->dur < 0.5f) { key = 46; name = "open hi-hat"; }
        else { key = 51; name = "ride"; }
    } else if (!f->periodic && f->zcr_hz >= 1500.0f) { key = 38; name = "snare"; }
    else if (!f->periodic) { key = f->zcr_hz < 600.0f && f->dur < 0.3f ? 36 : 38; name = key == 36 ? "kick" : "snare"; }
    else if (f->f0 < 120.0f || f->zcr_hz < 150.0f) { key = 36; name = "kick"; }
    else if (f->zcr_hz < 1500.0f) {
        key = f->zcr_hz < 250.0f ? 41 : f->zcr_hz < 400.0f ? 45 : f->zcr_hz < 700.0f ? 47 : 50;
        name = "tom";
    } else { key = f->dur < 0.2f ? 75 : 81; name = key == 75 ? "claves" : "triangle"; }
    snprintf(why, n, "%s: %s, %.0f Hz crossings, %.2f s", name, f->periodic ? "pitched" : "noise",
             f->zcr_hz, f->dur);
    return key;
}

/* Caches: per (VAB, program) and, for percussion, per (VAB, program, key). */
typedef struct { u32 vab; int prog; ProgMap m; } ProgCache;
typedef struct { u32 vab; int prog, key, gm; } DrumCache;
#define MAX_PROG_CACHE 512
#define MAX_DRUM_CACHE 1024
static ProgCache g_prog_cache[MAX_PROG_CACHE];
static int g_prog_cache_n;
static DrumCache g_drum_cache[MAX_DRUM_CACHE];
static int g_drum_cache_n;

static void auto_map_append(u32 vab, const char *line, const char *why) {
    FILE *f;
    if (!g_auto_path[0]) return;
    f = fopen(g_auto_path, "a");
    if (!f) return;
    fprintf(f, "[vab-%08x]\n%-22s ; %s (heuristic)\n", (unsigned)vab, line, why);
    fclose(f);
}

/* ---- the loaded SoundFont and its voices (mixer side, audio lock) ------ */

typedef struct {
    s16 preset;          /* TSF preset index; -1 = not a SoundFont note */
    u8 key;
    u8 drum;
    float transpose;     /* semitones */
    float gain;          /* linear (map gain x global gain) */
    float vel;           /* 0..1 */
} SfNote;
typedef struct {
    u32 gen, font_gen, mask;
    SfNote note[NUM_VOICES];
} Snap;
static Snap g_snap[SNAPS];
static u32 g_snap_gen;

typedef struct {
    int on;              /* a note sounds (not released) */
    int fresh;           /* keyed since the last render: the core starts it in the next mix */
    int flip;
    tsf *inst;
    int chan, key;
    u16 p0;
    float transpose, gain, vel;
    float vol, pan, tune;
} SfVoice;

static tsf *g_wet, *g_dry;           /* NULL: off */
static u32 g_font_gen;
static SfVoice g_v[NUM_VOICES];
static u32 g_owned;                  /* voices whose last key-on was a SoundFont note */
static char g_loaded[1100];          /* what is loaded ("" none) */
static char g_wanted[1100];
static int g_load_failed;
static SoundfontHealth g_h;
static float g_fw[256 * 2], g_fd[256 * 2];

static int voice_reverb(int v) {
    u16 eon = spu_psx_read_reg(SPU_REG(v < 16 ? 0x198 : 0x19A));
    return (eon >> (v & 15)) & 1;
}

static float level(u16 reg, float prev) {
    if (reg & 0x8000) return prev;                  /* sweep mode: libsnd never uses it */
    return fabsf((float)(s16)(u16)(reg << 1)) / 32768.0f;
}

/* The channel's gain, pan and tuning from the voice's registers. */
static void controls(int v, int force) {
    SfVoice *c = &g_v[v];
    u32 o = (u32)v * 0x10;
    float al = level(spu_psx_read_reg(SPU_REG(o)), 0.0f), ar = level(spu_psx_read_reg(SPU_REG(o + 2)), 0.0f);
    u16 p = spu_psx_read_reg(SPU_REG(o + 4));
    float g2 = al * al + ar * ar, vol, pan, tune;
    vol = sqrtf(g2) * c->gain / c->vel;
    pan = g2 > 0.0f ? (ar * ar - al * al) / (2.0f * g2) + 0.5f : 0.5f;
    tune = c->transpose + (c->p0 && p ? 12.0f * log2f((float)p / (float)c->p0) : 0.0f);
    if (force || fabsf(vol - c->vol) > 1e-6f) { tsf_channel_set_volume(c->inst, c->chan, vol); c->vol = vol; }
    if (force || fabsf(pan - c->pan) > 1e-6f) { tsf_channel_set_pan(c->inst, c->chan, pan); c->pan = pan; }
    if (force || fabsf(tune - c->tune) > 1e-5f) { tsf_channel_set_tuning(c->inst, c->chan, tune); c->tune = tune; }
}

static void note_off(int v) {
    SfVoice *c = &g_v[v];
    if (!c->on) return;
    tsf_channel_note_off(c->inst, c->chan, c->key);
    c->on = 0;
    c->fresh = 0;
    g_h.notes_off++;
}

static void note_on(int v, const SfNote *n) {
    SfVoice *c = &g_v[v];
    c->flip = (c->flip + 1) & (CHANNELS_PER_VOICE - 1);
    c->chan = v * CHANNELS_PER_VOICE + c->flip;
    c->inst = voice_reverb(v) ? g_wet : g_dry;
    c->key = n->key;
    c->transpose = n->transpose;
    c->gain = n->gain;
    c->vel = n->vel > 0.0f ? n->vel : 1.0f;
    c->p0 = spu_psx_read_reg(SPU_REG((u32)v * 0x10 + 4));
    tsf_channel_set_presetindex(c->inst, c->chan, n->preset);
    controls(v, 1);
    tsf_channel_note_on(c->inst, c->chan, n->key, c->vel);
    c->on = 1;
    c->fresh = 1;
    g_h.notes_on++;
}

/* After a quickload, a toolkit load or a rewind (pc/hal/spu.c
 * spu_hal_after_restore, audio lock held): the notes sounding belong to the
 * old time line.  Silence them at once; the voices stay out of the output
 * (a restored voice holding a music note is not heard until it is keyed
 * again), the next key-ons are the restored machine's. */
void soundfont_after_restore(void) {
    int v, ch;
    if (!g_wet) return;
    for (ch = 0; ch < NUM_VOICES * CHANNELS_PER_VOICE; ch++) {
        tsf_channel_sounds_off_all(g_wet, ch);
        tsf_channel_sounds_off_all(g_dry, ch);
    }
    for (v = 0; v < NUM_VOICES; v++) {
        int flip = g_v[v].flip;
        memset(&g_v[v], 0, sizeof g_v[v]);
        g_v[v].flip = flip;
    }
}

int soundfont_rendering(void) { return g_wet != NULL; }
int soundfont_split_on(void) { return g_wet != NULL; }
u32 soundfont_split_mask(void) { return g_wet ? g_owned : 0; }

void soundfont_key_write(u32 off, u16 value, u32 tag) {
    int base = (off & 2) ? 16 : 0, b;
    u32 before = g_owned;
    if (!g_wet || !value) return;
    if (off == 0x188 || off == 0x18A) {
        const Snap *s = tag ? &g_snap[tag % SNAPS] : NULL;
        if (s && (s->gen != tag || s->font_gen != g_font_gen)) { g_h.unknown_tag++; s = NULL; }
        for (b = 0; b < 16 && base + b < NUM_VOICES; b++) {
            int v = base + b;
            u32 bit = 1u << v;
            if (!((value >> b) & 1)) continue;
            note_off(v);                       /* the voice restarts: its note ends */
            if (s && (s->mask & bit) && s->note[v].preset >= 0) {
                note_on(v, &s->note[v]);
                g_owned |= bit;
            } else {
                if (g_owned & bit) g_h.taken++;
                g_owned &= ~bit;
            }
        }
    } else if (off == 0x18C || off == 0x18E) {
        for (b = 0; b < 16 && base + b < NUM_VOICES; b++)
            if ((value >> b) & 1) note_off(base + b);
    }
    if (g_owned != before && music_pack_split_refresh) music_pack_split_refresh();
}

void soundfont_render(s32 *frames, int n, float gain) {
    u32 on;
    int v, done = 0;
    float k = 32767.0f * g_gain_global * gain;
    if (!g_wet) { memset(frames, 0, (size_t)n * 4 * sizeof(s32)); return; }
    on = spu_psx_voice_on_mask();
    for (v = 0; v < NUM_VOICES; v++) {
        SfVoice *c = &g_v[v];
        if (!c->on) continue;
        if (!c->fresh && !((on >> v) & 1)) {   /* the voice ended by itself */
            note_off(v);
            g_h.ended++;
            continue;
        }
        c->fresh = 0;
        controls(v, 0);
    }
    while (done < n) {
        int m = n - done > 256 ? 256 : n - done, i;
        tsf_render_float(g_dry, g_fd, m, 0);
        tsf_render_float(g_wet, g_fw, m, 0);
        for (i = 0; i < m; i++) {
            s32 *o = frames + (size_t)(done + i) * 4;
            o[0] = (s32)floorf(g_fd[i * 2] * k + 0.5f);
            o[1] = (s32)floorf(g_fd[i * 2 + 1] * k + 0.5f);
            o[2] = (s32)floorf(g_fw[i * 2] * k + 0.5f);
            o[3] = (s32)floorf(g_fw[i * 2 + 1] * k + 0.5f);
        }
        done += m;
    }
    g_h.frames += (unsigned long long)n;
}

/* ---- resolving a note (game thread, audio lock held) ------------------- */

static int find_preset(tsf *f, int bank, int preset, int drum) {
    int i = tsf_get_presetindex(f, bank, preset);
    if (i < 0 && drum) i = tsf_get_presetindex(f, 128, 0);
    if (i < 0 && !drum) i = tsf_get_presetindex(f, 0, preset);
    if (i < 0 && !drum) i = tsf_get_presetindex(f, 0, 0);
    if (i < 0) i = tsf_get_presetcount(f) > 0 ? 0 : -1;
    return i;
}

static void resolve(int v, const u8 *rec, SfNote *out) {
    int acc = rd16(rec + 10) & 0xFF, note = (s16)rd16(rec + 8), prog = (s16)rd16(rec + 14);
    int row = (s16)rd16(rec + 12), tone = (s16)rd16(rec + 16), slot = (s16)rd16(rec + 18);
    const char *seq = music_pack_acc_id ? music_pack_acc_id(acc) : NULL;
    char vabid[16];
    const MapEntry *e;
    u32 vab;
    int bank, preset, drum, key, i;
    float transpose = 0.0f, gain = 1.0f, vel = (float)g_velocity / 127.0f;
    out->preset = -1;
    if (prog < 0 || prog > 127 || note < 0 || note > 127 || tone < 0 || tone > 15) return;
    vab = vab_hash(slot);
    if (!vab) return;
    snprintf(vabid, sizeof vabid, "vab-%08x", (unsigned)vab);
    e = map_find(seq, vabid, prog, M_PRESET, -1);
    if (e && e->kind == M_OFF) return;
    if (e) {
        bank = e->bank; preset = e->preset; drum = bank == 128;
    } else {
        const ProgCache *pc = NULL;
        for (i = 0; i < g_prog_cache_n; i++)
            if (g_prog_cache[i].vab == vab && g_prog_cache[i].prog == prog) { pc = &g_prog_cache[i]; break; }
        if (!pc) {
            const u8 *vh = PS1_PTR(_svm_vab_vh[slot]), *tn = PS1_PTR(_svm_vab_tn[slot]);
            const u8 *pa = vh + 32 + prog * 16, *tones = tn + row * 16 * 32;
            int ntones = pa[0] > 16 ? 16 : pa[0];
            SampleFeat sf;
            ProgCache nc;
            char line[40];
            analyze(SPU_PAGE[((u32)v * 0x10 + 6) / 2], SPU_PAGE[((u32)v * 0x10 + 4) / 2], &sf);
            nc.vab = vab;
            nc.prog = prog;
            nc.m = classify_program(pa, tones, ntones, &sf, tones + tone * 32);
            if (nc.m.drum) snprintf(line, sizeof line, "prog %d = drums", prog);
            else snprintf(line, sizeof line, "prog %d = %d", prog, nc.m.preset);
            printf("[SF2] %s prog %d -> %d:%d (%s)\n", vabid, prog, nc.m.bank, nc.m.preset, nc.m.why);
            auto_map_append(vab, line, nc.m.why);
            if (g_prog_cache_n < MAX_PROG_CACHE) {
                g_prog_cache[g_prog_cache_n] = nc;
                pc = &g_prog_cache[g_prog_cache_n++];
            } else {
                static ProgCache spill;
                spill = nc;
                pc = &spill;
            }
        }
        bank = pc->m.bank; preset = pc->m.preset; drum = pc->m.drum;
    }
    key = note;
    if ((e = map_find(seq, vabid, prog, M_KEY, note)) != NULL) {
        key = (int)e->value;
    } else if (drum) {
        const DrumCache *dc = NULL;
        for (i = 0; i < g_drum_cache_n; i++)
            if (g_drum_cache[i].vab == vab && g_drum_cache[i].prog == prog && g_drum_cache[i].key == note) {
                dc = &g_drum_cache[i];
                break;
            }
        if (dc) key = dc->gm;
        else {
            SampleFeat sf;
            char why[96], line[40];
            analyze(SPU_PAGE[((u32)v * 0x10 + 6) / 2], SPU_PAGE[((u32)v * 0x10 + 4) / 2], &sf);
            key = classify_drum(&sf, why, sizeof why);
            printf("[SF2] %s prog %d key %d -> drum %d (%s)\n", vabid, prog, note, key, why);
            snprintf(line, sizeof line, "prog %d key %d = %d", prog, note, key);
            auto_map_append(vab, line, why);
            if (g_drum_cache_n < MAX_DRUM_CACHE) {
                DrumCache *d = &g_drum_cache[g_drum_cache_n++];
                d->vab = vab; d->prog = prog; d->key = note; d->gm = key;
            }
        }
    }
    if ((e = map_find(seq, vabid, prog, M_TRANSPOSE, -1)) != NULL) transpose = e->value;
    if ((e = map_find(seq, vabid, prog, M_GAIN, -1)) != NULL) gain = e->value;
    if ((e = map_find(seq, vabid, prog, M_VELOCITY, -1)) != NULL) vel = e->value / 127.0f;
    if (!drum) {
        int whole = (int)floorf(transpose + 0.5f);
        key += whole;
        transpose -= (float)whole;
    }
    if (key < 0) key = 0;
    if (key > 127) key = 127;
    out->preset = (s16)find_preset(g_wet, bank, preset, drum);
    out->key = (u8)key;
    out->drum = (u8)drum;
    out->transpose = drum ? 0.0f : transpose;
    out->gain = gain;
    out->vel = vel < 0.01f ? 0.01f : vel > 1.0f ? 1.0f : vel;
}

u32 soundfont_kon_snapshot(u16 kon_lo, u16 kon_hi) {
    u32 keyed = (u32)kon_lo | ((u32)kon_hi << 16), tag;
    int v, replaced = music_pack_replaced_acc ? music_pack_replaced_acc() : -1;
    Snap *s;
    if (!g_wet || !keyed) return 0;
    tag = ++g_snap_gen;
    if (!tag) tag = ++g_snap_gen;
    s = &g_snap[tag % SNAPS];
    s->gen = tag;
    s->font_gen = g_font_gen;
    s->mask = 0;
    for (v = 0; v < NUM_VOICES; v++) {
        const u8 *rec = _svm_voice + v * VOICE_STRIDE;
        s16 owner = (s16)rd16(rec + 10);
        int acc = owner & 0xFF;
        if (!((keyed >> v) & 1)) continue;
        s->mask |= 1u << v;
        s->note[v].preset = -1;
        if (owner == -1 || acc >= 16 || acc == replaced || rd16(rec) == 255) continue;
        resolve(v, rec, &s->note[v]);
        if (s->note[v].preset >= 0) g_h.music_keys++;
    }
    return tag;
}

/* ---- loading ----------------------------------------------------------- */

static void reset_voices_locked(void) {
    memset(g_v, 0, sizeof g_v);
    g_owned = 0;
}

static void unload(void) {
    tsf *w, *d;
    spu_hal_audio_lock();
    w = g_wet; d = g_dry;
    g_wet = g_dry = NULL;
    reset_voices_locked();
    g_font_gen++;
    if (music_pack_split_refresh) music_pack_split_refresh();
    spu_hal_audio_unlock();
    if (w) tsf_close(w);
    if (d) tsf_close(d);
    g_loaded[0] = 0;
}

static tsf *prepare(tsf *f) {
    int ch;
    if (!f) return NULL;
    tsf_set_output(f, TSF_STEREO_INTERLEAVED, OUT_RATE, 0.0f);
    tsf_set_max_voices(f, MAX_TSF_VOICES);
    for (ch = 0; ch < NUM_VOICES * CHANNELS_PER_VOICE; ch++) tsf_channel_set_presetindex(f, ch, 0);
    return f;
}

static void load(const char *name) {
    char path[1100], dir[1000], base[600];
    tsf *w, *d;
    const char *slash, *dot;
    unload();
    resolve_path(name, path, sizeof path);
    w = tsf_load_filename(path);
    if (!w) {
        printf("[SF2] cannot load %s\n", path);
        g_load_failed = 1;
        return;
    }
    d = tsf_copy(w);
    if (!d) { tsf_close(w); return; }
    prepare(w);
    prepare(d);
    /* <name>.map next to the SF2, and the heuristic's <name>.auto.map */
    slash = strrchr(path, '/');
    if (!slash) slash = strrchr(path, '\\');
    snprintf(dir, sizeof dir, "%.*s", slash ? (int)(slash - path) : 1, slash ? path : ".");
    snprintf(base, sizeof base, "%s", slash ? slash + 1 : path);
    dot = strrchr(base, '.');
    if (dot) base[dot - base] = 0;
    g_map_n = 0;
    g_gain_global = 1.0f;
    g_velocity = DEFAULT_VELOCITY;
    {
        char mp[1700];
        snprintf(mp, sizeof mp, "%s/%s.map", dir, base);
        map_load(mp);
        snprintf(g_auto_path, sizeof g_auto_path, "%s/%s.auto.map", dir, base);
        printf("[SF2] loaded %s: %d presets; map %s (%d entries)\n", path, tsf_get_presetcount(w), mp, g_map_n);
    }
    g_prog_cache_n = g_drum_cache_n = 0;
    spu_hal_audio_lock();
    g_wet = w;
    g_dry = d;
    reset_voices_locked();
    g_font_gen++;
    if (music_pack_split_refresh) music_pack_split_refresh();
    spu_hal_audio_unlock();
    snprintf(g_loaded, sizeof g_loaded, "%s", name);
}

/* TOKI_SF2_TRACE=PATH: per VBlank, the voices the SoundFont owns, its
 * sounding notes (and those keyed since the last mix: the core starts them
 * in the next one), the core's sounding voices, TSF's active voices and the
 * counters. */
static FILE *g_trace;
static void trace_line(void) {
    static int init;
    u32 notes = 0, fresh = 0, core;
    int v, active;
    if (!init) {
        const char *p = getenv("TOKI_SF2_TRACE");
        init = 1;
        if (p && *p) g_trace = fopen(p, "w");
    }
    if (!g_trace) return;
    spu_hal_audio_lock();
    for (v = 0; v < NUM_VOICES; v++) {
        if (g_v[v].on) notes |= 1u << v;
        if (g_v[v].fresh) fresh |= 1u << v;
    }
    core = spu_psx_voice_on_mask();
    active = g_wet ? tsf_active_voice_count(g_wet) + tsf_active_voice_count(g_dry) : 0;
    fprintf(g_trace, "%u owned %06x notes %06x fresh %06x core %06x active %d on %u off %u keys %u ended %u "
            "taken %u unknown %u frames %llu\n", hal_clock_vblanks ? hal_clock_vblanks() : 0, (unsigned)g_owned,
            (unsigned)notes, (unsigned)fresh, (unsigned)core, active, g_h.notes_on, g_h.notes_off,
            g_h.music_keys, g_h.ended, g_h.taken, g_h.unknown_tag, g_h.frames);
    spu_hal_audio_unlock();
    fflush(g_trace);
}

static void health_at_exit(void) {
    printf("[SF2] health: notes on %u, off %u, music key-ons %u, ended by the voice %u, taken by effects %u, "
           "unknown tags %u\n", g_h.notes_on, g_h.notes_off, g_h.music_keys, g_h.ended, g_h.taken, g_h.unknown_tag);
    fflush(stdout);
}

void soundfont_tick(void) {
    const char *want = hal_model_on("MUSIC_SOUNDFONT") ? choice() : "";
    if (strcmp(want, g_wanted)) {
        static int registered;
        snprintf(g_wanted, sizeof g_wanted, "%s", want);
        g_load_failed = 0;
        if (want[0]) {
            load(want);
            if (!registered && g_wet) { registered = 1; atexit(health_at_exit); }
        } else if (g_loaded[0]) {
            unload();
            printf("[SF2] off: the original music\n");
        }
    }
    trace_line();
}

void soundfont_health(SoundfontHealth *out) { *out = g_h; }

u32 soundfont_notes_mask(void) {
    u32 m = 0;
    int v;
    for (v = 0; v < NUM_VOICES; v++)
        if (g_v[v].on) m |= 1u << v;
    return m;
}

/* ---- the settings menu ------------------------------------------------- */

#define MAX_SF 32
static char g_sf_names[MAX_SF][64];
static const char *g_sf_choice[MAX_SF + 2];
static int g_sf_count;

static int get_sf(void *ctx) {
    const PcSettings *s = settings();
    int i;
    (void)ctx;
    if (!s || !s->soundfont[0]) return 0;
    for (i = 0; i < g_sf_count; i++)
        if (!strcmp(g_sf_names[i], s->soundfont)) return i + 1;
    return 0;
}
static void set_sf(void *ctx, int v) {
    PcSettings *s = pc_settings_mut ? pc_settings_mut() : NULL;
    (void)ctx;
    if (!s) return;
    snprintf(s->soundfont, sizeof s->soundfont, "%s", v > 0 && v <= g_sf_count ? g_sf_names[v - 1] : "");
}
static const char *sf_note(void *ctx) {
    const char *e = getenv("TOKI_SOUNDFONT");
    (void)ctx;
    if (e && *e) return "ENV OVERRIDE";
    if (!hal_model_on("MUSIC_SOUNDFONT")) return "ENHANCED ONLY";
    if (g_load_failed) return "CANNOT LOAD";
    return g_sf_count ? "" : "NO .SF2 FOUND";
}
static int name_cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

void soundfont_menu_register(int page) {
    char dir[1000];
    DIR *d;
    struct dirent *e;
    PcMenuItem it;
    int i;
    soundfont_dir(dir, sizeof dir);
    g_sf_count = 0;
    if ((d = opendir(dir)) != NULL) {
        while ((e = readdir(d)) != NULL && g_sf_count < MAX_SF) {
            size_t n = strlen(e->d_name);
            if (n < 5 || n >= sizeof g_sf_names[0] || strcasecmp(e->d_name + n - 4, ".sf2")) continue;
            snprintf(g_sf_names[g_sf_count++], sizeof g_sf_names[0], "%s", e->d_name);
        }
        closedir(d);
    }
    qsort(g_sf_names, (size_t)g_sf_count, sizeof g_sf_names[0], name_cmp);
    g_sf_choice[0] = "OFF (ORIGINAL)";
    for (i = 0; i < g_sf_count; i++) g_sf_choice[i + 1] = g_sf_names[i];
    g_sf_choice[g_sf_count + 1] = NULL;
    memset(&it, 0, sizeof it);
    it.page = page;
    it.type = PC_MENU_CHOICE;
    it.label = "SOUNDFONT";
    it.key = "audio.soundfont";
    it.choices = g_sf_choice;
    it.get = get_sf;
    it.set = set_sf;
    it.note = sf_note;
    if (pc_menu_add(&it) < 0) printf("[SF2] the settings menu is full: no SOUNDFONT entry\n");
}
