/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SoundFont mode end to end, under host audio cadences
 * (pc/tools/test_soundfont.py, CadenceTests).
 *
 * Links the actual pc/hal/spu.c (SPU_SOURCE, included for its statics, as
 * pc/tests/spu_cadence_harness.c does), soundfont.c, music_pack.c (the
 * split's owner), cd_audio.c and the SPU core.  The device is simulated
 * (SDL's lock a no-op, a simulated wall clock); PS1 RAM is mapped at
 * 0x80000000 and holds a one-program VAB that libsnd's tables point at.
 *
 * The "game" plays 120 notes like the cadence harness: at every even VBlank
 * it fills a voice's libsnd record (a sequence note: owner = access 0,
 * note 69, program 0) and keys the voice, at the odd VBlank after it keys
 * it off.  The SPU sample is a constant (DC); the SoundFont (argv[3]: one
 * made by pc/tools/make_test_sf2.py, a 441 Hz sine at key 69) is what must
 * be heard: the split leaves the voices out.
 *
 * argv[1] = game cadence: steady | bursty | ff (VBlanks 40..160 arrive four
 *           times faster than real time: fast-forward) | det (the
 *           deterministic mixer: one mix per VBlank on the game thread)
 * argv[2] = device cadence: fixed | odd (as spu_cadence_harness.c)
 * argv[3] = the SF2
 *
 * Prints "notes N onsets M crossings C sf_on .. sf_keys .. unknown ..
 * kon_lost .. owned .. sounding .." and exits 0 when every note was heard
 * once from the SoundFont and no key write was lost. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#ifndef SPU_SOURCE
#define SPU_SOURCE "../hal/spu.c"
#endif
#include SPU_SOURCE
#include "soundfont.h"

static u64 g_sim_us;
static u64 g_emu_ticks;
static int g_det;

void SDLCALL SDL_LockAudioDevice(SDL_AudioDeviceID device) { (void)device; }
void SDLCALL SDL_UnlockAudioDevice(SDL_AudioDeviceID device) { (void)device; }
Uint64 SDLCALL SDL_GetPerformanceCounter(void) { return g_sim_us; }
Uint64 SDLCALL SDL_GetPerformanceFrequency(void) { return 1000000u; }
Uint32 SDLCALL SDL_GetTicks(void) { return (Uint32)(g_sim_us / 1000u); }
int pc_ptr_is_mapped(const void *p, unsigned long n)
{
    unsigned long a = (unsigned long)p;
    return a >= 0x1F801000ul && a + n <= 0x1F802000ul;
}
int parity_enabled(void) { return g_det; }
u64 hal_clock_ticks(void) { return g_emu_ticks; }
u32 hal_clock_ticks_per_second(void) { return 33868800u; }

/* libsnd's state (music_pack.c and soundfont.c read it) */
s32 _snd_openflag;
u32 _ss_score[16];
u8 _svm_voice[24 * 0x18];
u32 _svm_vab_vh[16], _svm_vab_tn[16];
u8 _svm_vab_used[16];

#define PAGE ((volatile u16 *)0x1F801C00ul)
#define NOTES 120
#define VBL_US 16683u
#define VBL_TICKS 565045u
#define MAX_FRAMES (44100 * 8)
#define VAB 0x80010000u

static s16 g_out[MAX_FRAMES * 2];
static int g_out_n;

static void page_write(u32 off, u16 v) { PAGE[off / 2] = v; }
static void put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }

static void make_vab(void)
{
    u8 *vh = (u8 *)(uintptr_t)VAB, *tn = vh + 32 + 2048;
    memset(vh, 0, 32 + 2048 + 16 * 32);
    memcpy(vh, "pBAV", 4);
    put16(vh + 18, 1);                      /* ps: one program */
    vh[32 + 0] = 1;                         /* program 0: one tone, tone row 0 */
    vh[32 + 1] = 127;
    tn[2] = 127; tn[3] = 64; tn[4] = 60; tn[6] = 0; tn[7] = 127;
    put16(tn + 16, 0x000F);
    put16(tn + 18, 0x1FC0);                 /* sustained (looped below) */
    put16(tn + 22, 1);
    _svm_vab_vh[0] = VAB;
    _svm_vab_tn[0] = VAB + 32 + 2048;
    _svm_vab_used[0] = 1;
}

static void load_sample(void)
{
    u8 blocks[32];
    int i;
    memset(blocks, 0, sizeof(blocks));
    blocks[1] = 0x04;
    for (i = 2; i < 16; i++) blocks[i] = 0x77;
    blocks[17] = 0x03;
    for (i = 18; i < 32; i++) blocks[i] = 0x77;
    spu_psx_dma_write(blocks, 0x2000, sizeof(blocks));
}

static void game_vblank(int v)
{
    int note = v / 2, voice = note % 8;
    u32 base = (u32)voice * 0x10;
    u8 *rec = _svm_voice + voice * 0x18;
    g_emu_ticks = (u64)v * VBL_TICKS;
    if (note >= NOTES) { spu_mmio_sync(); return; }
    if (!(v & 1)) {
        memset(rec, 0, 0x18);
        put16(rec + 0, 1);                  /* the VAG */
        put16(rec + 8, 69);                 /* the note */
        put16(rec + 10, 0x0000);            /* owner: track 0, access 0 */
        put16(rec + 12, 0);                 /* tone row */
        put16(rec + 14, 0);                 /* program */
        put16(rec + 16, 0);                 /* tone */
        put16(rec + 18, 0);                 /* VAB slot */
        page_write(base + 0x0, 0x1000);
        page_write(base + 0x2, 0x1000);
        page_write(base + 0x4, 0x1000);
        page_write(base + 0x6, 0x2000 >> 3);
        page_write(base + 0x8, 0x000F);
        page_write(base + 0xA, 0x0000);
        page_write(0x188, (u16)(1u << voice));
    } else {
        page_write(0x18C, (u16)(1u << voice));
    }
    spu_mmio_sync();
}

static void device_callback(int frames)
{
    if (g_out_n + frames > MAX_FRAMES) frames = MAX_FRAMES - g_out_n;
    if (frames <= 0) return;
    audio_callback(g_det ? NULL : &g_realtime_tag, (u8 *)(g_out + g_out_n * 2), frames * 4);
    g_out_n += frames;
}

/* an onset: the level passes 512 after at least 200 frames below 64 */
static int onsets(void)
{
    int i, quiet = 1000, armed = 1, n = 0;
    for (i = 0; i < g_out_n; i++) {
        int a = abs(g_out[i * 2]);
        if (a < 64) { if (++quiet >= 200) armed = 1; continue; }
        quiet = 0;
        if (a > 512 && armed) { n++; armed = 0; }
    }
    return n;
}

static int crossings(void)
{
    int i, n = 0;
    for (i = 1; i < g_out_n; i++)
        if ((g_out[i * 2] < 0) != (g_out[i * 2 - 2] < 0)) n++;
    return n;
}

static u64 vblank_due(int v, const char *mode)
{
    if (!strcmp(mode, "bursty")) {
        u64 t3 = (u64)(v - v % 6 + 3) * VBL_US;
        if (v % 6 == 1) return t3 - 200;
        if (v % 6 == 2) return t3 - 100;
    }
    if (!strcmp(mode, "ff")) {
        /* 40 VBlanks in real time, the next 120 at 4x, then real time */
        if (v <= 40) return (u64)v * VBL_US;
        if (v <= 160) return 40ull * VBL_US + (u64)(v - 40) * VBL_US / 4;
        return 40ull * VBL_US + 30ull * VBL_US + (u64)(v - 160) * VBL_US;
    }
    return (u64)v * VBL_US;
}

int main(int argc, char **argv)
{
    static const int odd_sizes[] = { 441, 480, 882, 960, 1024, 512 };
    SoundfontHealth h;
    int odd, v = 0, k = 0, heard, cross, sounding = 0, i;
    u64 game_due, dev_due = 5000;
    if (argc != 4) return 90;
    odd = !strcmp(argv[2], "odd");
    g_det = !strcmp(argv[1], "det");
    setenv("TOKI_SOUNDFONT", argv[3], 1);
    if (mmap((void *)0x1F801000ul, 0x1000, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) return 91;
    if (mmap((void *)0x80000000ul, 0x200000, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) return 92;
    spu_psx_init();
    cd_audio_init();
    spu_psx_write_reg(0x1F801DAA, 0xC000);
    load_sample();
    make_vab();
    g_audio_dev = 1;
    spu_mmio_start();
    page_write(0x180, 0x3FFF);
    page_write(0x182, 0x3FFF);
    soundfont_tick();                       /* loads the SF2 */
    if (!soundfont_rendering()) { printf("SF2 not loaded\n"); return 93; }
    game_due = 0;
    if (g_det) {
        /* the game-thread mixer: each VBlank, then its 735 frames */
        for (v = 0; v < 2 * NOTES + 30 && g_out_n + 735 <= MAX_FRAMES; v++) {
            game_vblank(v);
            device_callback(735);
        }
    } else {
        while (g_out_n < MAX_FRAMES && v < 2 * NOTES + 30) {
            if (game_due <= dev_due) {
                g_sim_us = game_due;
                game_vblank(v);
                v++;
                game_due = vblank_due(v, argv[1]);
            } else {
                int frames = odd ? odd_sizes[k % 6] : 1024;
                u64 period = (u64)frames * 1000000u / 44100u;
                g_sim_us = dev_due;
                device_callback(frames);
                if (odd) dev_due += (k & 1) ? 2 * period : 100;
                else dev_due += period;
                k++;
            }
        }
        /* let the queue drain */
        for (i = 0; i < 40; i++) { g_sim_us = dev_due; device_callback(1024); dev_due += 23220; }
    }
    soundfont_health(&h);
    sounding = __builtin_popcount(soundfont_notes_mask());
    heard = onsets();
    cross = crossings();
    printf("notes %d onsets %d crossings %d sf_on %u sf_keys %u unknown %u kon_lost %u koff_lost %u owned %06x "
           "sounding %d\n", NOTES, heard, cross, h.notes_on, h.music_keys, h.unknown_tag, g_hl.kon_lost,
           g_hl.koff_lost, (unsigned)soundfont_split_mask(), sounding);
    if (!strcmp(argv[1], "ff"))       /* squeezed notes may merge: every one keyed, none lost */
        return h.notes_on == NOTES && h.music_keys == NOTES && !h.unknown_tag && !g_hl.kon_lost &&
               !sounding && cross > 1000 ? 0 : 1;
    return heard == NOTES && h.notes_on == NOTES && h.music_keys == NOTES && !h.unknown_tag &&
           !g_hl.kon_lost && !g_hl.koff_lost && !sounding && cross > 1000 ? 0 : 1;
}
