/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Music packs against a simulated libsnd (pc/tools/test_music_pack.py).
 *
 * Links the actual pc/hal/music_pack.c (MUSIC_PACK_SOURCE, so the test can
 * build mutated copies as negative controls) and pc/hal/music_decode.c.
 * The PS1 RAM window is a real fixed mapping at 0x80000000 as in the port;
 * libsnd's state lives in it the way the matched code leaves it: a score
 * record (flags +144, volume +114/+116, +8 the data after the SEQ header)
 * behind _ss_score[0], _snd_openflag, and _svm_voice's owner words.  The
 * SPU core is a spy: spu_psx_music_split records its arguments and
 * spu_psx_read_reg answers the main volume.  No game code, no audio device.
 *
 * argv: CASE SEQFILE.  The test writes SEQFILE (a synthetic SEQ) and the
 * pack (TOKI_MUSIC_DIR/packs/<pack>/<id>.wav: a stereo ramp whose left
 * channel encodes the frame number, 4 per step and never 0, so every output
 * frame names the file position it came from).  Each check prints "CHECK name"
 * on failure; the test matches the name for the negative controls.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "types.h"
#include "music_pack.h"

#ifndef MUSIC_PACK_SOURCE
#define MUSIC_PACK_SOURCE "../hal/music_pack.c"
#endif
#include MUSIC_PACK_SOURCE

/* (weak: the negative controls build music_pack.c from before it existed) */
void music_pack_after_load(void) __attribute__((weak));

/* ---- the simulated game side ------------------------------------------ */
s32 _snd_openflag;
u32 _ss_score[16];
u8 _svm_voice[24 * 0x18];
s32 frame_counter;
u8 now_step = 18, now_sub_step;
static u32 g_vbl;
u32 hal_clock_vblanks(void) { return g_vbl; }
const char *hal_overlay_loaded(void) { return "-"; }
void spu_hal_audio_lock(void) {}
void spu_hal_audio_unlock(void) {}
/* the settings menu: a recorder */
static PcMenuItem g_items[8];
static int g_nitems;
static const char *g_page_title;
int pc_menu_page(const char *t) { g_page_title = t; return 3; }
int pc_menu_add(const PcMenuItem *i) {
    if (g_nitems >= 8) return -1;
    g_items[g_nitems] = *i;
    return g_nitems++;
}

static int g_split_calls, g_split_on_, g_split_replace_;
static u32 g_split_mask_;
static int16_t g_mainvol = 0x7FFE;
void spu_psx_music_split(int on, u32 mask, int replace) {
    g_split_calls++;
    g_split_on_ = on;
    g_split_mask_ = mask;
    g_split_replace_ = replace;
}
u16 spu_psx_read_reg(u32 addr) {
    return (addr == 0x1F801DB8 || addr == 0x1F801DBA) ? (u16)g_mainvol : 0;
}

#define SEQ_ADDR 0x8002E850u
#define SCORE_ADDR 0x80131000u
#define SCORE ((u8 *)(uintptr_t)SCORE_ADDR)
#define FLAGS (*(u32 *)(SCORE + 144))
#define VOL_L (*(s16 *)(SCORE + 114))
#define VOL_R (*(s16 *)(SCORE + 116))

static int g_fail;
#define CHECK(name, cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "CHECK %s: ", name); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); \
    g_fail = 1; } } while (0)

static void owner(int v, s16 o) { *(s16 *)(_svm_voice + v * 0x18 + 10) = o; }

static void tick(void) {
    g_vbl++;
    music_pack_tick();
    music_pack_voice_sync();
}

/* ---- a stdio spy (case "io"): file calls made inside music_pack_mix, the
 * audio thread's entry (STATE-MACHINES.md C5) ---- */
#include <dlfcn.h>
#include <stdarg.h>
static int g_in_mix, g_io_in_mix;
#define REAL(name) static __typeof__(name) *real_##name; \
    if (!real_##name) real_##name = (__typeof__(name) *)dlsym(RTLD_NEXT, #name)
#define SPY() do { if (g_in_mix) g_io_in_mix++; } while (0)
FILE *fopen(const char *p, const char *m) { REAL(fopen); SPY(); return real_fopen(p, m); }
int fclose(FILE *f) { REAL(fclose); SPY(); return real_fclose(f); }
size_t fwrite(const void *b, size_t s, size_t n, FILE *f) { REAL(fwrite); SPY(); return real_fwrite(b, s, n, f); }
size_t fread(void *b, size_t s, size_t n, FILE *f) { REAL(fread); SPY(); return real_fread(b, s, n, f); }
int fseek(FILE *f, long o, int w) { REAL(fseek); SPY(); return real_fseek(f, o, w); }
int fflush(FILE *f) { REAL(fflush); SPY(); return real_fflush(f); }
int vfprintf(FILE *f, const char *fmt, va_list ap) { REAL(vfprintf); SPY(); return real_vfprintf(f, fmt, ap); }
int fprintf(FILE *f, const char *fmt, ...) { va_list ap; int r; va_start(ap, fmt); r = vfprintf(f, fmt, ap); va_end(ap); return r; }
int printf(const char *fmt, ...) { va_list ap; int r; va_start(ap, fmt); r = vfprintf(stdout, fmt, ap); va_end(ap); return r; }

/* Mix `n` frames of silence + the replacement; positions[i] = the file
 * frame each output frame came from (-1 for silence). */
static int16_t g_out[200000 * 2];
static int16_t g_music_in[200000 * 2];     /* the core's music share (the dump's input) */
static void mix(int n, long *pos, double *gain) {
    int i;
    memset(g_out, 0, (size_t)n * 4);
    g_in_mix = 1;
    music_pack_mix(g_out, g_music_in, n);
    g_in_mix = 0;
    for (i = 0; i < n; i++) {
        int l = g_out[i * 2];
        if (pos) pos[i] = l == 0 ? -1 : lround((l + 16382.0) / 4.0);
        if (gain) gain[i] = l;
    }
}

/* The gain the ramp went through: its step is 4 per frame at gain 1. */
static double ramp_gain(const double *g, int n) {
    double d[4000];
    int i, k = 0, j, m;
    for (i = 1; i < n; i++) {
        double x = g[i] - g[i - 1];
        if (x > 0 && x < 32) d[k++] = x / 4.0;
    }
    for (i = 0; i < k; i++)                     /* median */
        for (j = i + 1; j < k; j++)
            if (d[j] < d[i]) { double t = d[i]; d[i] = d[j]; d[j] = t; }
    m = k / 2;
    return k ? d[m] : 0.0;
}

/* The file position `p` frames into the song with the sidecar's loop
 * (1000..3000) in force. */
static long loop_pos(long p) {
    return p < 3000 ? p : 1000 + (p - 1000) % 2000;
}

static void open_song(void) {
    _snd_openflag = 1;
    _ss_score[0] = SCORE_ADDR;
    memset(SCORE, 0, 168);
    *(u32 *)(SCORE + 8) = SEQ_ADDR + 16;      /* after the header and the first delta */
    VOL_L = VOL_R = 100;
    *(s16 *)(SCORE + 0x4A) = 2;
}

static int load_seq(const char *path) {
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f) return -1;
    n = fread((void *)(uintptr_t)SEQ_ADDR, 1, 65536, f);
    fclose(f);
    return n ? 0 : -1;
}

int main(int argc, char **argv) {
    static long pos[200000];
    const char *c;
    int i;
    if (argc != 3) return 90;
    c = argv[1];
    if (mmap((void *)0x80000000ul, 0x200000, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) return 91;
    if (strcmp(c, "decode") && load_seq(argv[2])) return 92;
    for (i = 0; i < 24; i++) owner(i, -1);

    if (!strcmp(c, "decode")) {               /* argv[2]: an audio file */
        MusicFileInfo info;
        MusicDecoder *d = music_decoder_open(argv[2], &info);
        int16_t buf[1024 * 2];
        long total = 0, n;
        double sum = 0;
        if (!d) { printf("open failed\n"); return 95; }
        while ((n = music_decoder_read(d, buf, 1024)) > 0) {
            for (i = 0; i < n * 2; i++) sum += buf[i] * (double)((total + i / 2) % 97);
            total += n;
        }
        music_decoder_seek(d, 1000);
        n = music_decoder_read(d, buf, 1);
        printf("%s rate %u channels %d frames %llu decoded %ld loop %lld %lld sum %.0f at1000 %d %d\n",
               info.format, (unsigned)info.rate, info.channels, (unsigned long long)info.frames,
               total, (long long)info.loop_start, (long long)info.loop_end, sum,
               n > 0 ? buf[0] : 0, n > 0 ? buf[1] : 0);
        music_decoder_close(d);
        return 0;
    }

    if (!strcmp(c, "menu")) {                 /* the MUSIC PACKS page */
        music_pack_menu_register();
        printf("page %s items %d\n", g_page_title ? g_page_title : "-", g_nitems);
        for (i = 0; i < g_nitems; i++) {
            const PcMenuItem *it = &g_items[i];
            int k;
            printf("item page=%d type=%d label=%s key=%s", it->page, it->type, it->label,
                   it->key ? it->key : "-");
            for (k = 0; it->choices && it->choices[k]; k++) printf(" [%s]", it->choices[k]);
            printf(" note=%s\n", it->note ? it->note(it->ctx) : "");
        }
        return 0;
    }

    if (!strcmp(c, "id")) {
        char id[16];
        if (music_pack_track_id((const u8 *)(uintptr_t)SEQ_ADDR, 65536, id, sizeof id)) return 93;
        printf("%s %u\n", id, music_pack_seq_span((const u8 *)(uintptr_t)SEQ_ADDR, 65536));
        return 0;
    }

    if (!strcmp(c, "follow")) {
        long p0, last;
        double g[4000];
        /* voices 0 and 3 play the song (tracks 0 and 1 of acc 0), voice 2
         * a sound effect (32), voice 5 another sequence (acc 1) */
        owner(0, 0x0000); owner(3, 0x0100); owner(2, 32); owner(5, 0x0001);
        open_song();
        tick();
        CHECK("open-only", g_split_calls == 0 || !g_split_on_, "split before the song plays");
        FLAGS = 1;                                  /* SsSeqPlay */
        tick();
        CHECK("mask", g_split_on_ && g_split_replace_ && g_split_mask_ == 0x9,
              "split on=%d replace=%d mask=0x%X, want 1 1 0x9", g_split_on_, g_split_replace_,
              g_split_mask_);
        mix(735, pos, NULL);
        CHECK("start-pos", pos[0] == 0 && pos[734] == 734, "positions %ld..%ld, want 0..734",
              pos[0], pos[734]);
        /* the voice owners change: the SE takes voice 0 */
        owner(0, 32);
        tick();
        CHECK("mask-follow", g_split_mask_ == 0x8, "mask 0x%X after voice 0 went to a sound effect",
              g_split_mask_);
        /* SsSeqSetVol / the driver's fade: half the starting volume */
        VOL_L = VOL_R = 50;
        tick();
        mix(1500, NULL, g);
        mix(200, NULL, g);
        {
            double r = ramp_gain(g, 200);
            CHECK("volume", fabs(r - 0.5) < 0.02, "gain %.3f at half the sequence volume", r);
        }
        VOL_L = VOL_R = 100;
        g_mainvol = 0x3FFF;                         /* SsSetMVol(63): half */
        tick();
        mix(1500, NULL, g);
        mix(200, NULL, g);
        {
            double r = ramp_gain(g, 200);
            CHECK("mainvol", fabs(r - 0.5) < 0.02, "gain %.3f at half the main volume", r);
        }
        g_mainvol = 0x7FFE;
        tick();
        mix(1000, pos, NULL);
        last = pos[999];
        /* SsSeqPause, 3,000 frames, SsSeqReplay */
        FLAGS = 2;
        tick();
        mix(3000, pos, NULL);
        CHECK("pause-silent", pos[2999] == -1 && pos[1500] == -1, "sound while paused");
        FLAGS = 1 | 8;
        tick();
        mix(1000, pos, NULL);
        /* positions only decode at full level: 900 frames after the resume,
         * past both 20 ms ramps (881 frames each), the file must be where it
         * was before the pause plus the two ramps -- not 3,000 frames on */
        p0 = loop_pos(last + 881 + 900);
        CHECK("pause-hold", labs(pos[899] - p0) <= 3,
              "resumed at %ld, want %ld (before the pause %ld)", pos[899], p0, last);
        /* loop points (the sidecar: 1000..3000) */
        mix(4000, pos, NULL);
        {
            int jumps = 0;
            long hi = 0;
            for (i = 1; i < 4000; i++) {
                if (pos[i] >= 0 && pos[i - 1] >= 0 && pos[i] < pos[i - 1]) {
                    jumps++;
                    CHECK("loop", pos[i - 1] == 2999 && pos[i] == 1000,
                          "jumped %ld -> %ld, want 2999 -> 1000", pos[i - 1], pos[i]);
                }
                if (pos[i] > hi) hi = pos[i];
            }
            CHECK("loop", jumps >= 1 && hi <= 2999, "%d loop jumps, highest frame %ld", jumps, hi);
        }
        /* the song ends by itself (end of track): the file plays out past
         * its loop end to its own end, then stops */
        FLAGS = 0x200 | 4;
        tick();
        mix(9000, pos, NULL);
        {
            long hi = 0;
            for (i = 0; i < 9000; i++) if (pos[i] > hi) hi = pos[i];
            CHECK("end-plays-out", hi >= 7990 && pos[8999] == -1,
                  "after the end: highest frame %ld, last %ld", hi, pos[8999]);
        }
        tick();
        /* SsSeqPlay again from the start, then SsSeqStop */
        FLAGS = 1;
        tick();
        mix(735, pos, NULL);
        CHECK("replay", pos[0] == 0, "replay started at %ld", pos[0]);
        FLAGS = 4;
        tick();
        CHECK("stop", !g_split_replace_ || !g_split_on_, "still replacing after SsSeqStop");
        mix(2000, pos, NULL);
        CHECK("stop", pos[1000] == -1 && pos[1999] == -1, "sound after SsSeqStop");
        /* SsSeqPlay, then SsSeqClose */
        FLAGS = 1;
        tick();
        CHECK("close", g_split_on_ && g_split_replace_, "not replacing after the replay");
        _snd_openflag = 0;
        tick();
        CHECK("close", !g_split_on_, "split on after SsSeqClose");
        mix(2000, pos, NULL);
        CHECK("close", pos[1999] == -1, "sound after SsSeqClose");
        return g_fail;
    }

    if (!strcmp(c, "io")) {                  /* a pack and the dump: no file I/O in the mixer */
        owner(0, 0);
        open_song();
        FLAGS = 1;
        tick();
        for (i = 0; i < 400; i++) {          /* ~9 s: the file loops, the dump grows */
            mix(1000, NULL, NULL);
            tick();
        }
        FLAGS = 4;
        tick();
        CHECK("io", g_io_in_mix == 0, "%d file calls inside music_pack_mix", g_io_in_mix);
        printf("io_in_mix %d\n", g_io_in_mix);
        return g_fail;
    }

    if (!strcmp(c, "samehandle")) {          /* another song at the same address (C4) */
        char want[16];
        owner(0, 0);
        open_song();
        FLAGS = 1;
        tick();
        FLAGS = 4;
        tick();
        /* the next bank's song is loaded where this one was: same handle */
        ((u8 *)(uintptr_t)SEQ_ADDR)[19] ^= 0x01;
        music_pack_track_id((const u8 *)(uintptr_t)SEQ_ADDR, 65536, want, sizeof want);
        FLAGS = 1;
        tick();
        CHECK("samehandle", !strcmp(g_acc[0].id, want), "playing %s, the bytes are %s", g_acc[0].id, want);
        return g_fail;
    }

    if (!strcmp(c, "afterload")) {           /* a state restore under the kept observer (C4) */
        int same = argc > 3;
        (void)same;
        owner(0, 0);
        open_song();
        FLAGS = 1;
        tick();
        mix(735, pos, NULL);
        CHECK("afterload", g_split_replace_, "the pack does not play");
        /* the restored machine plays another song at the same handle */
        ((u8 *)(uintptr_t)SEQ_ADDR)[19] ^= 0x01;
        if (music_pack_after_load) music_pack_after_load();
        tick();
        CHECK("afterload", !g_split_replace_, "still replacing the restored song with the old file");
        mix(2000, pos, NULL);
        CHECK("afterload", pos[1999] == -1, "the old file still sounds");
        return g_fail;
    }

    if (!strcmp(c, "afterload-same")) {      /* a rewind inside the same song: the file goes on */
        long before;
        owner(0, 0);
        open_song();
        FLAGS = 1;
        tick();
        mix(735, pos, NULL);
        before = pos[734];
        if (music_pack_after_load) music_pack_after_load();
        tick();
        mix(100, pos, NULL);
        CHECK("afterload-same", g_split_replace_ && pos[50] == before + 51,
              "after the restore: replacing %d, position %ld (was %ld)", g_split_replace_, pos[50], before);
        return g_fail;
    }

    if (!strcmp(c, "fallback")) {            /* the pack has no file for this song */
        owner(0, 0);
        open_song();
        FLAGS = 1;
        tick();
        CHECK("fallback", !g_split_on_ && !music_pack_mixing(), "split %d without a file", g_split_on_);
        return g_fail;
    }

    if (!strcmp(c, "dump")) {                 /* TOKI_MUSIC_DUMP=1, no pack */
        static int16_t music[1000 * 2];
        owner(0, 0);
        open_song();
        FLAGS = 1;
        tick();
        CHECK("dump", g_split_on_ && !g_split_replace_ && g_split_mask_ == 1,
              "dump split on=%d replace=%d mask=0x%X", g_split_on_, g_split_replace_, g_split_mask_);
        for (i = 0; i < 1000; i++) { music[i * 2] = (int16_t)i; music[i * 2 + 1] = (int16_t)-i; }
        for (i = 0; i < 3; i++) {
            memset(g_out, 0, 4000);
            music_pack_mix(g_out, music, 1000);
            CHECK("dump", g_out[500] == 0, "the dump changed the output");
        }
        FLAGS = 4;                            /* SsSeqStop: the take is closed */
        tick();
        /* a shorter second take must not replace the longer one */
        FLAGS = 1;
        tick();
        music_pack_mix(g_out, music, 1000);
        FLAGS = 4;
        tick();
        return g_fail;
    }
    return 94;
}
