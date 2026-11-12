/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Real-time SPU mixer under host audio cadences (pc/tools/test_audio_realtime.py).
 *
 * Links the actual pc/hal/spu.c (SPU_SOURCE), pc/hal/cd_audio.c and the
 * actual SPU core archive.  The audio device is simulated: SDL's lock is a
 * no-op, SDL_GetPerformanceCounter is a simulated wall clock, and the test
 * calls the real audio_callback (the device's) and spu_mmio_sync (the game
 * thread's) in wall-time order, the way a host would.
 *
 * The "game" plays 120 notes: at every even VBlank it keys a voice (eight
 * voices in turn) through the register page, at the odd VBlank after it
 * keys it off.  Each note is 16.7 ms of a constant sample with the fastest
 * attack and release, and 16.7 ms of silence separate two notes, so the
 * output has exactly one onset per note that sounded.
 *
 * argv[1] = game cadence:
 *   steady   a VBlank every 16.683 ms
 *   bursty   every sixth VBlank the host stalls 33 ms and delivers three
 *            VBlanks back to back (a Windows hitch)
 * argv[2] = device cadence:
 *   fixed    1,024 frames every 23.2 ms (PulseAudio, SDL's own buffer)
 *   odd      441/480/882/960/1,024/512-frame requests, delivered in pairs
 *            (the second right after the first, then a double gap): a
 *            WASAPI device at 48 kHz behind SDL's resampling stream, or a
 *            backend that pulls in its own period
 *
 * Prints "notes N onsets M kon_lost .. koff_lost .. stuck .. late .." and
 * exits 0 when every note sounded once and no key write was lost.
 * TOKI_SPU_LATENCY_MS=0 (direct register writes, the behaviour before
 * 2026-11-02) is the negative control: notes go missing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#ifndef SPU_SOURCE
#define SPU_SOURCE "../hal/spu.c"
#endif
#include SPU_SOURCE

static u64 g_sim_us;          /* simulated wall clock */
static u64 g_emu_ticks;       /* the game's emulated clock */

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
int parity_enabled(void) { return 0; }
static int g_ff;                        /* fast-forward held (pc/hal/enhance.c) */
int hal_ff_active(void) { return g_ff; }
u64 hal_clock_ticks(void) { return g_emu_ticks; }
u32 hal_clock_ticks_per_second(void) { return 33868800u; }

#define PAGE ((volatile u16 *)0x1F801C00ul)
#define NOTES 120
#define VBL_US 16683u
#define VBL_TICKS 565045u
#define MAX_FRAMES (44100 * 8)

static s16 g_out[MAX_FRAMES * 2];
static int g_out_n;

static void page_write(u32 off, u16 v) { PAGE[off / 2] = v; }

static void load_sample(void)
{
    u8 blocks[32];
    int i;
    memset(blocks, 0, sizeof(blocks));
    blocks[1] = 0x04;                                 /* loop start */
    for (i = 2; i < 16; i++) blocks[i] = 0x77;
    blocks[17] = 0x03;                                /* end + repeat */
    for (i = 18; i < 32; i++) blocks[i] = 0x77;
    spu_psx_dma_write(blocks, 0x2000, sizeof(blocks));
}

/* the game's VBlank v: key a note on (even v) or off (odd v) */
static void game_vblank(int v)
{
    int note = v / 2, voice = note % 8;
    u32 base = (u32)voice * 0x10;
    g_emu_ticks = (u64)v * VBL_TICKS;
    if (note >= NOTES) { spu_mmio_sync(); return; }
    if (!(v & 1)) {
        page_write(base + 0x0, 0x1000);
        page_write(base + 0x2, 0x1000);
        page_write(base + 0x4, 0x1000);
        page_write(base + 0x6, 0x2000 >> 3);
        page_write(base + 0x8, 0x000F);               /* fastest attack, full sustain */
        page_write(base + 0xA, 0x0000);               /* fastest release */
        page_write(0x188, (u16)(1u << voice));
    } else {
        page_write(0x18C, (u16)(1u << voice));
    }
    spu_mmio_sync();
}

/* ---- file I/O spy (case "io"): stdio calls made inside the callback ---- */
#include <dlfcn.h>
#include <stdarg.h>
static int g_in_callback, g_io_in_callback, g_direct;
#define REAL(name) static __typeof__(name) *real_##name; \
    if (!real_##name) real_##name = (__typeof__(name) *)dlsym(RTLD_NEXT, #name)
#define SPY() do { if (g_in_callback) g_io_in_callback++; } while (0)
FILE *fopen(const char *p, const char *m) { REAL(fopen); SPY(); return real_fopen(p, m); }
size_t fwrite(const void *b, size_t s, size_t n, FILE *f)
{ REAL(fwrite); SPY(); return real_fwrite(b, s, n, f); }
int fflush(FILE *f) { REAL(fflush); SPY(); return real_fflush(f); }
int fputs(const char *s, FILE *f) { REAL(fputs); SPY(); return real_fputs(s, f); }
int puts(const char *s) { REAL(puts); SPY(); return real_puts(s); }
int vfprintf(FILE *f, const char *fmt, va_list ap)
{ REAL(vfprintf); SPY(); return real_vfprintf(f, fmt, ap); }
int fprintf(FILE *f, const char *fmt, ...)
{ va_list ap; int r; va_start(ap, fmt); r = vfprintf(f, fmt, ap); va_end(ap); return r; }
int printf(const char *fmt, ...)
{ va_list ap; int r; va_start(ap, fmt); r = vfprintf(stdout, fmt, ap); va_end(ap); return r; }

static void device_callback(int frames)
{
    if (g_out_n + frames > MAX_FRAMES) frames = MAX_FRAMES - g_out_n;
    if (frames <= 0) return;
    g_in_callback = 1;
    /* g_direct (negative control of the spy): the game-thread mixer's
     * entry, which writes its files directly */
    audio_callback(g_direct ? NULL : &g_realtime_tag, (u8 *)(g_out + g_out_n * 2), frames * 4);
    g_in_callback = 0;
    g_out_n += frames;
}

static int onsets(void)
{
    /* an onset: the level passes 512 after at least 200 frames below 64 */
    int i, quiet = 1000, armed = 1, n = 0;
    for (i = 0; i < g_out_n; i++) {
        int a = abs(g_out[i * 2]);
        if (a < 64) { if (++quiet >= 200) armed = 1; continue; }
        quiet = 0;
        if (a > 512 && armed) { n++; armed = 0; }
    }
    return n;
}

/* when the host delivers VBlank v; bursty: 6k+1 and 6k+2 are late, sent
 * just before 6k+3 (a 33 ms stall, then three VBlanks at once) */
static u64 vblank_due(int v, int bursty)
{
    u64 t3 = (u64)(v - v % 6 + 3) * VBL_US;
    if (bursty && v % 6 == 1) return t3 - 200;
    if (bursty && v % 6 == 2) return t3 - 100;
    return (u64)v * VBL_US;
}

int main(int argc, char **argv)
{
    static const int odd_sizes[] = { 441, 480, 882, 960, 1024, 512 };
    int bursty, odd, v = 0, k = 0, notes_heard;
    u64 game_due, dev_due = 5000;
    int io;
    if (argc != 3) return 90;
    /* io device|direct: the steady/fixed run with PC_AUDIO_HASH and
     * PC_AUDIO_DUMP set (the test's environment); counts stdio calls made
     * inside the callback */
    io = !strcmp(argv[1], "io");
    g_direct = io && !strcmp(argv[2], "direct");
    bursty = !strcmp(argv[1], "bursty");
    odd = !strcmp(argv[2], "odd");
    if (io) audio_files_open();
    /* flush merged|plain: three syncs queue KON v1, KON v2, then KOFF v1 +
     * KON v3 with no mix in between; an SPU upload (spu_transfer_commit)
     * then applies the queue at once.  merged: the core must hold KON
     * v2|v3 (v1 keyed and released: cancelled), no KOFF lost.  plain (the
     * negative control): the same writes straight to the core keep only
     * the last KON. */
    /* STATE-MACHINES.md C1 / C6 / C7 / C12 (argv[1] restore|ffqueue|upload|resetwrite) */
    if (!strcmp(argv[1], "restore") || !strcmp(argv[1], "ffqueue") || !strcmp(argv[1], "resetwrite") ||
        !strcmp(argv[1], "upload")) {
        if (mmap((void *)0x1F801000ul, 0x1000, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) return 91;
        spu_psx_init();
        cd_audio_init();
        spu_psx_write_reg(0x1F801DAA, 0xC000);
        load_sample();
        g_audio_dev = 1;
        spu_mmio_start();
        if (!strcmp(argv[1], "restore")) {
            /* a KON queued in the old time line, then the machine restored to
             * an earlier moment: the KON must not reach the restored SPU */
            u16 pon;
            g_emu_ticks = 2000000; page_write(0x188, 1 << 1); spu_mmio_sync();
            if (g_rt.count < 1) { printf("not queued\n"); return 1; }
            if (!strcmp(argv[2], "hook")) spu_hal_after_restore();
            g_emu_ticks = 1000000; page_write(0x000, 0x0800); spu_mmio_sync();
            pon = spu_psx_read_reg(0x1F801D88);
            printf("pending KON 0x%04X dropped %u\n", pon, g_rt.dropped_restore);
            return pon == 0 && g_rt.dropped_restore >= 1 ? 0 : 1;
        }
        if (!strcmp(argv[1], "ffqueue")) {
            /* fast-forward: writes go straight to the core; the XA FIFO,
             * filled while it was held, keeps only its margin on the release */
            static u8 sec[2336];
            u32 queued, fifo_held, fifo_after, k;
            g_ff = 1;
            for (k = 0; k < 8; k++) {
                g_emu_ticks = 1000000 + (u64)k * VBL_TICKS;
                page_write(0x188, (u16)(1u << (k % 8)));
                spu_mmio_sync();
            }
            queued = g_rt.count;
            for (k = 0; k < sizeof sec; k++) sec[k] = (u8)(k * 37 + 11);
            for (k = 0; k < 4; k++) {
                sec[0] = sec[4] = 1; sec[1] = sec[5] = 1; sec[2] = sec[6] = 0x64; sec[3] = sec[7] = 0x01;
                cd_audio_xa_sector(sec, 0, g_emu_ticks);
            }
            g_sim_us = 1000;
            device_callback(512);
            fifo_held = cd_audio_fifo_frames();
            g_ff = 0;
            g_sim_us = 13000;
            device_callback(512);
            fifo_after = cd_audio_fifo_frames();
            printf("queued during ff %u fifo held %u after release %u margin %u\n", queued, fifo_held,
                   fifo_after, cd_audio_margin_frames());
            return queued == 0 && fifo_held > 5000 && fifo_after <= cd_audio_margin_frames() + 10 ? 0 : 1;
        }
        if (!strcmp(argv[1], "upload")) {
            /* a note keyed at one VBlank and released at the next, both still
             * queued when a bank upload commits: the note must still sound */
            static const u8 data[64];
            int k, heard = 0;
            page_write(0x000, 0x1000); page_write(0x002, 0x1000); page_write(0x004, 0x1000);
            page_write(0x006, 0x2000 >> 3); page_write(0x008, 0x000F); page_write(0x00A, 0x0000);
            page_write(0x180, 0x3FFF); page_write(0x182, 0x3FFF);
            g_emu_ticks = 1000000; g_sim_us = 0; page_write(0x188, 1); spu_mmio_sync();
            g_emu_ticks += VBL_TICKS; g_sim_us += VBL_US; page_write(0x18C, 1); spu_mmio_sync();
            spu_transfer_commit(data, 0x40000, sizeof data);
            for (k = 0; k < 20; k++) { g_sim_us += 23220; device_callback(1024); }
            for (k = 0; k < g_out_n; k++) if (abs(g_out[k * 2]) > 512) heard++;
            printf("cancelled %u heard %d frames\n", g_rt.flush_cancel, heard);
            return heard > 300 && !g_rt.flush_cancel ? 0 : 1;
        }
        /* resetwrite: a queued voice write, then a libspu reset write of the
         * same register: the reset is the last word */
        g_emu_ticks = 1000000; page_write(0x000, 0x1234); spu_mmio_sync();
        if (g_rt.count < 1) { printf("not queued\n"); return 1; }
        spu_reset_write(0x000, 0x0000);
        g_sim_us = 1000; device_callback(1024);
        g_sim_us = 200000; device_callback(1024);
        printf("voice 0 volume 0x%04X\n", spu_psx_read_reg(0x1F801C00));
        return spu_psx_read_reg(0x1F801C00) == 0 ? 0 : 1;
    }
    if (!strcmp(argv[1], "flush")) {
        static const u8 data[16];
        u16 pon;
        if (mmap((void *)0x1F801000ul, 0x1000, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) return 91;
        spu_psx_init();
        spu_psx_write_reg(0x1F801DAA, 0xC000);
        g_audio_dev = 1;
        spu_mmio_start();
        if (!strcmp(argv[2], "merged")) {
            g_emu_ticks = 1000000; page_write(0x188, 1 << 1); spu_mmio_sync();
            g_emu_ticks = 1565045; page_write(0x188, 1 << 2); spu_mmio_sync();
            g_emu_ticks = 2130090; page_write(0x18C, 1 << 1); page_write(0x188, 1 << 3); spu_mmio_sync();
            if (g_rt.count < 4) { printf("not queued (%u)\n", g_rt.count); return 1; }
            (void)data;
            spu_reset_read(0x1AE);            /* a register read applies the queue at once */
        } else {
            spu_core_write(0x188, 1 << 1);
            spu_core_write(0x188, 1 << 2);
            spu_core_write(0x18C, 1 << 1);
            spu_core_write(0x188, 1 << 3);
        }
        pon = spu_psx_read_reg(0x1F801D88);
        printf("pending KON 0x%04X KOFF 0x%04X\n", pon, spu_psx_read_reg(0x1F801D8C));
        return pon == ((1 << 2) | (1 << 3)) ? 0 : 1;
    }
    if (mmap((void *)0x1F801000ul, 0x1000, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) return 91;
    spu_psx_init();
    cd_audio_init();
    spu_psx_write_reg(0x1F801DAA, 0xC000);
    load_sample();
    g_audio_dev = 1;                                  /* a running real-time device */
    spu_mmio_start();
    page_write(0x180, 0x3FFF);
    page_write(0x182, 0x3FFF);
    game_due = 0;
    while (g_out_n < MAX_FRAMES && v < 2 * NOTES + 30) {
        if (game_due <= dev_due) {
            g_sim_us = game_due;
            game_vblank(v);
            v++;
            game_due = vblank_due(v, bursty);
        } else {
            int frames = odd ? odd_sizes[k % 6] : 1024;
            u64 period = (u64)frames * 1000000u / 44100u;
            g_sim_us = dev_due;
            device_callback(frames);
            /* odd: pairs -- the second request right after the first */
            if (odd) dev_due += (k & 1) ? 2 * period : 100;
            else dev_due += period;
            k++;
        }
    }
    if (io) {
        spu_audio_health_final();         /* the game thread's last drain */
        printf("io_in_callback %d callbacks %d hash_lines %lu\n", g_io_in_callback, k, calls);
        return g_io_in_callback ? 1 : 0;
    }
    notes_heard = onsets();
    printf("notes %d onsets %d kon_lost %u koff_lost %u stuck %u late %u resyncs %u queued %u latency %ld\n",
           NOTES, notes_heard, g_hl.kon_lost, g_hl.koff_lost, g_hl.kon_then_koff, g_rt.late, g_rt.resyncs,
           g_rt.applied, rt_latency_ms());
    return notes_heard == NOTES && !g_hl.kon_lost && !g_hl.koff_lost && !g_hl.kon_then_koff ? 0 : 1;
}
