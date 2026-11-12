/* SPDX-License-Identifier: GPL-3.0-or-later */
/* cd.c -- CD-ROM HAL: the Psy-Q libcd API over a tick-accurate drive model.
 *
 * The game never touches the drive directly: every load goes through the
 * matched CD queue (cd_read_start / cd_read_start2 -> cd_command_issue ->
 * cdq_command_enqueue; the worker cdq_command_process runs once per frame
 * from the main loop and issues libcd calls -- CdControl / CdControlB /
 * CdRead / CdSync / CdReadSync -- then sets cdq_head.flags (= cd_state2) when
 * a read has landed).  The scene code polls get_CD_State2() & 1.  So the
 * PC needs libcd's *contract* and the drive's *timing*; it does not need the
 * CD-ROM controller registers that the matched libcd internals (CD_cw,
 * CD_sync, getintr, callback) talk to.
 *
 * Until 2026-10-15 the HAL completed every operation instantly and the
 * pc-port overrides of cd_read_start2 / get_CD_State2 / title_data_read
 * loaded the file synchronously and set the ready flag at once, so every
 * wait in the game collapsed: boot_data_load took 3 frames where the PS1
 * takes 157, title_data_read 1 frame where the PS1 takes 270.  Those
 * overrides are gone; this file is what the matched queue runs on.
 *
 * TIMING MODEL
 *   A tick counter at the PS1 master clock (33.8688 MHz) is the system
 *   clock: VSync(0) (pc/hal/system.c) waits for its next VBlank
 *   (cd_hal_vblank_tick), and every VBlank it crosses raises the VBlank
 *   interrupt (hal_vblank_interrupt: kernel VBlank events, pad poll, trace),
 *   and further whenever libcd would have spun waiting on the drive
 *   (CdControl waits for the command's acknowledge, CdControlB / CdSync(0)
 *   for its completion).  The per-command costs are the reference
 *   drive's, measured as a black box from its command/seek/sector trace
 *   (and psx-spx "CDROM Drive", Response Timings), so a PC run and a
 *   reference run of the same script agree closely on when each load
 *   completes:
 *     acknowledge      25000 ticks (80000 for Init)
 *     second response  Pause: 7400 idle, else the rest of the revolution
 *                      (pause_ticks), min 1,000,000 (2x) / 2,000,000 (1x)
 *                      Init: max(speed change + seek to 0, 4,000,000)
 *                      Seek/Read: distance-based (wait on the revolution,
 *                      one track back, 50/100 ms jumps, sled moves;
 *                      seek_ticks_model) + 0.75 ms settling (a drive
 *                      varies by 0.5-1 ms; the midpoint keeps runs
 *                      deterministic)
 *     speed change     0.6 s to 2x, 0.7 s to 1x (Setmode bit 7)
 *     sector period    MASTER/150 at 2x, MASTER/75 at 1x
 *   The drive keeps reading after the last requested sector (ReadN runs
 *   until Pause), which is why the game's pause-before-read costs a track.
 *
 * libcd CONTRACT (from the matched CD_cw / CD_sync / getintr / CD_readsync):
 *   CdControl   issues after the previous command has COMPLETED (CD_cw calls
 *               CD_sync(0) first), then blocks until the acknowledge; 1 = ok
 *   CdControlB  as CdControl, then waits for completion; 1 = ok
 *   CdControlF  issues, returns at once
 *   CdSync(1)   0 while a two-response command (Pause/Init/Seek/Stop) has
 *               not completed, 2 (CdlComplete) otherwise, 5 on error;
 *               result[0] = the drive status byte (0x40 seeking, 0x20
 *               reading, 0x80 playing, 0x02 motor on)
 *   CdRead      ReadN of N sectors from the last Setloc into the buffer;
 *               each sector is copied in as it arrives
 *   CdReadSync(1) sectors still to arrive, 0 when the buffer is complete
 *
 * DATA: a sector is read from the extracted disc tree through the LBA table
 * (pc/src/file_lba_table.c, pc/hal/file_id.c), so the bytes -- including the
 * checksum footer the queue worker verifies -- are the disc's.  The
 * destination is a PS1 address; it must fall in one of the RAM windows
 * pc/src/main.c maps at their PS1 addresses (0x80010000-0x80041000 for the
 * sound driver's VAB/SEQ buffers, 0x80132000-0x80232000 for everything else).
 *
 * Remaining divergences: raw IRQ/register dispatch, callback firing and
 * hardware-error/retry injection are incomplete. Not modelled: read
 * errors / retries, CD-DA audio (CdPlay is a no-op; the game never plays
 * the audio track), the per-seek settling variance (seek_jitter uses
 * its midpoint).
 *
 * XA AUDIO: with Setmode bit 6 a realtime audio sector of a raw Mode 2 file
 * (.XA, .STR) is not delivered: it goes to the controller's ADPCM decoder
 * (pc/hal/cd_audio.c, Setfilter file/channel when Setmode bit 3 is set) at
 * the tick the drive reads it, and from there to the SPU's CD input.
 * GetlocL returns that sector's header, which the game's
 * check_head_position follows to end a voice line.
 *
 * Credit: the seek model's structure (wait on the revolution, one track
 * back, short and long jumps, sled moves) follows the CD drive timing
 * research in DuckStation, (C) Connor McLaughlin, GPL-3.0; the constants
 * and formulas here are this project's own fit.
 */
#include "spu_transfer.h"
#include "pc_data_root.h"
#include "cd_audio.h"
/* The controller's audio path (cd_audio.c) is weak here so the harnesses
 * that link the drive model alone still build; they get no XA audio. */
#pragma weak cd_audio_reset_decoder
#pragma weak cd_audio_set_filter
#pragma weak cd_audio_set_mute
#pragma weak cd_audio_set_volume
#pragma weak cd_audio_xa_sector
/* The enhanced backlog's voice capture (pc/hal/backlog_capture.c): it sees
 * each realtime ADPCM sector before the controller does; read only. */
#include "pc_backlog.h"
#pragma weak backlog_xa_sector
#define XA_CALL(fn, args) do { if (fn) fn args; } while (0)
#include "bios_printf.h"
#include "cpu_cost.h"
#include "profile.h"
#include "hal.h"
#include "nbn_trace.h"
#include "file_id.h"
#include "lang.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

extern const u32 g_file_lba_table_count;

/* ------------------------------------------------------------------ constants */
#define MASTER_CLOCK        33868800u
/* CPU ticks per NTSC frame: 263 lines x 3413 GPU clocks at 53.693175 MHz */
#define TICKS_PER_FRAME     566204u   /* measured on the reference: 566,203.8 */
#define ACK_TICKS           25000u
#define ACK_TICKS_INIT      80000u
#define INIT_TICKS          4000000u
#define PAUSE_IDLE_TICKS    7400u
#define SEEK_JITTER_TICKS   (MASTER_CLOCK / 1333)       /* 0.75 ms */
#define SUBQ_SECTOR_SKEW    2          /* a logical seek lands two sectors before its target */
#define LBA_PREGAP          150        /* the LBA table is ISO (0 = first data sector); the mech tables want absolute MSF */
#define SECTOR_SIZE         2048

/* libcd command numbers (CdlNop .. CdlReadS) */
enum {
    CMD_GETSTAT = 1, CMD_SETLOC = 2, CMD_PLAY = 3, CMD_FORWARD = 4, CMD_BACKWARD = 5,
    CMD_READN = 6, CMD_MOTORON = 7, CMD_STOP = 8, CMD_PAUSE = 9, CMD_INIT = 10,
    CMD_MUTE = 11, CMD_DEMUTE = 12, CMD_SETFILTER = 13, CMD_SETMODE = 14,
    CMD_GETPARAM = 15, CMD_GETLOCL = 16, CMD_GETLOCP = 17, CMD_SETSESSION = 18,
    CMD_GETTN = 19, CMD_GETTD = 20, CMD_SEEKL = 21, CMD_SEEKP = 22,
    CMD_TEST = 25, CMD_GETID = 26, CMD_READS = 27, CMD_RESET = 28
};

/* drive status byte */
#define STAT_ERROR      0x01
#define STAT_MOTOR_ON   0x02
#define STAT_READING    0x20
#define STAT_SEEKING    0x40
#define STAT_PLAYING    0x80

typedef enum { DRIVE_IDLE, DRIVE_SPEEDCHANGE, DRIVE_SEEKING, DRIVE_READING, DRIVE_PLAYING } DriveState;

/* ------------------------------------------------------------------ state */
static u64 g_now;             /* ticks */
static s32 pos_to_int(void *pos);
static int g_frames;          /* VBlanks seen (diagnostics) */

/* command responses */
static int g_cmd_pending;     /* command waiting for its acknowledge (0 none) */
static u64 g_ack_at;
static int g_second_cmd;      /* command waiting for its second response (0 none) */
static u64 g_second_at;
static int g_sync_status;     /* what CdSync(1) returns: 0 busy, 2 complete, 5 error */
static u8  g_result[8];       /* last response bytes (result[0] = status) */

/* drive */
static DriveState g_drive;
static u64 g_drive_at;        /* next drive event (seek end, speed change end, next sector) */
static u64 g_drive_last;      /* tick of the last sector event (for the pause formula) */
static u32 g_cur_lba;         /* logical position: while reading, the next sector to be read */
static u32 g_subq;            /* physical head position (the subchannel Q position): the last
                               * sector read; target-2 after a logical seek; while holding after
                               * a pause it cycles around the track (subq_now()) */
static u64 g_subq_tick;       /* when g_subq was last computed (hold cycling) */
static u64 g_subq_carry;
static int g_reset_in_pregap; /* physical head is at reset origin, not ISO 0 */
static int g_header_valid;    /* a data sector header has been read (hold offset +2) */
static int g_implicit_seek;   /* a read's own seek is in progress: status shows SEEKING until the first sector */
static u32 g_seek_target;
static int g_read_after_seek; /* the seek in progress was started by ReadN/ReadS/Play */
static int g_play_after_seek;
static int g_autopause_due;   /* libcd cb_read: Pause the drive when the requested sectors are in */
static int g_last_cmd;        /* the command CD_cw wrote last (wait_ack's return path) */
static int g_control_call;    /* CdControl/CdControlB is waiting on CD_cw */
static u32 g_setloc_lba;
static int g_setloc_pending;
static int g_double_speed;
static u8  g_mode;
static int g_motor_on = 1;

/* ReadN transfer (CdRead / CdReadSync) */
static int g_xfer_active;     /* a CdRead is outstanding */
static s32 g_xfer_remaining;  /* sectors still to arrive (CdReadSync value) */
static u32 g_xfer_ps1_dest;   /* PS1 address the next sector lands at */
static u32 g_xfer_total;
static u32 g_xfer_first_lba;
static int g_sector_irq;      /* drive_event: the sector interrupted the CPU (1 CdRead, 2 libstr stream) */
static int g_stream_active;   /* libstr's ReadS stream is running */
static u8  g_last_header[8];  /* GetlocL: the last data sector's header (MSF, mode) and subheader */
extern int libpress_stream_sector_irq(u32 lba, u8 mode) __attribute__((weak));
extern int libpress_mdec_out_busy(u64 t) __attribute__((weak));

/* callbacks the game registers (kept; the model does not fire them) */
static void *g_cb_ready, *g_cb_sync, *g_cb_data, *g_cb_read;

/* CdSearchFile / stream source */
static char g_cd_current_file[256];

/* cached sector source */
static FILE *g_sector_file;
static char g_sector_path[1024];
static u32 g_sector_file_lba, g_sector_file_nsec;
static long g_sector_file_size = -1;

static int g_debug = -1;
static int cd_debug(void) {
    if (g_debug < 0) {
        const char *e = getenv("PC_DEBUG");
        g_debug = (e && (strstr(e, "cd") || !strcmp(e, "all") || !strcmp(e, "1"))) ? 1 : 0;
    }
    return g_debug;
}
#define CDLOG(...) do { if (cd_debug()) { fprintf(stderr, "[HAL/CD f%d t%llu] ", g_frames, (unsigned long long)g_now); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* ------------------------------------------------------------------ helpers */
static u8 drive_stat(void) {
    u8 s = g_motor_on ? STAT_MOTOR_ON : 0;
    if (g_drive == DRIVE_SEEKING || (g_drive == DRIVE_READING && g_implicit_seek)) s |= STAT_SEEKING;
    else if (g_drive == DRIVE_READING) s |= STAT_READING;
    else if (g_drive == DRIVE_PLAYING) s |= STAT_PLAYING;
    return s;
}

static u32 ticks_per_sector(void) { return g_double_speed ? MASTER_CLOCK / 150 : MASTER_CLOCK / 75; }

/* CD_DRIVE_TIMING=0 -- instant loads (the enhanced profile, pc/hal/profile.c;
 * docs/pc-port/ENHANCED-MODE.md).  The mechanical times collapse to
 * INSTANT_TICKS: seeks and spin-up, speed changes, Pause, Init, Stop.  The
 * sectors of a data read (a CdRead of a cooked 2,048-byte file, not a libstr
 * stream) arrive INSTANT_TICKS apart instead of 1/150 s.  Everything else
 * is unchanged: the command protocol (acknowledges, second responses,
 * interrupts, libcd's auto-pause), and the sector rate of anything read
 * raw -- the XA voice and the movie streams stay real-time, because the
 * game ends a voice line by following the drive's position (GetlocL) and
 * paces the movie on its sectors.  Parity (=1) never reaches these paths. */
#define INSTANT_TICKS 2000u
static int drive_timing(void) {
    static int on = -1;
    if (on < 0) on = hal_model_on("CD_DRIVE_TIMING");
    return on;
}
static int lba_is_raw(u32 lba);
/* The time to the next sector of a ReadN: one sector period, or
 * INSTANT_TICKS while a CdRead of a cooked file is outstanding and the
 * drive model is off.  `raw` < 0: look the sector at `lba` up. */
static u32 read_sector_ticks(u32 lba, int raw) {
    if (!drive_timing() && g_xfer_active && g_xfer_remaining > 0 && !g_stream_active &&
        !(raw < 0 ? lba_is_raw(lba) : raw))
        return INSTANT_TICKS;
    return ticks_per_sector();
}
/* CD_WAIT_MUSIC_FADE (enhanced; only with CD_DRIVE_TIMING=0): a data read
 * holds its next sector while the game is fading its music out.
 *
 * At a scene change the game starts a fade-out of the current song
 * (seq_cmd -> seq_vol_fade_set; the sound driver lowers seq_vol_l once per
 * main-loop frame) and loads the next scene.  When the next scene's sound
 * bank arrives, SD_VabLoadCmd -> SD_VABClose stops the song on that bank
 * slot at whatever volume the fade has reached, and the new song starts a
 * few frames later.  On the console the drive is always slower than the fade
 * (parity, new_game: fade 2668 -> 2876, bank upload 2987), so the song has
 * faded to silence first.  With instant loads the bank arrived mid-fade: the
 * old song was cut at volume 23, 6 or even 83 (19 VBlanks into its fade) and
 * the new one started on top of its release and reverb tails -- the "music
 * overlapping" of the enhanced profile.  Holding the read until the fade is
 * over restores the console's order of events; nothing else waits.
 *
 * The fade is the sound driver's own state, read only: a song is playing
 * (gSDInfo bit 0x100) and fading (gSDSerialInfo bit 0).  Safety: the hold
 * ends as soon as seq_vol_l has not moved for half a second (a blocking
 * wait that stops the main loop also stops the fade) and after 8 s at most;
 * a read that was released is not held again.  Weak: the CD harnesses link
 * this file without the game's globals. */
extern s32 gSDInfo[] __attribute__((weak));
extern s32 gSDSerialInfo __attribute__((weak));
extern s16 seq_vol_l __attribute__((weak));
static int g_fade_hold;                 /* the current read is being held */
static int g_fade_released;             /* ... and was let go: do not hold again */
static u64 g_fade_hold_start, g_fade_vol_at;
static s16 g_fade_vol;
static int music_fade_wait(void) {
    static int on = -1;
    if (on < 0) on = hal_feature_on("CD_WAIT_MUSIC_FADE");
    return on;
}
static int music_fading(void) {
    return &gSDInfo[0] && &gSDSerialInfo && (gSDInfo[0] & 0x100) && (gSDSerialInfo & 1);
}
/* 1: keep the next sector of the outstanding cooked read back for now */
static int fade_hold_sector(void) {
    if (drive_timing() || !music_fade_wait() || !g_xfer_active || g_xfer_remaining == 0 ||
        g_stream_active || g_fade_released || lba_is_raw(g_cur_lba) || !music_fading()) {
        if (g_fade_hold) CDLOG("music fade over: read continues");
        g_fade_hold = 0;
        return 0;
    }
    if (!g_fade_hold) {
        g_fade_hold = 1;
        g_fade_hold_start = g_fade_vol_at = g_now;
        g_fade_vol = &seq_vol_l ? seq_vol_l : 0;
        CDLOG("read held: the music is fading out (volume %d)", g_fade_vol);
    }
    if (&seq_vol_l && seq_vol_l != g_fade_vol) {
        g_fade_vol = seq_vol_l;
        g_fade_vol_at = g_now;
    }
    if (g_now - g_fade_vol_at > MASTER_CLOCK / 2 || g_now - g_fade_hold_start > 8ull * MASTER_CLOCK) {
        CDLOG("read released: the fade is not progressing");
        g_fade_hold = 0;
        g_fade_released = 1;
        return 0;
    }
    return 1;
}

/* ---- drive mechanics ----
 * An independent model of the drive's mechanical delays: speed changes,
 * the track the head holds on, seeks and Pause.  Sources: psx-spx
 * "CDROM Drive" (Response Timings; SeekL/SeekP: after a seek the head
 * holds a few sectors behind its target; read rate 75/150 sectors per
 * second), the constant-linear-velocity disc geometry (the radius, and so
 * the sectors per revolution, grow with the square root of the playing
 * time), and the reference drive's timings measured as a black box from
 * its command/seek/sector trace over boot, new_game and date_save
 * (2026-11-06; 154 seeks, 219 Pauses).  All positions below are absolute
 * sectors (ISO LBA + LBA_PREGAP). */

#define MS_TICKS(ms)          ((u64)MASTER_CLOCK * (ms) / 1000u)
#define SPEED_UP_TICKS        (MASTER_CLOCK * 6u / 10u)    /* to 2x: 0.6 s, measured */
#define SPEED_DOWN_TICKS      (MASTER_CLOCK * 7u / 10u)    /* to 1x: 0.7 s, measured */
#define SEEK_SLED_SECTORS     6300    /* from here: a sled move */
#define PAUSE_MIN_TICKS_2X    1000000u
#define PAUSE_MIN_TICKS_1X    2000000u

static u32 speed_change_ticks(void) {
    if (!drive_timing()) return INSTANT_TICKS;
    return g_double_speed ? SPEED_UP_TICKS : SPEED_DOWN_TICKS;
}

/* Sectors in one revolution at an absolute position: proportional to the
 * radius, which on a CLV spiral grows as sqrt(t + t0) with t the playing
 * time.  Evaluated per whole disc minute; 2.31 and 12.9 minutes fit every
 * revolution length measured on the reference (8 sectors at minute 0,
 * 9 at 3-4, 11 at 8-11, 12 at 12-15, 13 at 18, 19 at 58, 20 at 62-63). */
static u32 spt_abs(s64 abs_sector) {
    double minute = abs_sector > 0 ? (double)(abs_sector / 4500) : 0.0;
    return (u32)(2.31 * sqrt(minute + 12.9) + 0.5);
}
static u32 sectors_per_track(u32 lba) {
    return spt_abs((s64)lba + LBA_PREGAP);
}

/* The physical head (ISO LBA).  While the drive holds -- idle, motor on,
 * after a data sector -- it plays one revolution forward from g_subq and
 * jumps back (psx-spx SeekL: "stays at around N-8..N-0"), one sector per
 * sector period. */
static u32 subq_now(void) {
    u32 spt, n;
    if (g_drive != DRIVE_IDLE || !g_motor_on || !g_header_valid || g_now < g_subq_tick)
        return g_subq;
    spt = sectors_per_track(g_subq);
    n = (u32)((g_now - g_subq_tick) / ticks_per_sector());
    return g_subq + (spt ? n % spt : 0);
}

/* The settling time a real drive adds to every seek varies by 0.5..1 ms
 * between seeks; the model uses its midpoint, so runs are deterministic. */
static u32 seek_jitter(void) {
    return SEEK_JITTER_TICKS;
}

/* A sled move: a power law in the distance, fitted to the reference's
 * long seeks (7,503..260,185 sectors: 10.2..26.9 M ticks, rms 61 k). */
static u64 sled_ticks(u64 sectors) {
    return 8948243u + (u64)(1983.85 * pow((double)sectors, 0.73));
}

/* The reach of the short (50 ms) track jump, in sectors: it shrinks
 * towards the rim.  Fitted to the reference: about 300 sectors at minute
 * 0 (248 short, 507 not), 200 at minute 9 (111 short, 239 not), under 90
 * past minute 60. */
static s64 short_jump_sectors(s64 abs_sector) {
    s64 minute = abs_sector > 0 ? abs_sector / 4500 : 0;
    return 5400 / (minute + 18);
}

/* Time from now until the head reads the sector before `iso_target`
 * (absolute target = iso_target + LBA_PREGAP; the reset origin is -150).
 * A speed change in progress completes first unless ignored. */
static u64 seek_ticks_model(s32 iso_target, int skip_speed_wait) {
    s64 cur = g_reset_in_pregap ? 0 : (s64)subq_now() + LBA_PREGAP;
    s64 tgt = (s64)iso_target + LBA_PREGAP;
    s64 d = tgt - cur;
    s64 dist = d < 0 ? -d : d;
    u32 spt = spt_abs(cur);
    u64 t = 0;
    if (!skip_speed_wait && g_drive == DRIVE_SPEEDCHANGE && g_drive_at > g_now)
        t += g_drive_at - g_now;
    if (d > 0 && d < (s64)spt)              /* ahead on this revolution: wait for it */
        t += (u64)d * ticks_per_sector();
    else if (d <= 0 && -d < (s64)spt)       /* just passed: one track back, then wait */
        t += (u64)(spt + d) * ticks_per_sector();
    else if (dist < short_jump_sectors(cur))
        t += MS_TICKS(50);
    else if (dist < SEEK_SLED_SECTORS)
        t += MS_TICKS(100);
    else
        t += sled_ticks((u64)dist);
    return t + seek_jitter();
}
static u64 seek_ticks(s32 iso_target, int skip_speed_wait) {
    if (!drive_timing()) return INSTANT_TICKS;
    return seek_ticks_model(iso_target, skip_speed_wait);
}

/* Pause's second response.  Idle: the measured 7,400 ticks (psx-spx
 * "Pause (when paused)": 7,666 average on hardware).  Reading or playing:
 * the drive finishes the revolution it is on and holds there (psx-spx:
 * about five sectors at the start of the disc); measured on the
 * reference as two sectors short of one revolution after the last sector
 * read, at least 1 M ticks at 2x (2 M at 1x). */
static u64 pause_ticks(void) {
    u64 since, t, rev;
    if (!drive_timing()) return INSTANT_TICKS;
    if (g_drive != DRIVE_READING && g_drive != DRIVE_PLAYING) return PAUSE_IDLE_TICKS;
    rev = (u64)(sectors_per_track(g_subq) - 2) * ticks_per_sector();
    since = g_now > g_drive_last ? g_now - g_drive_last : 0;
    t = rev > since ? rev - since : 0;
    if (t < (g_double_speed ? PAUSE_MIN_TICKS_2X : PAUSE_MIN_TICKS_1X))
        t = g_double_speed ? PAUSE_MIN_TICKS_2X : PAUSE_MIN_TICKS_1X;
    return t;
}

/* ---- sector source ---- */
static int open_sector_source(u32 lba) {
    const FileLbaEntry *e = file_id_lookup_lba(lba);
    u32 i;
    if (!e) {
        for (i = 0; i < g_file_lba_table_count; i++) {
            const FileLbaEntry *c = &g_file_lba_table[i];
            u32 nsec = (c->size + 2047) / 2048;
            if (lba >= c->lba && lba < c->lba + nsec) { e = c; break; }
        }
    }
    if (!e) return 0;
    if (g_sector_file && g_sector_file_lba == e->lba) return 1;
    if (g_sector_file) fclose(g_sector_file);
    snprintf(g_sector_path, sizeof g_sector_path, "%s/raw/%s", pc_asset_dir(), e->path);
    {
        /* [game] language = en: the English copy, when the file has one */
        char en[sizeof g_sector_path];
        const char *p = lang_path(g_sector_path, en, sizeof en);
        if (p != g_sector_path) snprintf(g_sector_path, sizeof g_sector_path, "%s", p);
    }
    g_sector_file = fopen(g_sector_path, "rb");
    g_sector_file_size = -1;
    g_sector_file_lba = e->lba;
    g_sector_file_nsec = (e->size + 2047) / 2048;
    return g_sector_file != NULL;
}

/* The raw Mode 2 sector (subheader + 2,324 bytes) at `lba`, when the file
 * holding it is stored raw (.XA / .STR: CD_AUDIO_RAW_SECTOR bytes a sector
 * on disk; the other files are their 2,048-byte Form 1 user data only). */
static int read_raw_sector(u32 lba, u8 *raw) {
    if (!open_sector_source(lba) || lba < g_sector_file_lba || lba >= g_sector_file_lba + g_sector_file_nsec)
        return 0;
    if (g_sector_file_size < 0) {
        fseek(g_sector_file, 0, SEEK_END);
        g_sector_file_size = ftell(g_sector_file);
    }
    if (g_sector_file_size != (long)g_sector_file_nsec * CD_AUDIO_RAW_SECTOR) return 0;
    fseek(g_sector_file, (long)(lba - g_sector_file_lba) * CD_AUDIO_RAW_SECTOR, SEEK_SET);
    return fread(raw, 1, CD_AUDIO_RAW_SECTOR, g_sector_file) == CD_AUDIO_RAW_SECTOR;
}
/* Is `lba` in a file stored raw (as read_raw_sector decides)?  Only the
 * instant-load path (CD_DRIVE_TIMING=0) asks. */
static int lba_is_raw(u32 lba) {
    if (!open_sector_source(lba) || lba < g_sector_file_lba || lba >= g_sector_file_lba + g_sector_file_nsec)
        return 0;
    if (g_sector_file_size < 0) {
        fseek(g_sector_file, 0, SEEK_END);
        g_sector_file_size = ftell(g_sector_file);
    }
    return g_sector_file_size == (long)g_sector_file_nsec * CD_AUDIO_RAW_SECTOR;
}

/* host pointer for a PS1 RAM address inside one of the mapped windows */
static void *ps1_window_ptr(u32 addr, u32 len) {
    if ((addr >= 0x80010000u && addr + len <= 0x80041000u) ||
        (addr >= 0x800C0000u && addr + len <= 0x800E0000u) ||
        (addr >= 0x80132000u && addr + len <= 0x80232000u))
        return (void *)(uintptr_t)addr;
    return NULL;
}

/* pc/hal/extras_state.c: which disc file a read put where (the gallery
 * names the pictures by it); weak: the CD harnesses link this file alone */
extern void extras_note_cd_load(const char *path, unsigned ps1_dest, unsigned bytes)
    __attribute__((weak));

static void deliver_sector(u32 lba, u32 ps1_dest) {
    void *dst = ps1_window_ptr(ps1_dest, SECTOR_SIZE);
    static int warned;
    if (!dst) {
        if (!warned++) fprintf(stderr, "[HAL/CD] read destination 0x%08X is outside the mapped PS1 RAM windows; data dropped\n", ps1_dest);
        return;
    }
    if (!open_sector_source(lba) || lba < g_sector_file_lba || lba >= g_sector_file_lba + g_sector_file_nsec) {
        static int warned2;
        if (!warned2++) fprintf(stderr, "[HAL/CD] no disc file covers LBA %u; sector zeroed\n", lba);
        memset(dst, 0, SECTOR_SIZE);
        return;
    }
    memset(dst, 0, SECTOR_SIZE);
    fseek(g_sector_file, (long)(lba - g_sector_file_lba) * SECTOR_SIZE, SEEK_SET);
    if (fread(dst, 1, SECTOR_SIZE, g_sector_file) > 0 && lba == g_xfer_first_lba) {
        size_t pl = strlen(g_sector_path);
        nbn_trace_register_load(g_sector_path, dst, 0, g_xfer_total * SECTOR_SIZE);
        if (extras_note_cd_load) extras_note_cd_load(g_sector_path, ps1_dest, g_xfer_total * SECTOR_SIZE);
        /* an EXN read into the overlay window: that overlay's code is the
         * one PS1 code addresses there now name (pc/hal/overlay.c) */
        if (ps1_dest == 0x80132000u && pl > 4 && !strcmp(g_sector_path + pl - 4, ".EXN")) {
            extern void hal_overlay_set_loaded(const char *exn_path);
            hal_overlay_set_loaded(g_sector_path);
        }
    }
}

/* ------------------------------------------------------------------ events */
static void complete(int status) {
    g_sync_status = status;
    g_result[0] = drive_stat();
}

/* The status byte between a read's seek landing and its first sector.
 * On the reference the status stays SEEKING after the landing until the
 * first sector is read (READING then replaces it), so for one sector
 * period (225,792 ticks at 2x) a Getstat still answers SEEKING.  The port
 * reported READING from the landing.  Measured (the reference's drive
 * trace, date_save phase 1, 2026-10-31 timing round 6): Shiori's
 * phone greeting (FUJISAKI.XA, ReadS seek 39671 -> 59004) lands at
 * request +14,383,755, the CD queue's Getstat at +14,506,733 answers
 * without 0x20 (cdq_command_process case 27 keeps polling), the first
 * sector is read at +14,609,570, and the next frame's Getstat sets queue
 * status 4: xa_m_mode 1 (the line's text starts) one VBlank later than the
 * port's.  CD_SEEK_STAT_UNTIL_SECTOR=0 is the negative control. */
static int seek_stat_until_sector(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("CD_SEEK_STAT_UNTIL_SECTOR"); on = !(e && e[0] == '0'); }
    return on;
}

/* Start a logical seek (SeekL, or a ReadN/ReadS/Play with
 * a Setloc pending, which seeks first and reads when it lands) */
static void begin_seek(u32 lba, int read_after, int play_after) {
    u64 t = seek_ticks(lba, 0);
    XA_CALL(cd_audio_reset_decoder, (g_now));
    g_seek_target = lba;
    g_read_after_seek = read_after;
    g_play_after_seek = play_after;
    g_drive = DRIVE_SEEKING;
    g_drive_at = g_now + t;
    g_xfer_active = g_xfer_active;      /* a pending CdRead keeps its count */
    CDLOG("seeking to %u, %.2f frames%s", lba, (double)t / TICKS_PER_FRAME, read_after ? " then read" : "");
}

/* Start reading without a pending Setloc: read from the logical
 * position; the head first moves from where it physically is (two sectors
 * short after a logical seek, somewhere on the held track after a pause). */
static void begin_reading(int play) {
    u64 t;
    if (g_setloc_pending) {
        g_setloc_pending = 0;
        if (g_drive == DRIVE_READING && g_setloc_lba == g_cur_lba) return;   /* already reading there */
        begin_seek(g_setloc_lba, !play, play);
        return;
    }
    if (g_drive == DRIVE_SEEKING) {     /* read when the seek in progress lands */
        g_read_after_seek = !play;
        g_play_after_seek = play;
        return;
    }
    if (g_drive == DRIVE_READING || g_drive == DRIVE_PLAYING) return;
    t = (play ? ticks_per_sector() : read_sector_ticks(g_cur_lba, -1)) + seek_ticks(g_cur_lba, 0);
    XA_CALL(cd_audio_reset_decoder, (g_now));
    g_drive = play ? DRIVE_PLAYING : DRIVE_READING;
    g_implicit_seek = 1;
    g_drive_at = g_now + t;
    g_drive_last = g_now;
    CDLOG("%s from %u: first sector in %.2f frames", play ? "play" : "read", g_cur_lba, (double)t / TICKS_PER_FRAME);
}

static void drive_event(void) {
    switch (g_drive) {
    case DRIVE_SPEEDCHANGE:
        g_drive = DRIVE_IDLE;
        break;
    case DRIVE_SEEKING:
        /* seek done: a logical seek lands SUBQ_SECTOR_SKEW sectors short */
        g_cur_lba = g_seek_target;
        g_reset_in_pregap = 0;
        g_subq = g_seek_target >= SUBQ_SECTOR_SKEW ? g_seek_target - SUBQ_SECTOR_SKEW : 0;
        g_subq_tick = g_now;
        g_subq_carry = 0;
        g_header_valid = 1;
        g_drive = DRIVE_IDLE;
        CDLOG("seek complete at %u", g_cur_lba);
        if (g_second_cmd == CMD_SEEKL || g_second_cmd == CMD_SEEKP) {
            g_second_cmd = 0;
            complete(2);
        }
        if (g_read_after_seek || g_play_after_seek) {
            /* a read or play queued behind the seek starts now: the first
             * sector arrives one sector period later */
            XA_CALL(cd_audio_reset_decoder, (g_now));
            g_drive = g_play_after_seek ? DRIVE_PLAYING : DRIVE_READING;
            g_implicit_seek = seek_stat_until_sector();
            g_drive_at = g_now + (g_drive == DRIVE_READING ? read_sector_ticks(g_cur_lba, -1)
                                                           : ticks_per_sector());
            g_drive_last = g_now;
        }
        g_read_after_seek = g_play_after_seek = 0;
        break;
    case DRIVE_READING:
    case DRIVE_PLAYING: {
        static u8 raw[CD_AUDIO_RAW_SECTOR];
        int is_raw;
        if (g_drive == DRIVE_READING && fade_hold_sector()) {
            g_drive_at = g_now + ticks_per_sector();    /* look again one sector on */
            break;
        }
        is_raw = g_drive == DRIVE_READING && read_raw_sector(g_cur_lba, raw);
        u32 msf = g_cur_lba + LBA_PREGAP;
        g_drive_last = g_now;
        g_implicit_seek = 0;
        g_subq = g_cur_lba;
        g_subq_tick = g_now;
        g_header_valid = 1;
        /* the sector header GetlocL reports: absolute MSF (BCD), mode 2, subheader */
        g_last_header[0] = (u8)(((msf / 4500) / 10) << 4 | ((msf / 4500) % 10));
        g_last_header[1] = (u8)((((msf % 4500) / 75) / 10) << 4 | (((msf % 4500) / 75) % 10));
        g_last_header[2] = (u8)(((msf % 75) / 10) << 4 | ((msf % 75) % 10));
        g_last_header[3] = 2;
        if (is_raw) memcpy(g_last_header + 4, raw, 4);
        else { g_last_header[4] = g_last_header[5] = g_last_header[7] = 0; g_last_header[6] = 0x08; }
        /* XA-ADPCM enabled (Setmode bit 6): a realtime audio sector goes to
         * the controller's decoder and the SPU, not to the CPU -- no data,
         * no interrupt */
        if (is_raw && (g_mode & 0x40) && (raw[2] & 0x44) == 0x44) {
            XA_CALL(backlog_xa_sector, (g_cur_lba, raw, (g_mode & 0x08) != 0));
            XA_CALL(cd_audio_xa_sector, (raw, (g_mode & 0x08) != 0, g_now));
            g_cur_lba++;
            g_drive_at = g_now + ticks_per_sector();
            break;
        }
        if (g_drive == DRIVE_READING)
            g_sector_irq = g_xfer_active && g_xfer_remaining > 0 ? 1
                         : g_stream_active && (!libpress_stream_sector_irq ||
                                               libpress_stream_sector_irq(g_cur_lba, g_mode)) ? 2 : 0;
        if (g_drive == DRIVE_READING && g_xfer_active && g_xfer_remaining > 0) {
            deliver_sector(g_cur_lba, g_xfer_ps1_dest);
            g_xfer_ps1_dest += SECTOR_SIZE;
            g_xfer_remaining--;
            if (g_xfer_remaining == 0) {
                g_xfer_active = 0;
                g_autopause_due = 1;    /* libcd cb_read pauses the drive itself */
                CDLOG("read of %u sectors complete (last LBA %u); libcd pauses", g_xfer_total, g_cur_lba);
            }
        }
        g_cur_lba++;
        g_drive_at = g_now + (g_drive == DRIVE_READING ? read_sector_ticks(g_cur_lba, is_raw)
                                                       : ticks_per_sector());
        break;
    }
    default:
        break;
    }
}

/* the command's effect, at acknowledge time */
static void execute(int cmd, const u8 *param) {
    g_result[0] = drive_stat();
    switch (cmd) {
    case CMD_SETLOC:
        g_setloc_lba = (u32)pos_to_int((void *)param);
        g_setloc_pending = 1;
        complete(2);
        break;
    case CMD_SETMODE: {
        u8 mode = param ? param[0] : 0;
        int was_double = g_double_speed;
        g_mode = mode;
        g_double_speed = (mode & 0x80) != 0;
        complete(2);
        if (was_double != g_double_speed) {
            u64 change = speed_change_ticks();
            if (g_drive == DRIVE_SPEEDCHANGE) {
                if (g_drive_at > g_now && g_drive_at - g_now >= change / 4) g_drive = DRIVE_IDLE;
            } else if (g_drive != DRIVE_IDLE) {
                g_drive_at += change;
            } else {
                g_drive = DRIVE_SPEEDCHANGE;
                g_drive_at = g_now + change;
            }
            CDLOG("setmode 0x%02X: speed change to %dx, %llu ticks", mode, g_double_speed ? 2 : 1, (unsigned long long)change);
        }
        break;
    }
    case CMD_READN:
    case CMD_READS:
        if (cmd == CMD_READN) g_stream_active = 0;
        begin_reading(0);
        complete(2);
        break;
    case CMD_PLAY:
        begin_reading(1);
        complete(2);
        break;
    case CMD_PAUSE: {
        u64 t = pause_ticks();
        CDLOG("pause (%s): completes in %.2f frames",
              g_drive == DRIVE_READING ? "reading" : g_drive == DRIVE_PLAYING ? "playing" : "idle",
              (double)t / TICKS_PER_FRAME);
        if (g_drive == DRIVE_READING && g_header_valid) {
            /* SetHoldPosition: the head jumps one track back from the last
             * sector read and keeps re-reading it (subq_now() cycles it) */
            u32 spt = sectors_per_track(g_subq);
            g_cur_lba = g_subq;
            g_subq = g_cur_lba >= spt ? g_cur_lba - spt : 0;
            g_subq_tick = g_now;
            g_subq_carry = 0;
        }
        g_second_cmd = CMD_PAUSE;
        g_second_at = g_now + t;
        g_sync_status = 0;
        g_stream_active = 0;
        XA_CALL(cd_audio_reset_decoder, (g_now));
        /* the drive stops delivering sectors now */
        g_xfer_active = 0;
        g_implicit_seek = 0;
        g_drive = DRIVE_IDLE;
        break;
    }
    case CMD_INIT: {
        /* SoftReset: single speed, drive idle, then the reset time */
        u64 change = g_double_speed ? (u64)(0.7 * MASTER_CLOCK) : 0;
        u64 seek;
        g_drive = DRIVE_IDLE;
        g_double_speed = 0;
        g_mode = 0x20;
        XA_CALL(cd_audio_reset_decoder, (g_now));
        /* SoftReset changes mode before calculating the seek at 1x. */
        seek = g_cur_lba != 0 ? seek_ticks(-(s32)LBA_PREGAP, 1) : 0;
        g_read_after_seek = 0;
        g_play_after_seek = 0;
        g_setloc_pending = 0;
        g_xfer_active = 0;
        g_implicit_seek = 0;
        g_stream_active = 0;
        {
            u64 total = change + seek;
            if (!drive_timing()) total = INSTANT_TICKS;
            else if (total < INIT_TICKS) total = INIT_TICKS;
            g_second_cmd = CMD_INIT;
            g_second_at = g_now + total;
            g_sync_status = 0;
            CDLOG("init: completes in %llu ticks (%.2f frames)", (unsigned long long)total, (double)total / TICKS_PER_FRAME);
        }
        break;
    }
    case CMD_SEEKL:
    case CMD_SEEKP:
        g_setloc_pending = 0;
        g_xfer_active = 0;
        g_stream_active = 0;
        begin_seek(g_setloc_lba, 0, 0);
        g_second_cmd = cmd;
        g_second_at = g_drive_at;
        g_sync_status = 0;
        break;
    case CMD_STOP: {
        u64 t = !drive_timing() ? INSTANT_TICKS
              : g_motor_on ? (g_double_speed ? 25000000u : 13000000u) : 7000u;
        g_drive = DRIVE_IDLE;
        g_xfer_active = 0;
        g_stream_active = 0;
        g_second_cmd = CMD_STOP;
        g_second_at = g_now + t;
        g_sync_status = 0;
        break;
    }
    case CMD_MOTORON:
        g_motor_on = 1;
        complete(2);
        break;
    case CMD_SETFILTER:
        XA_CALL(cd_audio_set_filter, (param ? param[0] : 0, param ? param[1] : 0));
        complete(2);
        break;
    case CMD_MUTE:
    case CMD_DEMUTE:
        XA_CALL(cd_audio_set_mute, (cmd == CMD_MUTE));
        complete(2);
        break;
    case CMD_GETLOCL:
        /* the header of the last sector read (psx-spx GetlocL; no
         * status byte) -- check_head_position follows an XA line with it */
        complete(2);
        memcpy(g_result, g_last_header, 8);
        break;
    default:            /* GetStat, Mute, Demute, Setfilter, Getparam, GetlocL/P, Test, ... */
        complete(2);
        break;
    }
}

static void second_response(void) {
    int cmd = g_second_cmd;
    g_second_cmd = 0;
    switch (cmd) {
    case CMD_INIT:
        g_reset_in_pregap = 1;
        g_cur_lba = 0;
        g_subq = 0;
        g_subq_tick = g_now;
        g_subq_carry = 0;
        g_header_valid = 0;
        g_motor_on = 1;
        break;
    case CMD_STOP:
        g_motor_on = 0;
        break;
    default:
        break;
    }
    complete(2);
    CDLOG("command %d complete", cmd);
}

static u8 g_cmd_param[16];

/* An event handler may spend CPU time or wait on another event. Keep that
 * elapsed handler time outside the interrupted CPU caller's work budget.
 * Suspend the interrupted frame during dispatch so nested handler waits
 * cannot charge it twice. Device deadlines stay absolute.
 */
typedef struct CpuWorkFrame {
    struct CpuWorkFrame *previous;
    u64 paused;
} CpuWorkFrame;
static CpuWorkFrame *g_cpu_work;


static void advance_to(u64 t);
static void autopause_in_handler(void);

/* ---- interrupt exceptions other than VBlank ----
 * Native handlers run in no emulated time, but on the console the CPU
 * spends each device interrupt inside an exception: the BIOS walks its
 * handler chain, libetc's trapIntr dispatches the callback, then
 * ReturnFromException.  Measured on the reference emulator (Interpreter,
 * SCPH-5500; -tracepc on the exception vector 0x80000080, the kernel's
 * ReturnFromException jump at 0x1010, trapIntr and the callbacks; a
 * 1,800-frame no-input boot), entry to return, medians:
 *   CD INT3 acknowledge (getintr only)            3,776   (n=65)
 *   CD INT2 completion  (getintr only)            4,616   (n=23)
 *   CD INT1 sector, CdRead (cb_read, data DMA)    4,559   (n=434)
 *   CD INT1 sector, libstr stream (StCdInterrupt) 5,849   (n=718)
 *     ... while MDEC-out DMA is busy (StCdInterrupt
 *     returns at once; strCallback takes the sector) 3,910   (n=441)
 *   (the stream's XA audio sectors, one in eight, raise no interrupt)
 *   SPU DMA completion (_spu_interruptDMA)       19,114   (n=37; its
 *       DeliverEvent(HwSPU) at 17,619)
 * The time comes out of whatever the interrupt stopped (the dispatch in
 * run_until charges it to the interrupted CPU work); an absolute wait just
 * resumes later.
 *
 * The kernel's VBlank verifier (kernel 0x18BC, its I_STAT read at 0x18E0)
 * runs 409 ticks after entry in every exception (median; 384..426 for the
 * classes above).  A VBlank raised after that read, while the exception
 * is still in trapIntr's loop, is acknowledged by trapIntr and dispatched
 * to trapIntrVSync inside the same exception: the kernel never delivers
 * its VBlank event (vblank_counter_tick -> vblnk_counter) for that edge.
 * trapIntr's last I_STAT read is 389 ticks before the return (median of
 * 6,173): a VBlank raised after it is left pending and taken by a new
 * exception right after the return, which runs both levels (an edge 5,536
 * into a 5,842-tick stream-sector exception was counted).
 * Retail does this 11 times in the first 585 game frames (stream sectors
 * in the boot movie, the SPU DMA at frame 458, a CdRead sector at 543);
 * an edge 8 ticks before the read is counted, 780 after it is not.  The
 * absorbed VBlank lengthens the exception by libetc's VBlank work with the
 * music running: 2,745 and 2,892 on the two title-phase cases (2,603 in
 * the movie, without the sequencer).  VSync(0) waiting on that edge
 * resumes 129 ticks after the exception returns, as it does after the
 * VBlank's own exception (24,291 - 24,162).
 *
 * An interrupt raised while the CPU is already inside an exception (or in a
 * native handler) is dispatched in the same loop and is not given a second
 * exception.  One raised while it is masked or in a critical section is
 * not deferred: its effect is immediate and it costs no time (unmodelled).
 * A VBlank raised inside an exception BEFORE the verifier's read runs both
 * levels there and lengthens the exception by VBLANK_IN_EXCEPTION_TICKS
 * (exceptions entered with VBlank and another source both pending:
 * 22,380 / 22,516 / 22,834 over the CdRead-sector, MDEC and stream-sector
 * exception alone).  Memory card interrupts are not measured and take no
 * time. */
#define IRQ_BIT_CD  0x0004u
#define IRQ_BIT_DMA 0x0008u
#define KERNEL_VBLANK_CHECK_TICKS   409u
#define CD_ACK_EXCEPTION_TICKS      3776u
#define CD_ACK_RESPONSE8_EXTRA_TICKS 317u  /* GetlocL/P: see completes_at_ack */
static int getloc_at_ack(void);
static int cd_path_costs(void);
static u32 ack_exception_ticks(int c);
static u32 complete_exception_ticks(int c);
#define CD_COMPLETE_EXCEPTION_TICKS 4616u
#define CD_READ_SECTOR_EXCEPTION_TICKS   4559u
#define CD_STREAM_SECTOR_EXCEPTION_TICKS 5849u
#define CD_STREAM_SECTOR_MDEC_BUSY_TICKS 3910u
#define LAST_MASK_READ_TO_RETURN_TICKS 389u
#define ABSORBED_VBLANK_TICKS       2800u
#define VBLANK_IN_EXCEPTION_TICKS   22516u
#define VSYNC_AFTER_RETURN_TICKS    129u
typedef struct IrqException {
    u64 entry;
    u64 end;            /* return without the VBlank work, (u64)-1 until known */
    u64 extra;          /* VBlank work done inside it */
    int absorbed;
} IrqException;
static IrqException *g_exc;
static u64 g_absorbed_edge = (u64)-1;    /* the last VBlank an exception absorbed */
static u64 g_absorbed_resume = (u64)-1;  /* when VSync(0) waiting on it resumes */
static int g_ack_exception;              /* the last acknowledge took its exception */
/* Harnesses that link cd.c without the interrupt controller take every
 * exception and never absorb a VBlank. */
extern int hal_irq_accepts(u16 bits) __attribute__((weak));
extern int hal_vblank_absorbed(void) __attribute__((weak));

static int exc_enter(IrqException *e, u16 bit) {
    if (g_exc || (hal_irq_accepts && !hal_irq_accepts(bit))) return 0;
    e->entry = g_now;
    e->end = (u64)-1;
    e->extra = 0;
    e->absorbed = 0;
    g_exc = e;
    return 1;
}

/* Run to the exception's return (`end`, plus the VBlank work done inside). */
static void exc_leave(IrqException *e, u64 end) {
    while (g_now < end + e->extra) advance_to(end + e->extra);
    CDLOG("exception %llu..%llu (%llu ticks)%s", (unsigned long long)e->entry,
          (unsigned long long)g_now, (unsigned long long)(g_now - e->entry),
          e->absorbed ? ", absorbed a VBlank" : "");
    g_exc = NULL;
    if (e->absorbed) g_absorbed_resume = g_now + VSYNC_AFTER_RETURN_TICKS;
}

/* One device interrupt's exception, from inside an event dispatch: the
 * device's effect has been applied (or `effect` runs `effect_at` after
 * entry), and the CPU returns `exit_at` after entry. */
/* Recovery abandons all active CPU calls (and any exception in progress)
 * before longjmp unwinds them. */
/* Device event dispatch in progress: game callbacks run from here are
 * exception time, not CPU work (pc/hal/cpu_cost.c). */
static int g_dispatching;
int hal_cd_dispatching(void) { return g_dispatching > 0; }

void hal_cpu_abort_work(void) {
    extern void hal_cpu_cost_abort(void) __attribute__((weak));
    g_cpu_work = NULL; g_control_call = 0; g_exc = NULL; g_dispatching = 0;
    if (hal_cpu_cost_abort) hal_cpu_cost_abort();
}

int hal_irq_exception(u16 bit, u32 effect_at, void (*effect)(void), u32 exit_at) {
    IrqException e;
    if (!exc_enter(&e, bit)) {
        if (effect) effect();
        return 0;
    }
    e.end = e.entry + exit_at;
    if (effect) {
        advance_to(e.entry + effect_at + e.extra);
        effect();
    }
    exc_leave(&e, e.end);
    return 1;
}

/* run every event that falls at or before `t`, in order, and move the clock there */
static void run_until(u64 t) {
    for (;;) {
        u64 next = (u64)-1;
        int which = 0;
        CpuWorkFrame *interrupted;
        u64 dispatch_start;
        if (g_cmd_pending && g_ack_at < next) { next = g_ack_at; which = 1; }
        if (g_second_cmd && g_second_at < next) { next = g_second_at; which = 2; }
        if (g_drive != DRIVE_IDLE && g_drive_at < next) { next = g_drive_at; which = 3; }
        if (memcard_next_event() < next) {
            next = memcard_next_event(); which = 4;
        }
        if (spu_transfer_next_event() < next) {
            next = spu_transfer_next_event(); which = 5;
        }
        if (!which || next > t) break;
        if (next > g_now) g_now = next;
        interrupted = g_cpu_work;
        dispatch_start = g_now;
        g_cpu_work = NULL;
        g_dispatching++;
        if (which == 1) {
            int c = g_cmd_pending;
            g_cmd_pending = 0;
            execute(c, g_cmd_param);
            g_ack_exception = hal_irq_exception(IRQ_BIT_CD, 0, NULL, ack_exception_ticks(c) +
                ((c == CMD_GETLOCL || c == CMD_GETLOCP) && getloc_at_ack()
                 ? CD_ACK_RESPONSE8_EXTRA_TICKS : 0u));
        }
        else if (which == 2) {
            int c2 = g_second_cmd;
            second_response();
            hal_irq_exception(IRQ_BIT_CD, 0, NULL,
                              complete_exception_ticks(c2));
        }
        else if (which == 3) {
            g_sector_irq = 0;
            drive_event();
            if (g_autopause_due && !g_cmd_pending) {
                /* the last sector's exception: cb_read pauses the drive */
                IrqException e;
                int taken = exc_enter(&e, IRQ_BIT_CD);
                autopause_in_handler();
                if (taken) exc_leave(&e, g_now);
            } else if (g_sector_irq) {
                hal_irq_exception(IRQ_BIT_CD, 0, NULL, g_sector_irq == 1
                                  ? CD_READ_SECTOR_EXCEPTION_TICKS
                                  : libpress_mdec_out_busy && libpress_mdec_out_busy(g_now)
                                  ? CD_STREAM_SECTOR_MDEC_BUSY_TICKS
                                  : CD_STREAM_SECTOR_EXCEPTION_TICKS);
            }
        }
        else if (which == 4) memcard_complete_event();
        else spu_transfer_complete_event();
        g_dispatching--;
        g_cpu_work = interrupted;
        if (interrupted) interrupted->paused += g_now - dispatch_start;
        if (g_autopause_due && !g_cmd_pending) {
            /* The last sector arrived while another command was in flight:
             * libcd's read callback issues CdlPause once it can; CdSync(1)
             * reports busy until it completes, and the game's own commands
             * queue behind it (CD_cw waits for it).  The usual case, an
             * idle drive, is autopause_in_handler. */
            g_autopause_due = 0;
            g_cmd_pending = CMD_PAUSE;
            g_ack_at = g_now + ACK_TICKS;
            g_sync_status = 0;
            memset(g_cmd_param, 0, sizeof g_cmd_param);
        }
    }
    if (t > g_now) g_now = t;
}

/* ---- the system clock: VBlanks fall every TICKS_PER_FRAME ----
 * advance_to() moves time forward through every VBlank it crosses, running
 * the enabled kernel VBlank events (vblank_counter_tick -> vblnk_counter) at
 * each one, exactly as the console's VBlank interrupt keeps counting while
 * the game spins on the drive. */
extern void hal_vblank_interrupt(void);   /* pc/hal/system.c: kernel VBlank events, pad poll, trace */
static u64 g_next_vblank = TICKS_PER_FRAME;
/* The VBlank period.  NTSC: 263 lines of 3,413 dot-clock ticks, the dot
 * clock 715,909/451,584 of the CPU's -- 566,203.775 CPU ticks a frame
 * (the reference trace: 566,203.776 a frame over 1,000 frames).  The port stepped a whole 566,204, so its
 * edges drifted 0.225 ticks a frame against the drive's sector clock --
 * ~140 ticks by the end of the boot movie, where a stream sector's
 * exception and an edge decide the lost-VBlank rule within ~100 ticks.
 * Edge n now falls at first + floor(n * 897,619 * 451,584 / 715,909).
 * VBLANK_EXACT_PERIOD=0 is the negative control (whole ticks). */
#define VBLANK_PERIOD_NUM (897619ull * 451584ull)
#define VBLANK_PERIOD_DEN 715909ull
static u64 g_vbl_first = TICKS_PER_FRAME;   /* edge 0 */
static u64 g_vbl_index;                     /* g_next_vblank is edge g_vbl_index */
static int g_vbl_exact = -1;
static int vbl_exact(void) {
    if (g_vbl_exact < 0) {
        const char *e = getenv("VBLANK_EXACT_PERIOD");
        g_vbl_exact = !(e && e[0] == '0');
    }
    return g_vbl_exact;
}
/* The VBlank interrupt is raised when the beam reaches the vertical
 * display range's end, GP1(07h) Y2 (psx-spx GP1(07h) "Vertical Display
 * range"; measured on the reference: the frame is 263 lines whatever Y2
 * is).  The phase above was measured with the GPU reset's
 * Y2 = 255 (GP1(00h): vertical range 0x3FC10); libgpu's PutDispEnv writes
 * 07040010 (Y2 = 256) and ResetGraph(0)'s GP1(00h) puts 255 back, so each
 * change moves the following edges by whole lines of 3,413 dot clocks
 * (2,152.87 CPU ticks).  Retail's boot shows it: the frame whose
 * PutDispEnv first writes 256 is 2,162 ticks long beyond the period (host
 * frame 555), the ResetGraph(0) after it takes the line back (556,
 * -2,154), and the movie's PutDispEnv sets 256 for good (561, +2,132).
 * A Y2 lowered behind the beam skips that frame's edge (the line never
 * reaches it again before the wrap).  VBLANK_DISPLAY_RANGE=0 is the
 * negative control (edges at Y2 = 255 throughout). */
#define VBLANK_Y2_RESET 255
#define VBL_SHIFTS 64
static struct { u64 from; s64 ticks; } g_vbl_shift[VBL_SHIFTS];
static int g_vbl_shifts;
static int g_vbl_y2 = VBLANK_Y2_RESET;
static u64 g_vbl_prev_edge;
static int g_vbl_range = -1;
static s64 vbl_shift_for(u64 n) {
    int i;
    for (i = g_vbl_shifts - 1; i >= 0; i--)
        if (g_vbl_shift[i].from <= n) return g_vbl_shift[i].ticks;
    return 0;
}
static u64 vbl_edge(u64 n) {
    return (u64)((s64)(g_vbl_first + (vbl_exact() ? n * VBLANK_PERIOD_NUM / VBLANK_PERIOD_DEN
                                                    : n * TICKS_PER_FRAME)) + vbl_shift_for(n));
}
static void vbl_step(void) { g_vbl_prev_edge = g_next_vblank; g_next_vblank = vbl_edge(++g_vbl_index); }
static u64 vbl_prev(void) { return g_vbl_index ? g_vbl_prev_edge : 0; }

void hal_vblank_display_end(int y2) {
    s64 lines, ticks;
    if (g_vbl_range < 0) {
        const char *e = getenv("VBLANK_DISPLAY_RANGE");
        g_vbl_range = !(e && e[0] == '0');
    }
    if (!g_vbl_range || y2 == g_vbl_y2) return;
    g_vbl_y2 = y2;
    lines = (s64)y2 - VBLANK_Y2_RESET;
    ticks = vbl_exact() ? lines * (s64)VBLANK_PERIOD_NUM / (s64)(VBLANK_PERIOD_DEN * 263u)
                        : lines * (s64)TICKS_PER_FRAME / 263;
    if (g_vbl_shifts == VBL_SHIFTS) {
        memmove(g_vbl_shift, g_vbl_shift + 1, sizeof g_vbl_shift[0] * (VBL_SHIFTS - 1));
        g_vbl_shifts--;
    }
    if (g_vbl_shifts && g_vbl_shift[g_vbl_shifts - 1].from == g_vbl_index) g_vbl_shifts--;
    g_vbl_shift[g_vbl_shifts].from = g_vbl_index;
    g_vbl_shift[g_vbl_shifts].ticks = ticks;
    g_vbl_shifts++;
    g_next_vblank = vbl_edge(g_vbl_index);
    if (g_next_vblank <= g_now) {
        /* behind the beam: this frame raises no VBlank */
        CDLOG("display end %d behind the beam: edge %llu skipped", y2,
              (unsigned long long)g_vbl_index);
        g_vbl_index++;
        g_next_vblank = vbl_edge(g_vbl_index);
    }
    CDLOG("display end Y2=%d: next VBlank at %llu", y2, (unsigned long long)g_next_vblank);
}
/* CPU time the VBlank exception itself takes from interrupted CPU work.
 * The native handlers run in no emulated time, but on retail the CPU is in
 * the exception for ~24,400 ticks of every frame (BIOS VBlank events and
 * pad poll, then libetc's chain; pc/hal/system.c reports it when that chain
 * is entered).  Only an exception taken during this VBlank dispatch counts:
 * a latched delivery on a later SetIntrMask/ExitCriticalSection is outside
 * any timed CPU work.  Waits (VSync, the drive) are absolute and unaffected.
 * It is one exception per edge: an overrun edge re-delivered to libetc
 * inside the same exception is not charged a second time (its extra cost
 * is unmeasured). */
static int g_in_vblank_dispatch;
static u64 g_irq_occupancy;
static u64 g_last_vblank_occupancy;   /* the last edge's exception (0: none) */
void hal_cpu_interrupt_occupancy(u32 ticks) {
    if (g_in_vblank_dispatch && ticks > g_irq_occupancy) g_irq_occupancy = ticks;
}

static void advance_to(u64 t) {
    while (g_next_vblank <= t) {
        u64 edge = g_next_vblank;
        CpuWorkFrame *interrupted;
        u64 dispatch_start;
        int outer_dispatch;
        u64 outer_occupancy;
        run_until(edge);
        /* A completion callback may itself advance the clock. */
        if (g_next_vblank != edge) continue;
        vbl_step();
        g_frames++;
        if (g_exc && edge >= g_exc->entry + KERNEL_VBLANK_CHECK_TICKS &&
                (g_exc->end == (u64)-1 ||
                 edge + LAST_MASK_READ_TO_RETURN_TICKS < g_exc->end + g_exc->extra) &&
                hal_vblank_absorbed && hal_vblank_absorbed()) {
            /* raised after the kernel's verifier: trapIntr takes it */
            g_exc->extra += ABSORBED_VBLANK_TICKS;
            g_exc->absorbed = 1;
            g_absorbed_edge = edge;
            g_absorbed_resume = (u64)-1;
            CDLOG("VBlank absorbed by the exception entered %llu ticks earlier",
                  (unsigned long long)(edge - g_exc->entry));
            continue;
        }
        interrupted = g_cpu_work;
        dispatch_start = g_now;
        g_cpu_work = NULL;
        /* A handler's own CPU work can cross further edges (nested here). */
        outer_dispatch = g_in_vblank_dispatch;
        outer_occupancy = g_irq_occupancy;
        g_in_vblank_dispatch = 1;
        g_irq_occupancy = 0;
        hal_vblank_interrupt();
        g_last_vblank_occupancy = g_irq_occupancy;
        g_cpu_work = interrupted;
        if (interrupted) interrupted->paused += g_now - dispatch_start + g_irq_occupancy;
        /* pending at the verifier: both levels run inside this exception */
        if (g_exc && g_irq_occupancy) g_exc->extra += VBLANK_IN_EXCEPTION_TICKS;
        g_in_vblank_dispatch = outer_dispatch;
        g_irq_occupancy = outer_occupancy;
    }
    run_until(t);
}

/* libcd's read callback pauses the drive from inside the CD interrupt
 * handler: when the last requested sector's interrupt is taken, it calls
 * CD_cw(CdlPause) synchronously, and CD_cw busy-waits for the acknowledge
 * there (the handler polls libcd's callback ~95 times, interrupts still
 * disabled), then the exception dispatches the acknowledge and returns.
 * Measured on the reference emulator (Interpreter; three boot reads, frames
 * 1356/1372/1406 of a 1,500-frame no-input boot), from the last sector's
 * trapIntr: CD_cw entered after 1,823..1,999 (1,880), CD_cw returns 26,475
 * ..26,490 later, the acknowledge's callback runs 1,348..1,352 after that.
 * The interrupt reaches trapIntr ~2,300 after it is raised (the acknowledge
 * of a timed command reaches it 27,233..27,518 after CD_cw's write, against
 * the drive's 25,000), and CD_cw's write comes ~665 after its entry here
 * (its CD_sync takes 532 inside the handler against 310 outside).  So the
 * Pause is written 4,845 ticks after the sector, and the CPU leaves the
 * handler 2,661 ticks after the acknowledge (811 to CD_cw's return, 1,350
 * to the re-dispatch, ~500 for that callback and the exception's return,
 * the last figure estimated, not probed).  The handler time is CPU time
 * taken from whatever the interrupt stopped: the nested clock advance runs
 * inside the drive event's dispatch, which charges it to that work. */
#define AUTOPAUSE_TO_WRITE_TICKS  4845u
#define AUTOPAUSE_ACK_TO_EXIT     2661u
static void autopause_in_handler(void) {
    g_autopause_due = 0;
    advance_to(g_now + AUTOPAUSE_TO_WRITE_TICKS);
    if (g_cmd_pending) {   /* a command issued meanwhile: defer as before */
        g_autopause_due = 1;
        return;
    }
    g_cmd_pending = CMD_PAUSE;
    g_last_cmd = CMD_PAUSE;
    g_ack_at = g_now + ACK_TICKS;
    g_sync_status = 0;
    memset(g_cmd_param, 0, sizeof g_cmd_param);
    CDLOG("read callback issues pause; handler waits for the acknowledge");
    if (g_exc) g_exc->end = g_ack_at + AUTOPAUSE_ACK_TO_EXIT;
    advance_to(g_ack_at);
    advance_to(g_now + AUTOPAUSE_ACK_TO_EXIT);
}

/* The system clock, for VSync / GsGetVcount (pc/hal/system.c): ticks since
 * power-on and VBlanks seen. */
u64 hal_clock_ticks(void) { return g_now; }
u32 hal_clock_vblanks(void) { return (u32)g_frames; }
u32 hal_clock_ticks_per_frame(void) { return TICKS_PER_FRAME; }
u32 hal_clock_ticks_per_second(void) { return MASTER_CLOCK; }

/* CPU time the console would spend in a long loop the PC runs at once (the
 * BIOS ROM byte loops, src/main/pc-port/bios_trampolines.c): the clock moves
 * on, and the VBlank interrupts that fall meanwhile are raised. */
void hal_cpu_spend(u32 ticks) {
    CpuWorkFrame work;
    u64 started, target;
    work.previous = g_cpu_work;
    work.paused = 0;
    g_cpu_work = &work;
    started = g_now;
    do {
        target = started + ticks + work.paused;
        advance_to(target);
    } while (g_now < started + ticks + work.paused);
    g_cpu_work = work.previous;
}
u32 hal_cpu_ticks_to_vblank(void) { return (u32)(g_next_vblank > g_now ? g_next_vblank - g_now : 0); }

/* libcd's CPU side of one command (CD_cw), measured on the reference emulator
 * (Interpreter, SCPH-5500) over the 2,200-frame no-input boot: 45 CD_cw
 * calls from CD_init, CdControl, CdControlB, CdRead and the queue worker.
 *   entry -> command-register write          430-540 ticks (median 443)
 *   write -> getintr (the acknowledge IRQ)   ack + 2,150..2,610 (exception
 *            entry and the BIOS/libetc dispatch to libcd's handler)
 *   getintr -> CD_cw return                  1,150..2,430 (median 1,400)
 * The drive acknowledges ACK_TICKS after the write; a caller blocked on it
 * resumes after the interrupt has been taken and CD_cw has returned.  The
 * wrappers' own work is charged by CdControl/CdControlB (CDCONTROL*_TICKS),
 * and their completing commands' longer return by
 * CD_CW_COMPLETE_EXTRA_TICKS.  Not cycle-exact: the per-path spread is
 * about +/-1,000 ticks. */
#define CD_CW_WRITE_TICKS   440u
/* Path-specific libcd times (the reference emulator, the boot movie's start:
 * strKickCD -> CdControl(CdlSeekL, loc) -> CdRead2; the reference drive log
 * with -tracepc on every main-binary function and the exception vector).
 * The constants above are the class medians; these paths measure:
 *   - CdControl's Setloc prefix (a positional command given a position):
 *     CD_cw entry -> register write 715 (CD_sync 145 in); the acknowledge's
 *     exception 3,805 (no sync event: the prefix is not the call's last
 *     command; standalone Setlocs deliver it, 4,453..4,489), and CdControl
 *     enters CD_cw for the command 152 ticks after it returns (the class
 *     model charged 984 + 440 there).
 *   - a CD_cw that found the previous command in flight waits inside its
 *     CD_sync (measured on CdRead2's; other callers keep the class model):
 *     entry -> CD_sync 44..74, then the register write 289 ticks
 *     after the completing exception returns (CdRead2's Setmode behind the
 *     SeekL: 179,165 -> 179,454), not a whole entry-to-write 440 after it.
 *   - the SeekL's acknowledge exception 3,463, its completion 4,514
 *     (def_cbsync -> DeliverEvent inside); CdRead2's Setmode acknowledge
 *     4,453, then CD_cw(ReadS) 420 after it returns (DMACallback,
 *     setIntrDMA); the ReadS acknowledge 4,526.
 * CD_PATH_COSTS=0 is the negative control (the class constants). */
#define CD_PREFIX_SETLOC_WRITE_TICKS   715u
#define CD_PREFIX_SETLOC_ACK_EXC_TICKS 3805u
#define CD_PREFIX_TO_NEXT_CW_TICKS     152u
#define CD_CW_TO_SYNC_TICKS            44u
#define CD_CW_WAITED_WRITE_TICKS       289u
#define CD_SEEKL_ACK_EXC_TICKS         3463u
#define CD_SEEKL_DONE_EXC_TICKS        4514u
#define CD_STREAM_SETMODE_ACK_EXC_TICKS 4453u
#define CD_STREAM_SETMODE_TO_NEXT_CW_TICKS 420u
#define CD_READS_ACK_EXC_TICKS         4526u
static int cd_path_costs(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("CD_PATH_COSTS"); on = !(e && e[0] == '0'); }
    return on;
}
static int g_issue_prefix;      /* issue_raw: this is CdControl's Setloc prefix */
static int g_issue_stream;      /* issue_raw: cd_hal_stream_start's commands */
static int g_pending_prefix, g_pending_stream;  /* of g_cmd_pending */
static u32 ack_exception_ticks(int c) {
    if (cd_path_costs()) {
        if (c == CMD_SETLOC && g_pending_prefix) return CD_PREFIX_SETLOC_ACK_EXC_TICKS;
        if (c == CMD_SEEKL) return CD_SEEKL_ACK_EXC_TICKS;
        if (c == CMD_SETMODE && g_pending_stream) return CD_STREAM_SETMODE_ACK_EXC_TICKS;
        if (c == CMD_READS) return CD_READS_ACK_EXC_TICKS;
    }
    return CD_ACK_EXCEPTION_TICKS;
}
static u32 complete_exception_ticks(int c) {
    return c == CMD_SEEKL && cd_path_costs() ? CD_SEEKL_DONE_EXC_TICKS : CD_COMPLETE_EXCEPTION_TICKS;
}
#define CD_ACK_IRQ_TICKS    2400u
#define CD_CW_EXIT_TICKS    1400u
#define CDREAD_SETUP_TICKS  1275u
/* Issued through CdControl/CdControlB, a command that completes at its
 * acknowledge (Getstat, Setloc, Setmode: no second response) takes ~960
 * ticks longer from libcd's callback to CD_cw's return: 2,341..2,480 for
 * those, against 1,310..1,455 for CdControl's Pause/Init/SeekL and for
 * the CD_cw calls CD_init and CdRead make themselves, Getstat and Setmode
 * included (same probe as CDCONTROL_*; 1,362 for CD_init's Getstat). */
#define CD_CW_COMPLETE_EXTRA_TICKS 960u
/* GetlocL / GetlocP are single-response commands too (psx-spx: INT3 with
 * the eight position bytes, no second response), so libcd completes them
 * at the acknowledge.  Measured on the reference emulator (Interpreter), the
 * name entry's check_head_position (CdControlB GetlocL, 2026-10-31): CD_cw
 * to check_head_position's CdPosToInt 31,364 ticks, against 29,249 in the
 * port before; the acknowledge's exception 5,094 against Getstat's 4,777 in
 * the same frames (getintr reads seven more response bytes:
 * CD_ACK_RESPONSE8_EXTRA_TICKS). */

static int getloc_at_ack(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("CD_GETLOC_AT_ACK");   /* =0: negative control */
        on = !(e && e[0] == '0');
    }
    return on;
}
static int completes_at_ack(int cmd) {
    return cmd == CMD_GETSTAT || cmd == CMD_SETLOC || cmd == CMD_SETMODE ||
           ((cmd == CMD_GETLOCL || cmd == CMD_GETLOCP) && getloc_at_ack());
}
static void wait_ack(void) {
    u32 after;
    if (!g_cmd_pending) return;
    advance_to(g_ack_at);
    /* CD_cw resumes once the acknowledge's exception has returned
     * (CD_ACK_EXCEPTION_TICKS, taken in run_until); the rest of the
     * measured span is CD_cw's own exit.  An acknowledge that took no
     * exception (raised with interrupts off) leaves the whole span here. */
    if (cd_path_costs() && g_last_cmd == CMD_SETLOC && g_pending_prefix) {
        /* the prefix: CdControl's next CD_cw (its entry is issue_raw's) */
        hal_cpu_spend(CD_PREFIX_TO_NEXT_CW_TICKS +
                      (g_ack_exception ? 0u : CD_PREFIX_SETLOC_ACK_EXC_TICKS));
        return;
    }
    if (cd_path_costs() && g_last_cmd == CMD_SETMODE && g_pending_stream) {
        hal_cpu_spend(CD_STREAM_SETMODE_TO_NEXT_CW_TICKS +
                      (g_ack_exception ? 0u : CD_STREAM_SETMODE_ACK_EXC_TICKS));
        return;
    }
    after = CD_ACK_IRQ_TICKS + CD_CW_EXIT_TICKS;
    if (g_ack_exception) {
        u32 exc = ack_exception_ticks(g_last_cmd);
        after = after > exc ? after - exc : 0u;
    }
    hal_cpu_spend(after +
                  (g_control_call && completes_at_ack(g_last_cmd) ? CD_CW_COMPLETE_EXTRA_TICKS : 0u));
}
static void wait_complete(void) {
    wait_ack();
    while (g_second_cmd) advance_to(g_second_at);
}

/* Native busy loops otherwise execute forever at a frozen emulated clock.
 * A repeated poll with no intervening clock progress spends a small CPU
 * quantum. Once-per-frame queue polls retain their existing timing. The
 * 128-tick quantum approximates polling overhead; it is not cycle-exact.
 */
static void poll_progress(u64 *last_poll) {
    if (*last_poll == g_now) advance_to(g_now + 128);
    *last_poll = g_now;
}

/* issue a command: CD_cw first waits for the previous one to complete */
static void issue_raw(int cmd, const u8 *param) {
    if (cd_path_costs() && g_issue_stream && (g_cmd_pending || g_second_cmd)) {
        /* CD_cw's CD_sync waits out the command in flight */
        hal_cpu_spend(CD_CW_TO_SYNC_TICKS);
        wait_complete();
        hal_cpu_spend(CD_CW_WAITED_WRITE_TICKS);
    } else {
        wait_complete();
        hal_cpu_spend(cd_path_costs() && g_issue_prefix ? CD_PREFIX_SETLOC_WRITE_TICKS
                                                        : CD_CW_WRITE_TICKS);
    }
    g_pending_prefix = g_issue_prefix;
    g_pending_stream = g_issue_stream;
    g_cmd_pending = cmd;
    g_last_cmd = cmd;
    g_ack_at = g_now + (cmd == CMD_INIT ? ACK_TICKS_INIT : ACK_TICKS);
    g_sync_status = 0;
    if (param) memcpy(g_cmd_param, param, sizeof g_cmd_param); else memset(g_cmd_param, 0, sizeof g_cmd_param);
    CDLOG("issue cmd %d", cmd);
}

/* libcd CdControl/CdControlB/CdControlF: a command that takes a disc position
 * (Play, ReadN, SeekL, SeekP, ReadS -- the libcd_cmd_setloc_flag_tbl table) is preceded by a
 * Setloc with that position when `param` is given (matched CdControl.c:
 * `if (arg1 != 0 && *tblPtr != 0) CD_cw(2, arg1, ...)`). */
static int takes_position(int cmd) {
    return cmd == CMD_PLAY || cmd == CMD_READN || cmd == CMD_SEEKL || cmd == CMD_SEEKP || cmd == CMD_READS;
}
static void issue(int cmd, const u8 *param) {
    if (param && takes_position(cmd)) {
        g_issue_prefix = 1;
        issue_raw(CMD_SETLOC, param);
        g_issue_prefix = 0;
        wait_ack();
    }
    issue_raw(cmd, takes_position(cmd) ? NULL : param);
}

/* ------------------------------------------------------------------ frame clock */
/* libetc's VSync(0) (matched VSync.c / v_wait.c) waits for its own vertical
 * counter, Vcount, to move.  Vcount is incremented by trapIntrVSync, which
 * libetc's trapIntr reaches only after the BIOS has finished its interrupt
 * chain for the VBlank -- measured on the reference emulator (Interpreter):
 * the kernel VBlank event (vblank_counter_tick) runs ~580 ticks after the
 * VBlank and trapIntrVSync ~21,300 ticks after it.  The CPU is still inside
 * the exception then: VSync(0)'s caller resumes only once the handler chain
 * has returned and v_wait has seen the new count.  Probing the instruction
 * after vblank_function's `jal VSync` over 179 boot frames puts that return
 * at VBlank + 24,291 (median; 10th/90th percentile 24,277 / 24,442), with
 * pad_main entered ~80 ticks later.  Until 2026-10-30 the wait ended at the
 * Vcount increment (21,300), ~3,000 ticks before retail resumes.  So the wait
 * ends at the first "VBlank + 24,291" that is still ahead. */
#define VSYNC_RETURN_LATENCY 24291u
/* The wait ends when the edge's exception returns: VSYNC_RETURN_LATENCY was
 * measured with the exception at its constant 24,449 ticks (the libsnd
 * sequencer idle).  With the sequencer's measured work in the exception
 * (pc/hal/cpu_cost.c, pc/hal/system.c deliver_sdk) the return moves with
 * the exception's length: retail's VBlank exceptions in the name entry run
 * 24,514..39,730 ticks and the frame's work starts that much later. */
#define VBLANK_EXCEPTION_MEASURED 24449u
static u64 vsync_return_latency(void) {
    u64 occ = g_last_vblank_occupancy;
    if (!occ) return VSYNC_RETURN_LATENCY;
    return occ >= VBLANK_EXCEPTION_MEASURED
        ? VSYNC_RETURN_LATENCY + (occ - VBLANK_EXCEPTION_MEASURED)
        : VSYNC_RETURN_LATENCY - (VBLANK_EXCEPTION_MEASURED - occ < VSYNC_RETURN_LATENCY
                                  ? VBLANK_EXCEPTION_MEASURED - occ : 0);
}
/* Whether the CPU is inside the last edge's VBlank exception, before
 * trapIntr's last I_STAT read: an interrupt raised now is dispatched in
 * that exception's loop (pc/hal/spu_transfer.c IRQ_IN_VBLANK_MERGE). */
int hal_in_vblank_exception(void) {
    u64 edge = vbl_prev(), occ = g_last_vblank_occupancy;
    return occ && g_now >= edge &&
           g_now + LAST_MASK_READ_TO_RETURN_TICKS < edge + occ;
}
void cd_hal_vblank_tick(void) {
    u64 edge = vbl_prev();
    /* An edge another interrupt's exception absorbed incremented Vcount in
     * that exception: VSync(0) resumes VSYNC_AFTER_RETURN_TICKS after it
     * returned, not at the edge's own exception's return.  Once that
     * exception has returned, the wait is for the next edge. */
    if (edge + vsync_return_latency() <= g_now ||
            (edge == g_absorbed_edge && g_absorbed_resume <= g_now))
        edge = g_next_vblank;
    advance_to(edge);
    if (edge == g_absorbed_edge)
        advance_to(g_absorbed_resume > g_now ? g_absorbed_resume : g_now);
    else
        advance_to(edge + vsync_return_latency());
}

/* ------------------------------------------------------------------ libstr stream */
/* libstr's CdRead2(mode): Setmode(mode) then ReadS from the position the
 * preceding SeekL set; the drive streams from there at the mode's speed
 * until the game pauses it (pc/hal/libpress.c owns the ring).  Returns the
 * LBA the stream starts at. */
static u32 g_stream_lba;
u32 cd_hal_stream_start(u8 mode) {
    u32 start = g_setloc_pending ? g_setloc_lba : (g_drive == DRIVE_SEEKING ? g_seek_target : g_cur_lba);
    g_stream_active = 1;
    g_issue_stream = 1;
    issue(CMD_SETMODE, &mode);
    wait_ack();
    issue(CMD_READS, NULL);     /* from the SeekL target: reads when it lands, or from the head's position */
    g_issue_stream = 0;
    wait_ack();
    g_stream_lba = start;
    CDLOG("stream start @ LBA %u, mode 0x%02X", start, mode);
    return start;
}

/* Has the drive read past disc sector `lba` yet?  If not, spin (libstr's
 * caller polls StGetNext in a tight loop; the console's time passes and its
 * VBlanks fire) until it has.  Returns 0 if the drive is not reading. */
int cd_hal_stream_wait_sector(u32 lba) {
    run_until(g_now);
    while (g_drive == DRIVE_SEEKING || ((g_drive == DRIVE_READING || g_drive == DRIVE_PLAYING) && g_cur_lba <= lba))
        advance_to(g_drive_at);
    return g_drive == DRIVE_READING || g_drive == DRIVE_PLAYING;
}

/* ------------------------------------------------------------------ Psy-Q API */
/* CD_init owns the controller attempt; the unchanged matched CdInit
 * wrapper supplies five retries and installs its default BIOS callbacks.
 * Read the diagnostic format/version from the mapped owner executable.
 * The old idle reset discarded the incoming drive position/speed and
 * skipped every controller command. Keep those inputs until Init consumes
 * them through the ordinary acknowledge/second-response scheduler.
 * Raw register writes, CD mixing and byte-level IRQ dispatch stay open.
 */
extern void ResetCallback(void);

/* Native bootstrap replaces BIOS disc loading. Inherit the minimum known
 * post-load position from the executable's real extent, with the BIOS's
 * double-speed data mode. Within-track phase and BIOS boot CPU work are
 * not emulated; this is an extent-based handoff, not a full BIOS snapshot.
 * Never overwrite an already running drive or fabricate missing metadata.
 */
/* The VBlank phase at the handoff.  The port's clock starts at _main's
 * entry; on the console that moment falls at a definite point of the frame,
 * set by the BIOS boot before it (the PSX.EXE load, then the EXE's own
 * start-up at 0x800422A0, which clears memory for ~65 frames before calling
 * _main).  Measured on the reference emulator (Interpreter, SCPH-5500 fast
 * boot; four no-input and scripted runs, all tick-identical): _main entered
 * at tick 242,720,424, the next VBlank at 242,884,197 -- 163,773 ticks
 * later.  The port's first VBlank fell a whole frame after _main
 * (566,204), which put every later step of the boot ~150,000 ticks late
 * against the frame: the card driver's StartCARD (retail 2,000 ticks
 * before an edge) crossed the edge, _bu_init's slot parity flipped and took
 * a frame longer, and the boot movie's stream started ~200,000 ticks late
 * against the VBlanks, so other sectors absorbed other edges.
 * BOOT_VBLANK_PHASE=0 is the negative control (first VBlank one frame in). */
#define BOOT_MAIN_TO_VBLANK_TICKS 163773u
static void boot_vblank_phase(void)
{
    const char *e = getenv("BOOT_VBLANK_PHASE");
    if (e && e[0] == '0') return;
    g_vbl_first = BOOT_MAIN_TO_VBLANK_TICKS;
    g_vbl_index = 0;
    g_vbl_shifts = 0;
    g_vbl_y2 = VBLANK_Y2_RESET;
    g_next_vblank = vbl_edge(0);
}

/* The last VBlank edge at or before `t` (0 before the first). */
u64 hal_clock_edge_at_or_before(u64 t)
{
    u64 n;
    if (t < g_vbl_first) return 0;
    n = vbl_exact() ? (t - g_vbl_first) * VBLANK_PERIOD_DEN / VBLANK_PERIOD_NUM
                    : (t - g_vbl_first) / TICKS_PER_FRAME;
    while (n && vbl_edge(n) > t) n--;
    while (vbl_edge(n + 1) <= t) n++;
    return vbl_edge(n);
}

s32 cd_hal_boot_handoff(void)
{
    const FileLbaEntry *entry;
    u32 i, sectors;
    if (g_now || g_cmd_pending || g_second_cmd || g_drive != DRIVE_IDLE ||
            g_cur_lba || g_mode) return 0;
    if (!g_vbl_index && !g_frames) boot_vblank_phase();
    for (i = 0; i < g_file_lba_table_count; i++) {
        entry = &g_file_lba_table[i];
        if (!entry->path || strcmp(entry->path, "PSX.EXE")) continue;
        if (entry->size <= SECTOR_SIZE) return 0;
        sectors = entry->size / SECTOR_SIZE;
        if (entry->size % SECTOR_SIZE) sectors++;
        if (entry->lba > 0xffffffffu - LBA_PREGAP - sectors) return 0;
        g_cur_lba = entry->lba + sectors - 1;
        g_subq = g_cur_lba;
        g_subq_tick = g_now;
        g_subq_carry = 0;
        g_header_valid = 1;
        g_double_speed = 1;
        g_mode = 0x80;
        g_sync_status = 2;
        CDLOG("boot extent %u+%u sectors: minimum head %u at 2x",
              entry->lba, sectors, g_subq);
        return 1;
    }
    return 0;
}

s32 CD_init(void)
{
    func_800B4850((const char *)(uintptr_t)0x800B9050u,
                 (const char *)(uintptr_t)0x800E2DFCu);
    g_cb_ready = 0;
    g_cb_sync = 0;
    ResetCallback();
    wait_complete();
    {
        /* CD_init's CD_vol block: ATV 0x80/0/0x80/0, applied (the SPU
         * register writes before it are undone by SpuInit's reset) */
        static const u8 unity[4] = { 0x80, 0x00, 0x80, 0x00 };
        XA_CALL(cd_audio_set_volume, (unity));
    }
    issue_raw(CMD_GETSTAT, 0);
    wait_ack();
    if (g_sync_status == 5) return -1;
    issue_raw(CMD_INIT, 0);
    wait_ack();
    if (g_sync_status == 5) return -1;
    /* issue_raw waits for Init's second response before issuing Demute. */
    issue_raw(CMD_DEMUTE, 0);
    wait_ack();
    if (g_sync_status == 5) return -1;
    wait_complete();
    return g_sync_status == 2 ? 0 : -1;
}

void cd_init_error_printf(void *marker)
{
    (void)marker;
    func_800B4850((const char *)(uintptr_t)0x800B8C40u);
}

/* The wrappers' own CPU work around CD_cw (the reference emulator
 * Interpreter, warm, a 1,500-frame no-input boot; the CD queue worker's
 * calls): CdControl reaches CD_cw after 74..101 ticks and returns 92 after
 * it; CdControlB reaches CD_cw after 112..114, and after CD_cw returns its
 * completion check (CD_sync(0)) and return take 449. */
#define CDCONTROL_ENTRY_TICKS   90u
#define CDCONTROL_EXIT_TICKS    92u
#define CDCONTROLB_ENTRY_TICKS  113u
#define CDCONTROLB_EXIT_TICKS   449u

s32 CdControl(u8 cmd, u8 *param, u8 *result) {
    hal_cpu_spend(CDCONTROL_ENTRY_TICKS);
    g_control_call = 1;
    issue(cmd, param);
    wait_ack();
    g_control_call = 0;
    hal_cpu_spend(CDCONTROL_EXIT_TICKS);
    if (result) memcpy(result, g_result, 8);
    return 1;
}

s32 CdControlB(u8 cmd, u8 *param, u8 *result) {
    hal_cpu_spend(CDCONTROLB_ENTRY_TICKS);
    g_control_call = 1;
    issue(cmd, param);
    wait_complete();
    g_control_call = 0;
    hal_cpu_spend(CDCONTROLB_EXIT_TICKS);
    if (result) memcpy(result, g_result, 8);
    return 1;
}

s32 CdControlF(u8 cmd, u8 *param) {
    issue(cmd, param);
    return 1;
}

/* CdSync(1)'s CPU work (same probe, 233 polls): 493 ticks entry to
 * return when the last command has completed, 377 while it is still
 * running.  Blocking CdSync(0) is a wait and is not given a cost. */
#define CDSYNC_POLL_DONE_TICKS  493u
#define CDSYNC_POLL_BUSY_TICKS  377u

s32 CdSync(s32 mode, u8 *result) {
    run_until(g_now);
    if (mode == 0) wait_complete();
    else hal_cpu_spend(g_cmd_pending || g_second_cmd ? CDSYNC_POLL_BUSY_TICKS
                                                    : CDSYNC_POLL_DONE_TICKS);
    if (result) { g_result[0] = drive_stat(); memcpy(result, g_result, 8); }
    if (g_cmd_pending || g_second_cmd) {
        if ((g_frames % 60) != 0) return 0;
        CDLOG("CdSync busy: pending=%d second=%d ack_at=%llu second_at=%llu drive_at=%llu",
              g_cmd_pending, g_second_cmd,
              (unsigned long long)g_ack_at, (unsigned long long)g_second_at,
              (unsigned long long)g_drive_at);
        return 0;
    }
    return g_sync_status;
}

/* CdRead: ReadN of `sectors` sectors from the last Setloc into `buf` (a PS1
 * address; libcd's data callback would DMA each sector there). */
s32 CdRead(s32 sectors, u32 *buf, s32 mode) {
    u8 m = (u8)mode;
    if (!buf || sectors <= 0) return 0;
    /* libcd's own setup before its first CD_cw (CD_readm, the retry state):
     * retail CdRead entry -> Setmode write is 1,715 ticks (median of nine
     * boot reads, minimum 1,147), of which CD_cw's own 440 is charged by
     * issue_raw. */
    hal_cpu_spend(CDREAD_SETUP_TICKS);
    /* libcd: CdRead issues Setmode(mode) then ReadN (cd_init_retry) */
    issue(CMD_SETMODE, &m);
    wait_ack();
    g_xfer_active = 1;
    g_xfer_remaining = sectors;
    g_xfer_total = (u32)sectors;
    g_fade_hold = g_fade_released = 0;
    g_xfer_ps1_dest = (u32)(uintptr_t)buf;
    g_xfer_first_lba = g_setloc_pending ? g_setloc_lba : g_cur_lba;
    issue(CMD_READN, NULL);
    wait_ack();
    CDLOG("CdRead %d sectors @ LBA %u -> 0x%08X", sectors, g_xfer_first_lba, g_xfer_ps1_dest);
    return 1;
}

s32 CdReadSync(s32 mode, u8 *result) {
    static u64 last_poll = (u64)-1;
    run_until(g_now);
    if (mode == 0) while (g_xfer_active) advance_to(g_drive_at);
    else if (g_xfer_active) poll_progress(&last_poll);
    if (result) { g_result[0] = drive_stat(); memcpy(result, g_result, 8); }
    return g_xfer_active ? g_xfer_remaining : 0;
}

/* CdReady(1, result): 1 (CdlDataReady) when a sector has arrived since the
 * last poll, 0 (CdlNoIntr) otherwise; 5 would be a disc error, which the
 * model never raises.  twinbee (a matched game function) polls it for 5. */
s32 CdReady(s32 mode, u8 *result) {
    static u64 last_seen;
    s32 r;
    run_until(g_now);
    if (result) { g_result[0] = drive_stat(); memcpy(result, g_result, 8); }
    r = ((g_drive == DRIVE_READING || g_drive == DRIVE_PLAYING) && g_drive_last > last_seen) ? 1 : 0;
    last_seen = g_drive_last;
    (void)mode;
    return r;
}

void *CdReadyCallback(void *func) { void *o = g_cb_ready; g_cb_ready = func; return o; }
void *CdSyncCallback(void *func)  { void *o = g_cb_sync;  g_cb_sync = func;  return o; }
void *CdDataCallback(void *func)  { void *o = g_cb_data;  g_cb_data = func;  return o; }
void *CdReadCallback(void *func)  { void *o = g_cb_read;  g_cb_read = func;  return o; }

/* libcd's CD_datasync (wait for the read DMA): with the model the data is
 * in place when CdReadSync reports 0, so there is nothing to wait for.  Kept
 * because a library-region game function (normal_date_bg_in_sub) and
 * CdDataSync call it directly. */
s32 CD_datasync(s32 once) { (void)once; return 0; }

void CdFlush(void) { }
s32 CdSetDebug(s32 level) { (void)level; return 0; }
s32 CdDiskReady(s32 mode) { (void)mode; return 2; }
s32 CdGetSector(void *buf, s32 size) { (void)buf; (void)size; return 0; }
s32 CdGetToc(void *toc) { (void)toc; return 0; }
s32 CdPlay(s32 mode, void *param, s32 vol) { (void)mode; (void)param; (void)vol; return 1; }
/* CdMix (matched CdMix -> CD_vol): after CD_sync(0) waits out the command
 * in flight, the four ATV attenuations go to the controller (index 2: CD-L
 * to SPU-L / SPU-R; index 3: CD-R to SPU-R / SPU-L) and the 0x20 write
 * applies them.  SD_StereoModeSet sets {127,0,127,0} (stereo) or 79 x 4
 * (mono). */
s32 CdMix(void *vol) {
    HAL_SDK_COST("CdMix");   /* CD_vol and its CD_sync folded in (BOOT_ROWS) */
    wait_complete();
    if (vol) XA_CALL(cd_audio_set_volume, ((const u8 *)vol));
    return 1;
}

/* CdlLOC is BCD minute/second/sector, 150 sectors of lead-in (libcd). */
static u8 to_bcd(u32 v) { return (u8)(((v / 10) << 4) | (v % 10)); }
static u32 from_bcd(u8 b) { return (u32)((b >> 4) * 10 + (b & 0x0F)); }

/* libcd's CdPosToInt / CdIntToPos: their retail CPU time when game code
 * calls them (cpu_cost.h HAL_SDK_COST); the drive model uses pos_to_int. */
s32 CdPosToInt(void *pos) {
    HAL_SDK_COST("CdPosToInt");
    return pos_to_int(pos);
}
static s32 pos_to_int(void *pos) {
    u8 *loc = (u8 *)pos;
    if (!loc) return (s32)g_cur_lba;
    return (s32)((from_bcd(loc[0]) * 60 + from_bcd(loc[1])) * 75 + from_bcd(loc[2])) - 150;
}
void CdIntToPos(s32 i, void *pos) {
    u8 *loc = (u8 *)pos;
    u32 sec = (u32)i + 150;
    HAL_SDK_COST("CdIntToPos");
    if (!loc) return;
    loc[0] = to_bcd(sec / 4500);
    loc[1] = to_bcd((sec % 4500) / 75);
    loc[2] = to_bcd(sec % 75);
    loc[3] = 0;
}

/* ------------------------------------------------------------------ files */
s32 CdSearchFile(void *fp_, const char *name) {
    typedef struct { u32 pos; u32 size; char name[16]; } CdlFILE;
    CdlFILE *fp = (CdlFILE *)fp_;
    char path[512];
    if (!name) return 0;
    printf("[HAL/CD] CdSearchFile: '%s'\n", name);
    if (assets_resolve_path(name, path, sizeof(path))) {
        printf("[HAL/CD]   Found: %s\n", path);
        if (fp) {
            FILE *f = fopen(path, "rb");
            if (f) { fseek(f, 0, SEEK_END); fp->size = (u32)ftell(f); fclose(f); }
            fp->pos = 0;
            strncpy(fp->name, name, sizeof(fp->name) - 1);
        }
        strncpy(g_cd_current_file, path, sizeof(g_cd_current_file) - 1);
        return 1;
    }
    printf("[HAL/CD]   NOT FOUND: %s\n", name);
    return 0;
}

/* The file the last CdSearchFile() resolved: the stream source for
 * pc/hal/libpress.c (CdRead2 / StGetNext), which has no sector-level CD. */
const char *cd_hal_current_file(void) {
    return g_cd_current_file[0] ? g_cd_current_file : NULL;
}

/* the sector libstr's CdRead2 streams from: the last Setloc / SeekL target
 * (the seek may still be in flight when strKickCD calls CdRead2) */
s32 cd_hal_current_sector(void) {
    return (s32)(g_setloc_pending ? g_setloc_lba : (g_drive == DRIVE_SEEKING ? g_seek_target : g_cur_lba));
}

/* The disc file that contains `sector`, as a path under the extracted
 * tree (pc/src/file_lba_table.c; exact start first, then the file whose
 * extent covers the sector).  For the streaming reads (libstr) that seek by
 * sector instead of CdSearchFile. */
const char *cd_hal_file_for_sector(s32 sector, char *out, int max_len) {
    const FileLbaEntry *e = file_id_lookup_lba((u32)sector);
    u32 i;
    if (!e) {
        for (i = 0; i < g_file_lba_table_count; i++) {
            const FileLbaEntry *c = &g_file_lba_table[i];
            u32 nsec = (c->size + 2047) / 2048;
            if ((u32)sector >= c->lba && (u32)sector < c->lba + nsec) { e = c; break; }
        }
    }
    if (!e) return NULL;
    snprintf(out, max_len, "%s/raw/%s", pc_asset_dir(), e->path);
    {
        char en[1024];
        const char *p = lang_path(out, en, sizeof en);
        if (p != out) snprintf(out, max_len, "%s", p);
    }
    return out;
}

/* pc/hal/save_auto.c: an automatic save state is taken only while the
 * drive has nothing in flight -- no command waiting for its acknowledge or
 * second response, no Setloc / seek pending, no CdRead transfer.  (A
 * streamed XA / libstr read is steady playback a state carries exactly,
 * pc/tools/test_quicksave.py; it does not count.)  Observation only. */
int hal_cd_quiet(void) {
    return !g_cmd_pending && !g_second_cmd && !g_setloc_pending && !g_implicit_seek &&
           !g_xfer_active;
}
