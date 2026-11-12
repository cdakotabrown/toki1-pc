/* SPDX-License-Identifier: GPL-3.0-or-later */
/* memcard.c -- PSX memory-card emulation backed by a .mcr image.
 *
 * Tokimeki calls the PSX BIOS file API (`FileOpen("bu00:BISLPS-00065...",
 * mode)` / `FileRead` / `FileWrite` / `FileClose`) to persist player state
 * onto a memory card. On PC we back this with a standard 128 KB PSX memcard
 * image (`saves/card1.mcr`, or the folder pc/hal/save_paths.c names) so any PS1 memcard tool (MemcardRex, mcrtrans,
 * PS1 emulators) can inspect, edit, or swap the card.
 *
 * Layout (PSX standard, 16 blocks x 8192 bytes):
 *   Block  0: directory (64 frames x 128 bytes)
 *     Frame 0     : header "MC" + checksum
 *     Frame 1..15 : one directory entry per data block
 *     Frame 16..35: broken-sector list (unused by us)
 *     Frame 36..62: test/reserved
 *     Frame 63    : backup header "MC"
 *   Block 1..15: raw 8 KB save data.
 *
 * Directory entry (128 bytes):
 *   0x00..03 state  (u32 LE) 0x51 first/used, 0x52 middle, 0x53 last,
 *                            0xA0 fresh, 0xA1..A3 deleted variants.
 *   0x04..07 filesize (u32 LE, bytes)
 *   0x08..09 next block link (u16 LE, 0xFFFF = terminator)
 *   0x0A..1D filename (20 bytes, NUL-terminated, ASCII)
 *   0x1E..7E reserved (zero)
 *   0x7F     XOR checksum of bytes 0x00..0x7E
 *
 * FileOpen("bu00:NAME") semantics:
 *   - Read:  find a used entry whose filename starts with NAME; fd maps to
 *            the entry's block chain. FileRead/Seek operate on the logical
 *            concatenation of those blocks.
 *   - Write: if NAME exists, open in place; otherwise allocate a single
 *            free block (0x51, next=0xFFFF), default size 8192, and update
 *            the directory.
 *
 * Seed pipeline: any `saves/seed/*.mcs` file (standard single-save export,
 * 128 B dir frame + 8192 B data) is injected at init time if a save with
 * the same filename isn't already on the card. Lets you drop a ripped save
 * in and boot straight into it for debugging.
 *
 * What callers see from the BIOS layer:
 *   _bu_init()          -> scans both slots, waits for sector completions
 *   _card_load/info     -> registers asynchronous requests, raises events
 *   _card_status(slot)  -> pending operation, idle, timeout or error
 *   FileOpen/Read/Write/Close/Seek -> operate on the .mcr image.
 */

#include <SDL2/SDL.h>
#include "hal.h"
#include "pc_debug.h"
#include "pc_irq.h"
#include "profile.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>

#define MEMCARD_MAX_FD    8
/* Where the card image lives: pc/hal/save_paths.c (TOKI_SAVE_DIR, the
 * settings' [system] save_dir, else the historical cwd-relative "saves").
 * Weak, so the harnesses that #include this file alone keep the literals. */
extern const char *pc_save_dir(void) __attribute__((weak));
extern const char *pc_save_card_path(void) __attribute__((weak));
extern const char *pc_save_seed_dir(void) __attribute__((weak));
#define SAVE_DIR          (pc_save_dir ? pc_save_dir() : "saves")
#define SEED_DIR          (pc_save_seed_dir ? pc_save_seed_dir() : "saves/seed")
#define MC_PATH           (pc_save_card_path ? pc_save_card_path() : "saves/card1.mcr")
#define MC_SIZE           (128 * 1024)
#define MC_BLOCK_SIZE     8192
#define MC_BLOCK_COUNT    16
#define MC_DATA_BLOCKS    15
#define MC_FRAME_SIZE     128
#define MC_DIR_FRAMES     64

#define DE_STATE_FREE_FRESH 0xA0
#define DE_STATE_USED_FIRST 0x51
#define DE_STATE_USED_MID   0x52
#define DE_STATE_USED_LAST  0x53
#define DE_LINK_TERM        0xFFFF

#define MCS_HEADER_SIZE 128
#define MCS_TOTAL_SIZE  (MCS_HEADER_SIZE + MC_BLOCK_SIZE)

typedef struct {
    FILE *fp;
    int   in_use;
    int   is_memcard;
    long  file_size;        /* for non-memcard / info only */
    /* memcard-specific fd state */
    int   dir_entry;        /* 1..15, index of first dir entry */
    int   cur_pos;          /* byte offset within logical file */
    int   allocated_bytes;  /* total bytes across block chain */
    int   writable;
    int   async;            /* FASYNC (0x8000): FileRead/FileWrite return at once */
    char  path[128];
} MemCardFd;

/* The BIOS keeps descriptors 0 and 1 for the tty, so the first file opened
 * is 2 (retail FileOpen returns 2 to the game's card_fd). */
#define MEMCARD_FD_BASE 2

static MemCardFd g_fds[MEMCARD_MAX_FD];
static int       g_bu_initialized = 0;
static u8        g_card[MC_SIZE];
static int       g_card_dirty = 0;

static void ensure_dir(const char *p) {
    struct stat st;
    if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) return;
    mkdir(p, 0755);
}

/* ---- raw block helpers ------------------------------------------------- */

static u8 *dir_frame(int i) {
    return g_card + i * MC_FRAME_SIZE;
}

static u8 *block_data(int block) {
    return g_card + block * MC_BLOCK_SIZE;
}

static u8 xor_checksum(const u8 *p, int n) {
    u8 x = 0;
    for (int i = 0; i < n; i++) x ^= p[i];
    return x;
}

static void header_frame_write(u8 *f) {
    memset(f, 0, MC_FRAME_SIZE);
    f[0] = 'M';
    f[1] = 'C';
    f[MC_FRAME_SIZE - 1] = xor_checksum(f, MC_FRAME_SIZE - 1);
}

/* Directory entry accessors. Frame index = data block number (1..15). */

static u32 de_state(int idx)  { u32 v; memcpy(&v, dir_frame(idx) + 0x00, 4); return v; }
static u32 de_size(int idx)   { u32 v; memcpy(&v, dir_frame(idx) + 0x04, 4); return v; }
static u16 de_link(int idx)   { u16 v; memcpy(&v, dir_frame(idx) + 0x08, 2); return v; }
static const char *de_name(int idx) { return (const char *)(dir_frame(idx) + 0x0A); }

static void de_seal(int idx) {
    u8 *f = dir_frame(idx);
    f[MC_FRAME_SIZE - 1] = xor_checksum(f, MC_FRAME_SIZE - 1);
}

static void de_set_free(int idx) {
    u8 *f = dir_frame(idx);
    memset(f, 0, MC_FRAME_SIZE);
    f[0] = DE_STATE_FREE_FRESH;
    memcpy(f + 0x08, &(u16){DE_LINK_TERM}, 2);
    de_seal(idx);
}

static void de_set_single(int idx, const char *name, u32 size) {
    u8 *f = dir_frame(idx);
    memset(f, 0, MC_FRAME_SIZE);
    u32 state = DE_STATE_USED_FIRST;
    u16 link  = DE_LINK_TERM;
    memcpy(f + 0x00, &state, 4);
    memcpy(f + 0x04, &size,  4);
    memcpy(f + 0x08, &link,  2);
    strncpy((char *)(f + 0x0A), name, 20);
    f[0x0A + 19] = 0;
    de_seal(idx);
}

/* Find first entry whose filename matches `name` exactly or (if strict=0)
 * as a prefix (covers game code that may pass a short key). */
static int dir_find(const char *name, int strict) {
    for (int i = 1; i <= MC_DATA_BLOCKS; i++) {
        u32 s = de_state(i);
        if (s != DE_STATE_USED_FIRST) continue;
        const char *n = de_name(i);
        if (strict) {
            if (strcmp(n, name) == 0) return i;
        } else {
            if (strncmp(n, name, strlen(name)) == 0) return i;
        }
    }
    return -1;
}

static int dir_find_free(void) {
    for (int i = 1; i <= MC_DATA_BLOCKS; i++) {
        u32 s = de_state(i);
        if (s == DE_STATE_FREE_FRESH || (s & 0xF0) == 0xA0) return i;
    }
    return -1;
}

/* ---- mcr image I/O ----------------------------------------------------- */

static void card_format(void) {
    memset(g_card, 0, sizeof(g_card));
    header_frame_write(dir_frame(0));
    for (int i = 1; i <= MC_DATA_BLOCKS; i++) de_set_free(i);
    /* frames 16..35: broken sector list, default 0xFFFF-link, checksum seal */
    for (int i = 16; i < 36; i++) {
        u8 *f = dir_frame(i);
        memset(f, 0xFF, 4);                 /* block number: none */
        memcpy(f + 0x08, &(u16){DE_LINK_TERM}, 2);
        f[MC_FRAME_SIZE - 1] = xor_checksum(f, MC_FRAME_SIZE - 1);
    }
    header_frame_write(dir_frame(63));
    g_card_dirty = 1;
}

/* milliseconds, monotonic (no SDL: the card test harnesses link this alone) */
static unsigned long long card_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
}

/* The file is written off the game thread (pc/hal/card_writer.c: a slow
 * host write -- antivirus scanning a new file on Windows took 5 s -- must
 * not stall a frame); the image is copied now, so later card writes do not
 * race the file write.  Weak: the card harnesses link this file alone, and
 * write on the calling thread as before. */
extern void card_writer_submit(const char *path, const unsigned char *img, size_t n)
    __attribute__((weak));
static void card_flush(void) {
    unsigned long long t0;
    FILE *fp;
    if (!g_card_dirty) return;
    if (card_writer_submit) {
        card_writer_submit(MC_PATH, g_card, MC_SIZE);
        g_card_dirty = 0;
        return;
    }
    t0 = card_ms();
    fp = fopen(MC_PATH, "wb");
    if (!fp) {
        fprintf(stderr, "[memcard] flush: cannot write %s: %s\n",
                MC_PATH, strerror(errno));
        return;
    }
    fwrite(g_card, 1, MC_SIZE, fp);
    fclose(fp);
    g_card_dirty = 0;
    if (card_ms() - t0 > 250)
        fprintf(stderr, "[memcard] flush took %llu ms (%s)\n", card_ms() - t0, MC_PATH);
}

static int card_load_or_format(void) {
    FILE *fp = fopen(MC_PATH, "rb");
    if (!fp) {
        card_format();
        card_flush();
        fprintf(stderr, "[memcard] formatted new card at %s\n", MC_PATH);
        return 1;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz != MC_SIZE) {
        fprintf(stderr, "[memcard] %s size %ld != %d, reformatting\n",
                MC_PATH, sz, MC_SIZE);
        fclose(fp);
        card_format();
        card_flush();
        return 1;
    }
    if (fread(g_card, 1, MC_SIZE, fp) != MC_SIZE) {
        fprintf(stderr, "[memcard] short read on %s, reformatting\n", MC_PATH);
        fclose(fp);
        card_format();
        card_flush();
        return 1;
    }
    fclose(fp);
    /* Sanity: first frame must be "MC". If not, format. */
    u8 *h = dir_frame(0);
    if (h[0] != 'M' || h[1] != 'C') {
        fprintf(stderr, "[memcard] bad header in %s, reformatting\n", MC_PATH);
        card_format();
        card_flush();
    }
    return 0;
}

/* ---- seed injection ---------------------------------------------------- */

/* Inject a single .mcs (8320-byte single-save export) into the card.
 * Returns 1 if injected, 0 if skipped (already present / invalid). */
static int inject_mcs_file(const char *mcs_path) {
    FILE *fp = fopen(mcs_path, "rb");
    if (!fp) return 0;
    u8 buf[MCS_TOTAL_SIZE];
    size_t n = fread(buf, 1, MCS_TOTAL_SIZE, fp);
    fclose(fp);
    if (n != MCS_TOTAL_SIZE) {
        fprintf(stderr, "[memcard] seed skip (bad size %zu): %s\n", n, mcs_path);
        return 0;
    }
    const char *name = (const char *)(buf + 0x0A);
    char nm[24];
    strncpy(nm, name, 20);
    nm[20] = 0;
    if (nm[0] == 0) {
        fprintf(stderr, "[memcard] seed skip (no name): %s\n", mcs_path);
        return 0;
    }
    if (dir_find(nm, 1) >= 0) {
        fprintf(stderr, "[memcard] seed skip (exists): %s\n", nm);
        return 0;
    }
    int slot = dir_find_free();
    if (slot < 0) {
        fprintf(stderr, "[memcard] seed skip (card full): %s\n", nm);
        return 0;
    }
    /* Copy dir frame + data block. */
    memcpy(dir_frame(slot), buf, MC_FRAME_SIZE);
    de_seal(slot);
    memcpy(block_data(slot), buf + MC_FRAME_SIZE, MC_BLOCK_SIZE);
    g_card_dirty = 1;
    PC_DEBUG_MEM("seeded '%s' -> block %d", nm, slot);
    return 1;
}

static void seed_inject_all(void) {
    DIR *d = opendir(SEED_DIR);
    if (!d) return;
    int injected = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len < 4) continue;
        if (strcasecmp(e->d_name + len - 4, ".mcs") != 0) continue;
        char path[1300];
        snprintf(path, sizeof(path), "%s/%s", SEED_DIR, e->d_name);
        injected += inject_mcs_file(path);
    }
    closedir(d);
    if (injected) card_flush();
}

/* ---- BIOS surface ------------------------------------------------------ */

void memcard_init(void) {
    if (g_bu_initialized) return;
    memset(g_fds, 0, sizeof(g_fds));
    ensure_dir(SAVE_DIR);
    ensure_dir(SEED_DIR);
    card_load_or_format();
    seed_inject_all();
    g_bu_initialized = 1;
    PC_DEBUG_MEM("init: %s (%d blocks)", MC_PATH, MC_DATA_BLOCKS);
}

/* Legacy native movie-completion gate (its user, the old str_player.c, is gone).
 * BIOS card requests deliver kernel events; they do not own this game flag. */
extern u8 D_800FF869;
void memcard_mark_ready(void) { D_800FF869 = 1; }

/* Serial-card requests share the CD/CPU clock. The BIOS alternates slots
 * at VBlank; submitting a request does not transfer bytes or raise events.
 * SCPH-5500 probes (2026-10-24) measure about 208k ticks from slot service
 * to read completion and 204k for writes. An absent device times out at
 * the following VBlank's cleanup, about 20k ticks after that edge.
 * These bounded service costs include BIOS/serial work, not a boot delay.
 * Scheduling adds about 19k ticks for service dispatch after VBlank.
 * Byte-level SIO IRQ timing and controller contention remain unmodelled. */
extern u64 hal_clock_ticks(void);
extern void hal_cpu_spend(u32 ticks);
extern u32 hal_cpu_ticks_to_vblank(void);
extern u32 hal_clock_ticks_per_frame(void);
extern void hal_bios_deliver_event(u32 cls, u32 spec);

#define CARD_HW_EVENT 0xF0000011u
#define CARD_SW_EVENT 0xF4000001u
#define CARD_IOE      0x0004u
#define CARD_TIMEOUT  0x0100u
#define CARD_ERROR    0x8000u
#define CARD_NEW      0x2000u
#define CARD_NO_EVENT ((u64)-1)

typedef struct {
    int operation;                 /* 2 read, 4 write, 8 connection test */
    int sector;
    u8 *buffer;
    u8 write_data[MC_FRAME_SIZE];
    int status;
    int result;
    int directory_stage;           /* -1 idle; 0 header; 1 backup; 2..36 */
    u8 directory[36][MC_FRAME_SIZE];
    u8 transfer[MC_FRAME_SIZE];
    int software_request;
    int file_transfer;             /* issued by the FileRead/FileWrite driver */
    int suppress_new;
} CardRequest;

static CardRequest g_requests[2];

/* FileRead/FileWrite on bu00: one sector request after another on slot 0,
 * each issued when the previous one completes (the BIOS file driver runs
 * from the card completion, not from the caller).  Measured on
 * the reference emulator, SCPH-5500 (2026-10-31; -tracepc on the game's
 * open/read/write/close/_card_status thunks, -log Dev for the card's own
 * sector log): an 0x2000-byte FileWrite on an 0x8002 descriptor returns in
 * 20k ticks, then the card writes one sector every second VBlank (slot 0's
 * turn) -- 64 sectors in 128 VBlanks -- and _card_status(0) reads 4 the
 * whole time, 8 for one more slot-0 turn after the last sector (a closing
 * request with no data, which the card's sector log does not show), then 1.
 * An 0x8001 FileRead is the same with status 2 and no closing request.
 * FileClose during the transfer does not stop it (the game closes the
 * descriptor on every poll).  Without FASYNC (0x8000) the call returns
 * when the transfer is done. */
typedef struct {
    int active;
    int operation;                 /* 2 read, 4 write */
    int sector;                    /* the sector in flight */
    int end;                       /* one past the last data sector */
    u8 *buffer;                    /* the caller's buffer for `sector` */
    int closing;                   /* the write's closing request is in flight */
} FileTransfer;

static FileTransfer g_file_xfer;
static int g_card_driver_initialized;
static int g_card_driver_running;
static int g_card_slot;
static int g_card_active_slot = -1;
static int g_card_auto = 1;
static int g_card_unconfirmed = 1;
static int g_card_suppress_new;
static u64 g_card_event = CARD_NO_EVENT;

static int card_slot(long chan)
{
    /* No multitap or second card is installed in the native fixture. */
    if (chan == 0) return 0;
    if (chan == 0x10) return 1;
    return -1;
}

void memcard_card_init(void)
{
    int i;
    if (!g_bu_initialized) memcard_init();
    memset(g_requests, 0, sizeof(g_requests));
    for (i = 0; i < 2; i++) {
        g_requests[i].status = 1;
        g_requests[i].directory_stage = -1;
    }
    g_card_slot = 0;
    g_card_active_slot = -1;
    g_card_event = CARD_NO_EVENT;
    g_card_unconfirmed = 1;
    g_card_suppress_new = 0;
    g_card_driver_running = 0;
    memset(&g_file_xfer, 0, sizeof(g_file_xfer));
    g_card_driver_initialized = 1;
}

void memcard_card_start(void)
{
    if (!g_card_driver_initialized) memcard_card_init();
    g_card_driver_running = 1;
}

long memcard_card_auto(long enabled)
{
    int previous;
    previous = g_card_auto;
    g_card_auto = enabled != 0;
    return previous;
}

void memcard_new_card(void) { g_card_suppress_new = 1; }

/* CARD_IO_TIMING=0 -- instant card I/O (the enhanced profile,
 * pc/hal/profile.c; docs/pc-port/ENHANCED-MODE.md).  A request is serviced
 * as soon as it is submitted (or the slot frees), not at its slot's turn
 * every second VBlank, and completes CARD_INSTANT_TICKS later with the
 * same events, statuses and data as the timed model.  A 64-sector album
 * write takes well under a frame instead of 128 VBlanks.  Parity (=1)
 * never reaches card_instant_kick. */
#define CARD_INSTANT_TICKS 2000u
static int card_io_timing(void)
{
    static int on = -1;
    if (on < 0) on = hal_model_on("CARD_IO_TIMING");
    return on;
}

static void card_instant_kick(void)
{
    CardRequest *request;
    int slot;
    if (!g_card_driver_running || g_card_active_slot >= 0) return;
    for (slot = 0; slot < 2; slot++) {
        request = &g_requests[slot];
        if (!request->operation) continue;
        if (request->operation == 4 && request->buffer)
            memcpy(request->write_data, request->buffer, MC_FRAME_SIZE);
        g_card_active_slot = slot;
        g_card_event = hal_clock_ticks() + CARD_INSTANT_TICKS;
        return;
    }
}

static int card_submit(int slot, int operation, int sector, u8 *buffer)
{
    CardRequest *request;
    request = &g_requests[slot];
    if (request->operation || sector < 0 || sector >= MC_SIZE / MC_FRAME_SIZE)
        return 0;
    request->operation = operation;
    request->sector = sector;
    request->buffer = buffer;
    request->status = operation;
    request->result = 0;
    request->suppress_new = g_card_suppress_new;
    g_card_suppress_new = 0;
    if (operation == 4) {
        if (buffer) memcpy(request->write_data, buffer, MC_FRAME_SIZE);
        else memcpy(request->write_data, dir_frame(63), MC_FRAME_SIZE);
    }
    PC_DEBUG_MEM("request slot=%d op=%d sector=%d tick=%llu",
                 slot, operation, sector, hal_clock_ticks());
    if (!card_io_timing()) card_instant_kick();
    return 1;
}

static long card_raw_request(long chan, long sector, u8 *buffer, int operation)
{
    int slot;
    slot = card_slot(chan);
    if (!g_card_driver_initialized || slot < 0 ||
        sector < 0 || sector >= MC_SIZE / MC_FRAME_SIZE)
        return 0;
    if (g_requests[slot].directory_stage >= 0) return 0;
    if (buffer) {
        buffer = hal_ps1_ptr(buffer);
        if (!pc_ptr_is_mapped(buffer, MC_FRAME_SIZE)) return 0;
    } else if (operation != 4 || sector != 63) {
        return 0;
    }
    return card_submit(slot, operation, (int)sector, buffer);
}

long memcard_card_read(long chan, long sector, u8 *buffer)
{
    return card_raw_request(chan, sector, buffer, 2);
}

long memcard_card_write(long chan, long sector, const u8 *buffer)
{
    return card_raw_request(chan, sector, (u8 *)buffer, 4);
}

void memcard_vblank_tick(void)
{
    CardRequest *request;
    if (!g_card_driver_running) return;
    if (!card_io_timing()) {
        card_instant_kick();
        return;
    }
    /* A failed device probe occupies the slot through next VBlank's
     * timeout cleanup (BIOS 0x5058); that path does not toggle slots. */
    if (g_card_active_slot >= 0) return;
    /* BIOS 0x5128 toggles the slot even when it has no pending request. */
    g_card_slot = 1 - g_card_slot;
    request = &g_requests[g_card_slot];
    if (!request->operation) return;
    /* The card is sent the caller's bytes now, not when it was queued. */
    if (request->operation == 4 && request->buffer)
        memcpy(request->write_data, request->buffer, MC_FRAME_SIZE);
    g_card_active_slot = g_card_slot;
    g_card_event = hal_clock_ticks() + (g_card_slot == 1 ?
        hal_clock_ticks_per_frame() + 20000u :
        request->operation == 4 ? 223000u :
        request->operation == 8 ? 24000u : 228000u);
}

u64 memcard_next_event(void) { return g_card_event; }

static int card_backup_always(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("CARD_LOAD_BACKUP_ALWAYS");
        on = e && e[0] == '1';
    }
    return on;
}

static void card_directory_next(int slot)
{
    CardRequest *request;
    int stage;
    request = &g_requests[slot];
    stage = request->directory_stage;
    g_card_suppress_new = 1;       /* BIOS calls _new_card before each sector */
    card_submit(slot, stage == 1 ? 4 : 2,
                stage == 1 ? 63 : stage == 0 ? 0 : stage - 1,
                request->transfer);
}

static void file_transfer_submit(int operation, int sector, u8 *buffer)
{
    g_card_suppress_new = 1;       /* BIOS calls _new_card before each sector */
    card_submit(0, operation, sector, buffer);
    g_requests[0].file_transfer = 1;
}

/* A file-driver request on slot 0 completed with `event`. */
static void file_transfer_next(u32 event)
{
    FileTransfer *x = &g_file_xfer;
    if (!x->active) return;
    if (event != CARD_IOE || x->closing) {
        x->active = 0;
        hal_bios_deliver_event(CARD_SW_EVENT, event);
        return;
    }
    x->sector++;
    x->buffer += MC_FRAME_SIZE;
    if (x->sector < x->end) {
        file_transfer_submit(x->operation, x->sector, x->buffer);
    } else if (x->operation == 4) {
        x->closing = 1;
        file_transfer_submit(8, 0, NULL);
    } else {
        x->active = 0;
        hal_bios_deliver_event(CARD_SW_EVENT, event);
    }
}

/* Spend CPU time (VBlanks and card events run) while *busy. */
static void card_spin_while(const int *busy)
{
    while (*busy) {
        u64 next, now;
        u32 ticks;
        now = hal_clock_ticks();
        next = memcard_next_event();
        ticks = hal_cpu_ticks_to_vblank();
        if (next != CARD_NO_EVENT && next > now && next - now < ticks)
            ticks = (u32)(next - now);
        hal_cpu_spend(ticks ? ticks : 1u);
    }
}

/* Start a FileRead (2) / FileWrite (4) of `nbytes` at byte `pos` of the file
 * in data block `block`.  -> nbytes, or -1 when the slot is busy. */
static int file_transfer_start(int operation, int block, int pos, u8 *buffer,
                               int nbytes, int wait)
{
    FileTransfer *x = &g_file_xfer;
    CardRequest *r = &g_requests[0];
    int sectors;
    if (x->active || r->operation || r->directory_stage >= 0) return -1;
    if (pos % MC_FRAME_SIZE || nbytes % MC_FRAME_SIZE) return -1;
    sectors = nbytes / MC_FRAME_SIZE;
    buffer = hal_ps1_ptr(buffer);
    if (!pc_ptr_is_mapped(buffer, (unsigned long)nbytes)) return -1;
    memset(x, 0, sizeof(*x));
    x->active = 1;
    x->operation = operation;
    x->sector = block * (MC_BLOCK_SIZE / MC_FRAME_SIZE) + pos / MC_FRAME_SIZE;
    x->end = x->sector + sectors;
    x->buffer = buffer;
    PC_DEBUG_MEM("file %s block=%d sectors %d..%d tick=%llu",
                 operation == 4 ? "write" : "read", block, x->sector,
                 x->end - 1, hal_clock_ticks());
    file_transfer_submit(operation, x->sector, buffer);
    if (wait) card_spin_while(&x->active);
    return nbytes;
}

void memcard_complete_event(void)
{
    CardRequest *request;
    int slot, operation, stage, software_request, file_transfer;
    u32 event;
    slot = g_card_active_slot;
    if (slot < 0 || hal_clock_ticks() < g_card_event) return;
    request = &g_requests[slot];
    operation = request->operation;
    stage = request->directory_stage;
    software_request = request->software_request;
    request->software_request = 0;
    file_transfer = request->file_transfer;
    request->file_transfer = 0;
    g_card_active_slot = -1;
    g_card_event = CARD_NO_EVENT;
    request->operation = 0;
    event = slot == 1 ? CARD_TIMEOUT : CARD_IOE;
    if (slot == 0 && g_card_unconfirmed && !request->suppress_new)
        event = CARD_NEW;
    if (event == CARD_IOE) {
        if (operation == 2)
            memcpy(request->buffer, dir_frame(request->sector), MC_FRAME_SIZE);
        else if (operation == 4) {
            memcpy(dir_frame(request->sector), request->write_data, MC_FRAME_SIZE);
            g_card_dirty = 1;
            g_card_unconfirmed = 0;
            card_flush();
        }
    }
    request->status = event == CARD_TIMEOUT ? 0x11 : 1;
    request->result = event == CARD_IOE;
    PC_DEBUG_MEM("complete slot=%d op=%d sector=%d event=%x tick=%llu",
                 slot, operation, request->sector, event, hal_clock_ticks());
    hal_bios_deliver_event(CARD_HW_EVENT, event);
    if (stage >= 0) {
        if (event == CARD_IOE && operation == 2) {
            if (xor_checksum(request->transfer, MC_FRAME_SIZE) != 0 ||
                (stage == 0 && (request->transfer[0] != 'M' ||
                                request->transfer[1] != 'C'))) {
                event = CARD_ERROR;
                request->status = 0x21;
                request->result = 0;
            } else {
                memcpy(request->directory[request->sector], request->transfer,
                       MC_FRAME_SIZE);
            }
        }
        if (event == CARD_IOE && stage < 36) {
            request->directory_stage++;
            /* The backup-frame write (stage 1) acknowledges a new card's
             * FLAG; a card already written since power-on is read
             * straight through.  Measured on the reference emulator: at boot
             * _card_load reads sector 0, writes 63 (a 137-interrupt,
             * 199,000-tick burst) and reads on; the title's _card_load
             * (13/01/01, card written at boot) reads sector 0 and 1..35,
             * 36 slot-0 turns, no write -- the port did 37 and took three
             * frames longer.  CARD_LOAD_BACKUP_ALWAYS=1 restores the old
             * unconditional write (negative control). */
            if (request->directory_stage == 1 && !g_card_unconfirmed &&
                !card_backup_always())
                request->directory_stage = 2;
            card_directory_next(slot);
            return;
        }
        request->directory_stage = -1;
        hal_bios_deliver_event(CARD_SW_EVENT, event);
        card_flush();
    } else if (file_transfer) {
        file_transfer_next(event);
    } else if (software_request) {
        hal_bios_deliver_event(CARD_SW_EVENT, event);
    }
}

long memcard_card_load(long chan)
{
    int slot;
    slot = card_slot(chan);
    if (!g_card_driver_initialized || slot < 0 ||
        g_requests[slot].operation || g_requests[slot].directory_stage >= 0)
        return 0;
    g_requests[slot].directory_stage = 0;
    card_directory_next(slot);
    return 1;
}

long memcard_card_info(long chan)
{
    int slot;
    slot = card_slot(chan);
    if (!g_card_driver_initialized || slot < 0 ||
        g_requests[slot].directory_stage >= 0) return 0;
    if (!card_submit(slot, 8, 0, NULL)) return 0;
    g_requests[slot].software_request = 1;
    return 1;
}

long memcard_card_status(long slot)
{
    static u64 last_poll = CARD_NO_EVENT;
    if (slot < 0 || slot > 1 || !g_card_driver_initialized) return 0;
    if (last_poll == hal_clock_ticks()) hal_cpu_spend(128u);
    last_poll = hal_clock_ticks();
    return g_requests[slot].status;
}

#define BU_INIT_TAIL_TICKS 82000u
static int bu_init_tail(void)
{
    return hal_model_on("BU_INIT_TAIL");
}

int memcard_bu_init(void)
{
    int slot;
    u32 mask;
    u64 t0 = hal_clock_ticks();
    /* Retail's `_bu_init` runs with the VBlank bit of I_MASK clear: the
     * reference boot delivers 76 BIOS VBlank callbacks inside it and no
     * libetc handler, and the first edge after its return runs both again.
     * The latch from those masked edges is never delivered (nothing arrives
     * between the return and that edge), so the card routine's own
     * acknowledgment discards it. */
    mask = GetIntrMask();
    SetIntrMask((s32)(mask & ~1u));
    memcard_card_start();
    for (slot = 0; slot < 2; slot++) {
        if (!memcard_card_load(slot << 4)) continue;
        while (g_requests[slot].directory_stage >= 0) {
            u64 next, now;
            u32 ticks;
            now = hal_clock_ticks();
            next = memcard_next_event();
            ticks = hal_cpu_ticks_to_vblank();
            if (next != CARD_NO_EVENT && next > now && next - now < ticks)
                ticks = (u32)(next - now);
            hal_cpu_spend(ticks ? ticks : 1u);
        }
    }
    hal_irq_ack(1u);
    SetIntrMask((s32)mask);
    /* After its last slot completes (slot 1, empty: the timeout at the
     * VBlank exception ~20,000 ticks after the edge) the BIOS polls the
     * four card events (four B-table calls, 552+24,320..27,379) and returns
     * 102,000 ticks after the edge (_card_auto's thunk at +101,997):
     * the reference emulator, SCPH-5500, the boot (-tracepc on the A0/B0
     * dispatchers).  The port returned at the completion; it now spends the
     * measured remainder.  BU_INIT_TAIL=0 is the negative control. */
    if (bu_init_tail()) hal_cpu_spend(BU_INIT_TAIL_TICKS);
    if (getenv("PC_DEBUG") && strstr(getenv("PC_DEBUG"), "card"))
        fprintf(stderr, "[memcard] _bu_init %llu..%llu\n", (unsigned long long)t0,
                (unsigned long long)hal_clock_ticks());
    return 0;
}

/* PsyQ access mode bits (sys/file.h; the BIOS B(0x32) FileOpen contract):
 *   0x0001 FREAD, 0x0002 FWRITE, 0x0004 FNBLOCK, 0x0200 FCREAT,
 *   0x8000 FASYNC; bits 16..31 are the block count FCREAT allocates.
 * The game creates with 0x10200 (card_file_make: one block), reads with
 * 0x8001 and writes with 0x8002 / 0x0002.  We serve RDWR for any WRITE
 * path since we back an in-memory image.  (Until 2026-10-30 0x0004 was
 * read as "create", so the game's 0x10200 never created a file and the
 * new-game album could not be made: "file cannot be created".) */
/* FileOpen with FCREAT is synchronous on the card: the BIOS does four
 * requests on slot 0, each at a slot-0 turn of the driver (every second
 * VBlank), and FileOpen returns after the last.  Measured on
 * the reference emulator, SCPH-5500 (2026-10-31; -tracepc on the game's
 * open/close thunks and the exception vector, -log Dev for the card's own
 * sector log), card_file_make's two creates at 13/02/4c:
 *   turn 1  read sector 0 (the header frame)  ~229,000 ticks of SIO IRQs
 *   turn 2  a request with no data             ~7,000 (op 8, the driver's
 *                                              test, as the write's close)
 *   turn 3  write the new directory frame      ~225,000 (sector 1, then 2)
 *   turn 4  a request with no data             ~5,000
 * the call returns 13,802 / 13,837 ticks after the last request's
 * interrupts end: 4,529,356 and 4,418,986 ticks from the call, 8 VBlanks
 * either way, where the port returned at once.  Each request completes
 * through memcard_complete_event (HwCARD event, no software event: the
 * BIOS waits on its own driver).  CARD_CREATE_IO=0 is the negative
 * control (the old instant create). */
#define FCREAT_RETURN_TICKS 13800u
static int card_create_io(void)
{
    static int on = -1;
    if (on < 0) {
        on = hal_model_on("CARD_CREATE_IO");
    }
    return on && g_card_driver_running;
}

static int slot0_busy(void)
{
    return g_requests[0].operation || g_requests[0].directory_stage >= 0 ||
           g_file_xfer.active;
}

/* One synchronous request on slot 0: wait for the slot, submit, wait. */
static void card_sync_request(int operation, int sector, u8 *buffer)
{
    while (slot0_busy()) {
        u64 now = hal_clock_ticks(), next = memcard_next_event();
        u32 ticks = hal_cpu_ticks_to_vblank();
        if (next != CARD_NO_EVENT && next > now && next - now < ticks)
            ticks = (u32)(next - now);
        hal_cpu_spend(ticks ? ticks : 1u);
    }
    g_card_suppress_new = 1;       /* BIOS calls _new_card before each sector */
    if (!card_submit(0, operation, sector, buffer)) return;
    card_spin_while(&g_requests[0].operation);
}

static void card_create_sync(int entry)
{
    static u8 header[MC_FRAME_SIZE];
    u8 frame[MC_FRAME_SIZE];
    memcpy(frame, dir_frame(entry), MC_FRAME_SIZE);
    PC_DEBUG_MEM("FileOpen CREATE card i/o: directory frame %d tick=%llu",
                 entry, hal_clock_ticks());
    card_sync_request(2, 0, header);
    card_sync_request(8, 0, NULL);
    card_sync_request(4, entry, frame);
    card_sync_request(8, 0, NULL);
    hal_cpu_spend(FCREAT_RETURN_TICKS);
    PC_DEBUG_MEM("FileOpen CREATE returns tick=%llu", hal_clock_ticks());
}

/* FileOpen of a name the directory does not hold (no FCREAT) reads the
 * card's header frame before failing: one slot-0 request, and the call
 * returns ~22,450 ticks after its interrupts end.  Measured on the same
 * reference: card_file_check's retry loop at 13/02/01 on a new card (177
 * failed opens): a ~203,500-tick read burst at every second VBlank and the
 * next open 22,463 / 22,495 ticks after each burst ends -- 34 VBlanks for
 * the system file's 16 retries, 18 for an album's 8, where the port took 1.
 * An open that finds its file does no card I/O (0x8002 at 13/02/4c: no
 * burst).  Same negative control as the create (CARD_CREATE_IO=0). */
#define FOPEN_MISS_RETURN_TICKS 22450u
static void card_miss_sync(void)
{
    static u8 header[MC_FRAME_SIZE];
    card_sync_request(2, 0, header);
    hal_cpu_spend(FOPEN_MISS_RETURN_TICKS);
}

static int mode_wants_create(u32 mode) { return (mode & 0x0200) != 0; }
static int mode_create_blocks(u32 mode) { return (mode >> 16) ? (int)(mode >> 16) : 1; }
static int mode_is_write   (u32 mode) { return (mode & 0x0002) != 0; }

static int alloc_fd(void) {
    for (int i = 0; i < MEMCARD_MAX_FD; i++) if (!g_fds[i].in_use) return i;
    return -1;
}

static int is_bu_path(const char *p) {
    return p && (strncmp(p, "bu00:", 5) == 0 || strncmp(p, "bu10:", 5) == 0);
}

int memcard_file_open(const char *filename, u32 accessmode) {
    if (!g_bu_initialized) memcard_init();
    if (!is_bu_path(filename)) {
        fprintf(stderr, "[memcard] FileOpen: unsupported path '%s'\n",
                filename ? filename : "(null)");
        return -1;
    }
    if (strncmp(filename, "bu10:", 5) == 0) return -1;
    const char *name = filename + 5;

    int entry = dir_find(name, 1);
    if (entry < 0) entry = dir_find(name, 0);  /* prefix fallback */

    if (entry < 0) {
        if (!mode_wants_create(accessmode)) {
            fprintf(stderr, "[memcard] FileOpen: '%s' not found\n", filename);
            if (card_create_io()) card_miss_sync();
            return -1;
        }
        if (mode_create_blocks(accessmode) != 1) {
            /* Linked multi-block files are not modelled (Tokimeki's are
             * one block); refuse rather than create a short file. */
            fprintf(stderr, "[memcard] FileOpen: '%s' asks for %d blocks (unsupported)\n",
                    filename, mode_create_blocks(accessmode));
            return -1;
        }
        entry = dir_find_free();
        if (entry < 0) {
            fprintf(stderr, "[memcard] FileOpen: card full for '%s'\n", filename);
            return -1;
        }
        de_set_single(entry, name, MC_BLOCK_SIZE);
        memset(block_data(entry), 0, MC_BLOCK_SIZE);
        g_card_dirty = 1;
        PC_DEBUG_MEM("FileOpen CREATE '%s' -> block %d", name, entry);
        if (card_create_io()) card_create_sync(entry);
    }

    int fd = alloc_fd();
    if (fd < 0) return -1;

    g_fds[fd].fp             = NULL;
    g_fds[fd].in_use         = 1;
    g_fds[fd].is_memcard     = 1;
    g_fds[fd].dir_entry      = entry;
    g_fds[fd].cur_pos        = 0;
    /* Single-block assumption (Tokimeki uses 1 block). If the game ever
     * chains blocks the allocated size will need to walk the link chain. */
    g_fds[fd].allocated_bytes = MC_BLOCK_SIZE;
    g_fds[fd].file_size      = (long)de_size(entry);
    g_fds[fd].writable       = mode_is_write(accessmode) || mode_wants_create(accessmode);
    g_fds[fd].async          = (accessmode & 0x8000) != 0;
    strncpy(g_fds[fd].path, filename, sizeof(g_fds[fd].path) - 1);

    PC_DEBUG_MEM("FileOpen '%s' -> fd=%d block=%d size=%u",
            filename, fd + MEMCARD_FD_BASE, entry, de_size(entry));
    return fd + MEMCARD_FD_BASE;
}

/* B(0x45) FileDelete("bu00:NAME"): the kernel marks the file's directory
 * frame deleted (0x51 -> 0xA1, the name stays: the "deleted variants" of
 * the header comment) and writes that frame back; 1 on success, 0 when no
 * such file.  card_file_delete (NAME_ENT's album delete, ETC) calls it.
 * Before 2026-11-01 the port's delete() was a stub returning 1, so an
 * album "deleted" on the album screen came back on the next directory
 * read (ev_albums: album_status1 01 01 on the port, 01 00 on retail).
 * The directory write goes through slot 0 like a create; its duration on
 * retail is not measured. */
int memcard_file_delete(const char *filename) {
    if (!g_bu_initialized) memcard_init();
    if (!is_bu_path(filename) || strncmp(filename, "bu10:", 5) == 0) return 0;
    const char *name = filename + 5;
    int entry = dir_find(name, 1);
    if (entry < 0) entry = dir_find(name, 0);
    if (entry < 0) return 0;
    for (int i = 0; i < MEMCARD_MAX_FD; i++)
        if (g_fds[i].in_use && g_fds[i].is_memcard && g_fds[i].dir_entry == entry)
            return 0;               /* the kernel refuses an open file */
    u8 *f = dir_frame(entry);
    f[0] = (u8)(DE_STATE_FREE_FRESH | (de_state(entry) & 0x0F));
    de_seal(entry);
    g_card_dirty = 1;
    PC_DEBUG_MEM("FileDelete '%s' -> block %d", name, entry);
    if (card_create_io()) {
        u8 frame[MC_FRAME_SIZE];
        memcpy(frame, f, MC_FRAME_SIZE);
        card_sync_request(4, entry, frame);
        card_sync_request(8, 0, NULL);
    }
    card_flush();
    return 1;
}

static MemCardFd *fd_lookup(int fd) {
    fd -= MEMCARD_FD_BASE;
    if (fd < 0 || fd >= MEMCARD_MAX_FD || !g_fds[fd].in_use) return NULL;
    return &g_fds[fd];
}

/* With the card driver running, the bytes move one sector per slot-0 turn
 * (file_transfer_start); before it runs (unit fixtures) they move at once. */
int memcard_file_read(int fd, void *buf, int nbytes) {
    MemCardFd *f = fd_lookup(fd);
    if (!f || !buf || nbytes <= 0) return -1;
    if (!f->is_memcard) return -1;
    int avail = f->allocated_bytes - f->cur_pos;
    if (avail <= 0) return 0;
    if (nbytes > avail) nbytes = avail;
    if (g_card_driver_running) {
        nbytes = file_transfer_start(2, f->dir_entry, f->cur_pos, buf, nbytes,
                                     !f->async);
        if (nbytes > 0) f->cur_pos += nbytes;
        return nbytes;
    }
    memcpy(buf, block_data(f->dir_entry) + f->cur_pos, (size_t)nbytes);
    f->cur_pos += nbytes;
    return nbytes;
}

int memcard_file_write(int fd, const void *buf, int nbytes) {
    MemCardFd *f = fd_lookup(fd);
    if (!f || !buf || nbytes <= 0) return -1;
    if (!f->is_memcard || !f->writable) return -1;
    int avail = f->allocated_bytes - f->cur_pos;
    if (avail <= 0) return 0;
    if (nbytes > avail) nbytes = avail;
    if ((u32)(f->cur_pos + nbytes) > de_size(f->dir_entry)) {
        u32 nsize = (u32)(f->cur_pos + nbytes);
        memcpy(dir_frame(f->dir_entry) + 0x04, &nsize, 4);
        de_seal(f->dir_entry);
        g_card_dirty = 1;
    }
    if (g_card_driver_running) {
        nbytes = file_transfer_start(4, f->dir_entry, f->cur_pos, (u8 *)buf,
                                     nbytes, !f->async);
        if (nbytes > 0) f->cur_pos += nbytes;
        return nbytes;
    }
    memcpy(block_data(f->dir_entry) + f->cur_pos, buf, (size_t)nbytes);
    f->cur_pos += nbytes;
    g_card_dirty = 1;
    card_flush();
    return nbytes;
}

/* Closing does not stop a transfer the descriptor started. */
int memcard_file_close(int fd) {
    MemCardFd *f = fd_lookup(fd);
    if (!f) return -1;
    if (f->is_memcard) card_flush();
    memset(f, 0, sizeof(*f));
    return 0;
}

int memcard_file_seek(int fd, int offset, int mode) {
    MemCardFd *f = fd_lookup(fd);
    if (!f) return -1;
    if (!f->is_memcard) return -1;
    int np = f->cur_pos;
    if      (mode == 0) np = offset;
    else if (mode == 1) np = f->cur_pos + offset;
    else if (mode == 2) np = f->allocated_bytes + offset;
    else return -1;
    if (np < 0 || np > f->allocated_bytes) return -1;
    f->cur_pos = np;
    return np;
}

/* pc/hal/save_auto.c: no automatic save state while the card has a request,
 * a directory pass or a file transfer in flight on either slot.
 * Observation only. */
int memcard_busy(void)
{
    return slot0_busy() || g_requests[1].operation || g_requests[1].directory_stage >= 0;
}
