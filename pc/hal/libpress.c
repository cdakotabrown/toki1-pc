/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libpress.c -- Psy-Q libstr / libpress (STR streaming + MDEC) for the PC port.
 *
 * The matched movie player (movie_main / strInit / strNextVlc / strSync /
 * strCallback -- all matched C, untouched) drives the Sony streaming API:
 *
 *   strKickCD:   CdControl(CdlSeekL, loc); CdRead2(448)      start the stream
 *   strNext:     StGetNext(&data, &hdr)                        next video frame
 *   strNextVlc:  DecDCTvlc(data, vlcbuf); StFreeRing(data)    VLC-decode it
 *   movie_main:  DecDCTin(vlcbuf, mode); DecDCTout(img, n)    MDEC decode, slice out
 *   (MDEC out DMA done) -> DecDCToutCallback == strCallback    LoadImage slice,
 *                                                              DecDCTout next slice
 *   strSync:     waits for strCallback's "all slices done" flag
 *
 * On the PS1 the ring is filled by the CD-ROM at 2x and the MDEC hardware
 * decodes; here neither exists, so this file implements the SAME API over the
 * .STR file the game asked the CD for (pc/hal/cd.c remembers the file of the
 * last CdSearchFile) and the native MDEC decoder (pc/hal/mdec_native.c).
 * This is the FMV exception of the PC-port policy: the decoder is swapped,
 * the PS1 API contract and the game's control flow are not.  Concretely:
 *
 *   * StGetNext hands out the video frames of the .STR in file order, one per
 *     call, exactly as libstr would once the sectors arrived; `*hdr` is the
 *     32-byte STR sector header of the frame's first chunk (strNext checks
 *     hdr+8 frame number, hdr+16 width, hdr+20 == first word of the data),
 *     `*data` the concatenated 2016-byte chunk payloads (the BS frame).
 *   * DecDCTvlc decodes the BS straight to a 15-bit image and remembers it
 *     under the vlcbuf pointer it was asked to fill (the RLE intermediate the
 *     MDEC would consume is never materialised -- nothing but DecDCTin reads
 *     the vlcbuf, and DecDCTin only hands it on).
 *   * DecDCTout copies the next 16-pixel column slice into the caller's
 *     buffer and invokes the registered out-callback at once, which is what
 *     the MDEC DMA completion did; strCallback then asks for the next slice
 *     (recursion depth = frame width / slice width).
 *
 * CD pacing (2026-10-15): CdRead2 starts the stream in the drive model of
 * pc/hal/cd.c (Setmode + ReadS from the SeekL target, 2x) and StGetNext
 * hands a frame out only once the drive has read the frame's last sector,
 * spinning the model's clock -- and the console's VBlanks -- until it has.
 * The movie therefore runs at the disc's pace (15 fps) and vblnk_counter
 * advances as on the PS1 (~4 VBlanks per movie frame).
 *
 * DIVERGENCES (intentional, the only ones):
 *   * The XA audio interleaved in the STR is not parsed here: the drive
 *     model routes those sectors to the CD-ROM controller's decoder and the
 *     SPU's CD input as it reads them (pc/hal/cd.c, pc/hal/cd_audio.c), as
 *     the console does with Setmode bit 6.
 *   (24-bit streams: DecDCTin mode bit 0, which the boot movie uses, gets
 *   the decoder's RGB24 output; see rgb24_out_on.)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "types.h"
#include "hal.h"
#include "mdec_native.h"
#include "cpu_cost.h"
#include "profile.h"

extern const char *cd_hal_current_file(void);

#define SECTOR_RAW   2336
#define SECTOR_USER  2048
#define SUBHDR       8
#define STRHDR       32
#define PAYLOAD      (SECTOR_USER - STRHDR)   /* 2016 */

typedef struct {
    u8  hdr[STRHDR];
    u8 *data;
    u32 size;
    u32 cap;            /* chunks the data buffer holds (the first sector's count) */
    u32 w, h;
    u32 last_sector;    /* index in the .STR of the frame's last chunk: the frame is complete once the drive has read it */
} StrFrame;
static u32 g_stream_lba;       /* disc sector the stream started at (the SeekL target) */

static StrFrame *g_frames = NULL;
static int g_nframes = 0, g_next = 0, g_cap = 0;
/* per raw sector of the stream: an XA-ADPCM audio sector (submode bit 2) */
static u8 *g_sector_audio = NULL;
static u32 g_nsectors = 0;
static char g_stream_path[512];

static void (*g_out_cb)(void) = NULL;
static void (*g_in_cb)(void) = NULL;

/* decoded images, keyed by the vlcbuf pointer DecDCTvlc was given: a
 * movie frame (img, w x h) or a still bitstream in RAM (still) */
typedef struct { void *key; u16 *img; u8 *rgb; u32 w, h; MdecBsStream *still; } Decoded;

/* DecDCTin mode bit 0 asks the MDEC for 24-bit pixels, which the boot movie
 * displays with isrgb24 (movie_main).  Each movie frame is also decoded to
 * RGB24 so DecDCTout can hand those bytes on.  MDEC_RGB24_OUT=0 is the
 * negative control: the old 15-bit pixels in a 24-bit slice, which the
 * 24-bit scanout shows striped and two thirds wide. */
static int rgb24_out_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("MDEC_RGB24_OUT");
        on = !(e && e[0] == '0');
    }
    return on;
}
static Decoded g_dec[4];
static int g_dec_rr = 0;
static Decoded *g_cur = NULL;
static u32 g_col = 0;
static u16 g_stp = 0;          /* DecDCTin mode bit 1: output bit 15 (15-bit depth) */
static int g_rgb24 = 0;        /* DecDCTin mode bit 0: 24-bit output */

/* PS1 main RAM, where the game's own BS buffers live */
#define PS1_RAM_BASE 0x80000000u
#define PS1_RAM_END  0x80200000u

static void free_frames(void) {
    int i;
    for (i = 0; i < g_nframes; i++) free(g_frames[i].data);
    free(g_frames);
    g_frames = NULL; g_nframes = g_next = g_cap = 0;
    free(g_sector_audio);
    g_sector_audio = NULL; g_nsectors = 0;
}

/* Does disc sector `lba` of the running stream interrupt the CPU?  With
 * XA-ADPCM enabled in the mode (CdRead2 sets it), the drive routes the
 * stream's audio sectors -- every eighth in the boot movie -- to the SPU
 * and raises no data-ready interrupt for them: retail's libstr handler
 * (StCdInterrupt) runs for 7 sectors of every 8 (pc/hal/cd.c). */
int libpress_stream_sector_irq(u32 lba, u8 mode) {
    u32 i = lba - g_stream_lba;
    if (!(mode & 0x40) || !g_sector_audio || lba < g_stream_lba || i >= g_nsectors)
        return 1;
    return !g_sector_audio[i];
}

static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd32(const u8 *p) { return (u32)(p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24)); }

/* Parse every video sector of the .STR into whole frames (chunk payloads
 * concatenated in chunk order).  Raw 2336-byte sectors: 8-byte XA subheader,
 * 32-byte STR header (status 0x0160, type 0x8001 = video, chunk, nchunks,
 * frame, frame bytes, width, height, ...), 2016 bytes of data. */
static int load_stream(const char *path) {
    FILE *f = fopen(path, "rb");
    long len;
    u8 *buf;
    long off;
    int cur_frame = -1;
    if (!f) { fprintf(stderr, "[libpress] cannot open %s\n", path); return 0; }
    fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
    buf = malloc(len);
    if (!buf || fread(buf, 1, len, f) != (size_t)len) { fclose(f); free(buf); return 0; }
    fclose(f);
    free_frames();
    g_nsectors = (u32)(len / SECTOR_RAW);
    g_sector_audio = calloc(g_nsectors ? g_nsectors : 1, 1);
    for (off = 0; off + SECTOR_RAW <= len; off += SECTOR_RAW) {
        const u8 *h = buf + off + SUBHDR;
        if (g_sector_audio) g_sector_audio[off / SECTOR_RAW] = (buf[off + 2] & 0x04) != 0;
        u16 status = rd16(h), type = rd16(h + 2), chunk = rd16(h + 4), nchunks = rd16(h + 6);
        u32 frame = rd32(h + 8), fsize = rd32(h + 12);
        u16 w = rd16(h + 16), hh = rd16(h + 18);
        StrFrame *fr;
        if (status != 0x0160 || type != 0x8001 || nchunks == 0)
            continue;                                   /* XA audio / padding */
        if ((int)frame != cur_frame) {
            if (g_nframes == g_cap) {
                int cap = g_cap ? g_cap * 2 : 256;
                StrFrame *grown = realloc(g_frames, (size_t)cap * sizeof *g_frames);
                if (!grown) break;
                g_frames = grown;
                g_cap = cap;
            }
            fr = &g_frames[g_nframes];
            memset(fr, 0, sizeof *fr);
            memcpy(fr->hdr, h, STRHDR);
            fr->data = calloc(1, (size_t)nchunks * PAYLOAD + 16);
            if (!fr->data) break;
            g_nframes++;
            fr->cap = nchunks;
            /* the decoder reads `size` bytes: never more than the buffer */
            fr->size = fsize < (u32)nchunks * PAYLOAD ? fsize : (u32)nchunks * PAYLOAD;
            fr->w = w; fr->h = hh;
            cur_frame = (int)frame;
        }
        if (!g_nframes) continue;
        fr = &g_frames[g_nframes - 1];
        /* a later sector's own chunk count is not the buffer's: bound the
         * chunk by the count the buffer was made for */
        if ((u32)chunk < nchunks && (u32)chunk < fr->cap)
            memcpy(fr->data + (size_t)chunk * PAYLOAD, h + STRHDR, PAYLOAD);
        fr->last_sector = (u32)(off / SECTOR_RAW);
    }
    free(buf);
    strncpy(g_stream_path, path, sizeof g_stream_path - 1);
    printf("[libpress] stream %s: %d video frames\n", path, g_nframes);
    return g_nframes > 0;
}

/* ---- libcd streaming read (libstr's strKickCD) ---- */
static int is_movie(const char *path) {
    size_t n = strlen(path);
    return n > 4 && (!strcmp(path + n - 4, ".STR") || !strcmp(path + n - 4, ".str"));
}

extern s32 cd_hal_current_sector(void);
extern const char *cd_hal_file_for_sector(s32 sector, char *out, int max_len);
extern u32 cd_hal_stream_start(u8 mode);
extern int cd_hal_stream_wait_sector(u32 lba);

s32 CdRead2(s32 sectors) {
    static char path[512];
    /* the stream start is the sector strKickCD seeked to (movie_main passes
     * the disc sector: KONAMIC3.STR is LBA 400); a CdSearchFile'd file is the
     * fallback */
    const char *p = cd_hal_file_for_sector(cd_hal_current_sector(), path, sizeof path);
    (void)sectors;
    if (!p)
        p = cd_hal_current_file();
    if (!p) {
        fprintf(stderr, "[libpress] CdRead2: no file at sector %d and no CdSearchFile'd file: no stream\n",
                cd_hal_current_sector());
        free_frames();
        return 1;                 /* strKickCD polls for non-zero; StGetNext yields nothing */
    }
    /* only a movie has video frames to hand out: an .XA voice/music read
     * (the CD queue's XA command, CdRead2(200)) is audio the drive routes
     * to the SPU itself (pc/hal/cd.c, cd_audio.c) */
    if (!is_movie(p))
        free_frames();
    else if (strcmp(p, g_stream_path) != 0 || g_nframes == 0)
        load_stream(p);
    g_next = 0;
    /* the drive: Setmode(mode) + ReadS from the seek target at 2x (libstr's
     * CdRead2); StGetNext below hands a frame out only once the drive has
     * read its last sector, so the movie runs at the disc's pace (15 fps:
     * ~10 sectors a frame at 150/s) and the console's VBlanks pass meanwhile */
    g_stream_lba = cd_hal_stream_start((u8)sectors);
    return 1;
}

/* ---- libstr ring ---- */
/* libpress / libcd stream setup: their retail CPU time (cpu_cost.h
 * HAL_SDK_COST; DecDCTReset and StSetRing with the helpers they call,
 * measure_cpu_costs.py analyze-hal). */
s32 StSetRing(s32 ring, s32 nsectors) { HAL_SDK_COST("StSetRing"); (void)ring; (void)nsectors; return 0; }
void StSetMask(s32 a, s32 b, s32 c) { HAL_SDK_COST("StSetMask"); (void)a; (void)b; (void)c; }
/* Retail's StSetStream ends in a call of StSetMask (the boot's movie start:
 * StSetStream 28 ticks to it, StSetMask 96 to the caller's next call); the
 * HAL's did not, so StSetMask's time was never charged.  HAL_SDK_NESTED=0
 * is the negative control. */
static int sdk_nested_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("HAL_SDK_NESTED"); on = !(e && e[0] == '0'); }
    return on;
}
void StSetStream(s32 mode, s32 channel, s32 nframes, s32 cb_in, s32 cb_out) {
    HAL_SDK_COST("StSetStream");
    (void)mode; (void)channel; (void)nframes; (void)cb_in; (void)cb_out;
    if (sdk_nested_on()) StSetMask(0, 0, 0);
}
void StSetChannel(s32 ch) { (void)ch; }
/* StUnSetRing's CPU work: Enter/ExitCriticalSection (two BIOS syscalls)
 * around clearing libcd's data and ready callbacks.  1,441 ticks entry to
 * return on the reference emulator (Interpreter, warm; the CD queue worker's
 * six calls in a 1,500-frame no-input boot, all equal). */
#define STUNSETRING_TICKS 1441u
extern void hal_cpu_spend(u32 ticks);
void StUnSetRing(void) {
    hal_cpu_spend(STUNSETRING_TICKS);
    free_frames();
    g_stream_path[0] = 0;
}
s32 StFreeRing(s32 addr) { (void)addr; return 0; }
void StCdInterrupt(void) { }

s32 StGetNext(s32 *out_data, s32 *out_hdr) {
    if (g_next >= g_nframes)
        return 1;                                   /* nothing arrived */
    /* CD pacing: wait (spinning, as strNext does on the console) until the
     * drive has read the frame's last sector */
    if (!cd_hal_stream_wait_sector(g_stream_lba + g_frames[g_next].last_sector))
        return 1;                                   /* the drive was paused: nothing more arrives */
    if (getenv("PC_DEBUG") && strstr(getenv("PC_DEBUG"), "cd")) {
        extern s32 vblnk_counter;
        fprintf(stderr, "[libpress] frame %u (sector %u) handed out at vbl %d\n",
                rd32(g_frames[g_next].hdr + 8), g_frames[g_next].last_sector, vblnk_counter);
    }
    /* the callers' out-params are their own `u8 *` locals (strNext: &sp54,
     * &sp50) declared s32* for the PS1; store whole host pointers */
    *(u8 **)out_data = g_frames[g_next].data;
    *(u8 **)out_hdr = g_frames[g_next].hdr;
    g_next++;
    return 0;
}

/* ---- libpress MDEC ---- */
void DecDCTReset(s32 mode) { HAL_SDK_COST("DecDCTReset"); (void)mode; g_cur = NULL; g_col = 0; }
/* Retail's DecDCToutCallback installs the MDEC-out DMA callback through
 * libetc's DMACallback / setIntrDMA (two SetIntrMask): 263 ticks from its
 * entry to the caller's next call at the boot's strInit.  Charged from the
 * boot table (HAL_BOOT_COST: measured there only; BOOT_ROWS=0 off). */
void DecDCToutCallback(void (*fn)(void)) { HAL_BOOT_COST("DecDCToutCallback"); g_out_cb = fn; }
void DecDCTinCallback(void (*fn)(void)) { g_in_cb = fn; }

static StrFrame *frame_of(const void *data) {
    int i;
    for (i = g_next - 1; i >= 0 && i >= g_next - 4; i--)
        if (g_frames[i].data == data) return &g_frames[i];
    for (i = 0; i < g_nframes; i++)
        if (g_frames[i].data == data) return &g_frames[i];
    return NULL;
}

extern u64 hal_clock_ticks(void);
/* The VLC pass's CPU time.  libpress DecDCTvlc runs MDEC_vlc, the CPU's
 * Huffman decode of the frame's bitstream into MDEC run-level codes,
 * between strNextVlc's StGetNext and StFreeRing: 117,905..646,855 ticks a
 * boot-movie frame (the reference emulator, SCPH-5500, the 154 frames of
 * KONAMIC3.STR, entry to the next entry, exceptions excluded).  That time
 * decides which VBlank the movie loop's VSync(0) waits for: the port,
 * charging nothing, started each movie frame one VBlank early against the
 * stream's sectors whenever the frame's last sector came in the frame's
 * third VBlank.  A least-squares fit to the bitstream's size in bytes and
 * its run-level word count (header halfword 0), over the same 154 frames:
 * 25.59 * size + 47.82 * rl - 46,427, rms 3,056, max 15,490 (size alone
 * leaves rms 10,117).  Movie frames (a still BS: MDEC_VLC_STILL below).
 * MDEC_VLC_CPU=0 is the negative control. */
#define VLC_TICKS_PER_BYTE_X100 2559u
#define VLC_TICKS_PER_RL_X100   4782u
#define VLC_TICKS_OFFSET        46427u
extern void hal_cpu_charge(unsigned ticks) __attribute__((weak));
static void vlc_cpu(const StrFrame *fr) {
    static int on = -1;
    u64 t;
    if (on < 0) {
        on = hal_model_on("MDEC_VLC_CPU");
    }
    if (!on || !hal_cpu_charge || fr->size < 4) return;
    t = ((u64)VLC_TICKS_PER_BYTE_X100 * fr->size +
         (u64)VLC_TICKS_PER_RL_X100 * rd16(fr->data)) / 100u;
    hal_cpu_charge(t > VLC_TICKS_OFFSET ? (unsigned)(t - VLC_TICKS_OFFSET) : 0u);
}
/* MDEC_VLC_STILL: the same pass over a still BS (decdct's background
 * pictures).  Retail's VLC of the name entry's 320 x 240 background
 * (header 13,248 words; 19,329 bitstream bytes give 26,448 run-level
 * halfwords) ran 1,048,762 ticks, MDEC_vlc's entry to DecDCTin
 * (the reference emulator, new_game, game frame 3104, exception excluded):
 * the frame took four VBlanks there, one on the port, which charged
 * nothing.  The movie fit above, with the bytes the pass reads and the
 * header's word count, gives 1,081,721 (+3.1%; the only still measured,
 * and above the movie frames' range).  MDEC_VLC_STILL=0 is the negative
 * control. */
/* MDEC_VLC_STILL_FIT: the stills' own fit.  Nine retail decodes of five
 * background stills (the reference emulator, date_save phase 1 to host frame
 * 12,500, MDEC_vlc entry -> DecDCTin, exceptions excluded; 2026-10-31
 * timing round 6), with the port's bitstream extent of each:
 *   bytes  halfwords  rl     retail VLC          movie fit   this fit
 *   19329  26448      13248  1,048,762           +32,959     +10,656
 *   16931  20490      10272  911,078..911,152    -33,034     -645
 *   20031  23248      11648  1,071,176..1,071,201 -48,002    -319
 *   16045  19534       9792  859,980..860,738    -27,562     +3,511
 *   18556  24978      12512  1,027,528           -783        -13,203
 * Least squares on bytes and run-level halfwords: 45.960 * bytes +
 * 6.508 * halfwords - 1,060 (rms 7,600; the movie fit's 34,000 on
 * these).  =0: the movie fit again. */
#define STILL_VLC_PER_BYTE_X1000 45960u
#define STILL_VLC_PER_HW_X1000    6508u
#define STILL_VLC_OFFSET          1060u
static int still_fit_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("MDEC_VLC_STILL_FIT");
        on = !(e && e[0] == '0');
    }
    return on;
}
static void vlc_cpu_still(const u8 *bs, u32 size) {
    static int on = -1;
    u32 hw, bytes;
    u64 t;
    if (on < 0) {
        on = hal_model_on("MDEC_VLC_STILL");
    }
    if (!on || !hal_cpu_charge) return;
    bytes = mdec_bs_vlc_extent(bs, size, &hw);
    if (getenv("PC_DEBUG") && strstr(getenv("PC_DEBUG"), "cd"))
        fprintf(stderr, "[libpress] vlc still rl %u bytes %u halfwords %u t%llu\n",
                (unsigned)rd16(bs), bytes, hw, (unsigned long long)hal_clock_ticks());
    if (!bytes) return;
    if (still_fit_on()) {
        t = ((u64)STILL_VLC_PER_BYTE_X1000 * bytes +
             (u64)STILL_VLC_PER_HW_X1000 * hw) / 1000u;
        hal_cpu_charge(t > STILL_VLC_OFFSET ? (unsigned)(t - STILL_VLC_OFFSET) : 0u);
        return;
    }
    t = ((u64)VLC_TICKS_PER_BYTE_X100 * bytes +
         (u64)VLC_TICKS_PER_RL_X100 * rd16(bs)) / 100u;
    hal_cpu_charge(t > VLC_TICKS_OFFSET ? (unsigned)(t - VLC_TICKS_OFFSET) : 0u);
}
/* pc/hal/extras_gallery.c: the gallery learns which BS file a still is
 * (read only); weak: the libpress harness links this file alone */
extern void extras_gallery_still(const void *bs) __attribute__((weak));

void DecDCTvlc(void *bs, void *vlcbuf) {
    StrFrame *fr = frame_of(bs);
    Decoded *d;
    int i;
    for (i = 0; i < 4; i++) if (g_dec[i].key == vlcbuf) break;
    if (i == 4) { i = g_dec_rr; g_dec_rr = (g_dec_rr + 1) & 3; }
    d = &g_dec[i];
    d->key = vlcbuf;
    mdec_bs_stream_close(d->still);
    d->still = NULL;
    if (!fr) {
        /* Not a movie frame: a still BS the game decodes itself (decdct:
         * the background pictures, read from the CD into RAM).  The MDEC
         * output order is the bitstream's macroblock order; DecDCTout
         * slices it.  The stream copies the bitstream now -- decdct
         * decodes into the buffer that holds it, as the console may once
         * the VLC pass has consumed it.  Header halfword 0 is the size of
         * the MDEC run-level data in words; no VLC code is longer than
         * twice its run-level halfword, so 8 + 8 * words bytes bound the
         * bitstream (clamped to the end of main RAM). */
        const u8 *p = (const u8 *)hal_ps1_ptr(bs);
        uintptr_t a = (uintptr_t)p;
        size_t size = 0;
        if (p && pc_ptr_is_mapped(p, 8)) {
            size = 8 + 8 * (size_t)rd16(p);
            if (a >= PS1_RAM_BASE && a < PS1_RAM_END && size > PS1_RAM_END - a)
                size = PS1_RAM_END - a;
            if (pc_ptr_is_mapped(p, size))
                d->still = mdec_bs_stream_open(p, (u32)size);
        }
        if (!d->still)
            fprintf(stderr, "[libpress] DecDCTvlc: not a movie frame or BS still: %p\n", bs);
        else
            vlc_cpu_still(p, (u32)size);
        if (d->still && extras_gallery_still) extras_gallery_still(p);
        return;
    }
    if (!d->img || d->w != fr->w || d->h != fr->h) {
        free(d->img);
        free(d->rgb);
        d->img = calloc((size_t)fr->w * fr->h, sizeof(u16));
        d->rgb = rgb24_out_on() ? calloc((size_t)fr->w * fr->h, 3) : NULL;
        d->w = fr->w; d->h = fr->h;
    }
    if (getenv("PC_DEBUG") && strstr(getenv("PC_DEBUG"), "cd"))
        fprintf(stderr, "[libpress] vlc frame %u size %u rl %u q %u t%llu\n", rd32(fr->hdr + 8),
                fr->size, (unsigned)rd16(fr->data), (unsigned)rd16(fr->data + 4),
                (unsigned long long)hal_clock_ticks());
    if (mdec_decode_bs_native(fr->data, fr->size, fr->w, fr->h, d->img, 0) != 0)
        fprintf(stderr, "[libpress] DecDCTvlc: decode failed (frame %u)\n", rd32(fr->hdr + 8));
    if (d->rgb &&
        mdec_decode_bs_native_rgb24(fr->data, fr->size, fr->w, fr->h, d->rgb) != 0)
        fprintf(stderr, "[libpress] DecDCTvlc: 24-bit decode failed (frame %u)\n", rd32(fr->hdr + 8));
    vlc_cpu(fr);
}

void DecDCTin(void *vlcbuf, s32 mode) {
    int i;
    /* matched DecDCTin: bit 0 selects 24-bit output, bit 1 sets the MDEC
     * command's bit-15 flag, which a 15-bit decode writes into every pixel */
    g_stp = (!(mode & 1) && (mode & 2)) ? 0x8000 : 0;
    g_rgb24 = mode & 1;
    g_cur = NULL;
    for (i = 0; i < 4; i++) if (g_dec[i].key == vlcbuf) g_cur = &g_dec[i];
    g_col = 0;
    if (g_in_cb) g_in_cb();
}

/* The SDK's sync waits (matched MDEC_in_sync / MDEC_out_sync) read the DMA
 * channel control registers through the PS1 pointer cells 0x800D0990 and
 * 0x800D099C; pc/Makefile points those two objects at these native copies
 * of the cells' retail values.  They address the mapped, zero-filled MMIO
 * page (pc/src/main.c): the decode above is synchronous, so CHCR bit 24
 * (busy) reads clear and the waits return at once.  Charging the time
 * retail spends waiting for the out-DMA (libpress_mdec_out_busy below)
 * is not modelled. */
u32 *volatile hal_mdec_dma0_chcr = (u32 *)(unsigned long)0x1F801088u;
u32 *volatile hal_mdec_dma1_chcr = (u32 *)(unsigned long)0x1F801098u;

/* ---- MDEC out-DMA timeline (timing only) ----
 * The decode itself is instant here, but on the console MDEC-out DMA is in
 * flight for most of each movie frame, and that changes the cost of a
 * stream sector's interrupt: retail StCdInterrupt returns early when the
 * MDEC-out channel (DMA1 CHCR, 0x1F801098 bit 24) is busy and the sector is
 * processed later from strCallback (pc/hal/cd.c uses
 * libpress_mdec_out_busy to pick the shorter exception).  Measured on
 * the reference emulator (Interpreter; 147 boot-movie frames, -tracepc on
 * DecDCTout, strCallback and the exception vector): a slice's DMA
 * completes 42,048 ticks after DecDCTout (44,481 for the first slice after
 * DecDCTin), strCallback runs 2,406 after that interrupt and calls the next
 * DecDCTout 4,868 later; a completion inside the VBlank exception is
 * handled at its end, ~25,000 after the edge (median of 130).  The 20-slice
 * train is replayed from the first DecDCTout of a frame.  The slices' own
 * exceptions (8,369 each) are not modelled. */
#define MDEC_FIRST_SLICE_TICKS  44481u
#define MDEC_SLICE_TICKS        42048u
#define MDEC_IRQ_TO_CALLBACK    2406u
#define MDEC_CALLBACK_TO_OUT    4868u
#define MDEC_VBLANK_EXC_TICKS   24162u
#define MDEC_IN_VBLANK_CALLBACK 25000u
extern u64 hal_clock_ticks(void);
extern u32 hal_clock_ticks_per_frame(void);
/* Weak: harnesses link libpress.c without the clock's edge grid. */
extern u64 hal_clock_edge_at_or_before(u64 t) __attribute__((weak));
static u64 g_mdec_t0 = (u64)-1;
static u32 g_mdec_slices;

int libpress_mdec_out_busy(u64 t) {
    u64 start = g_mdec_t0, done, edge, cb;
    u32 i;
    if (g_mdec_t0 == (u64)-1 || t < start) return 0;
    for (i = 0; i < g_mdec_slices; i++) {
        done = start + (i == 0 ? MDEC_FIRST_SLICE_TICKS : MDEC_SLICE_TICKS);
        if (t < done) return 1;
        edge = hal_clock_edge_at_or_before ? hal_clock_edge_at_or_before(done)
                                           : done / hal_clock_ticks_per_frame() *
                                             hal_clock_ticks_per_frame();
        cb = done - edge < MDEC_VBLANK_EXC_TICKS ? edge + MDEC_IN_VBLANK_CALLBACK
                                                 : done + MDEC_IRQ_TO_CALLBACK;
        start = cb + MDEC_CALLBACK_TO_OUT;
        if (t < start) return 0;
    }
    return 0;
}

/* MDEC_OUT_STILL_WAIT: decdct's slice loop (DecDCTout, DecDCToutSync,
 * LoadImage per 16-pixel column of a still) waits in MDEC_out_sync for
 * the slice's out-DMA, which runs while the MDEC decodes it.  Retail's
 * name-entry background (320 x 240, 15-bit; new_game game frame 3104):
 * DecDCTout's entry to the LoadImage after it 40,954 / 41,573..41,689
 * ticks (median 41,660 over the slices not interrupted by a VBlank).
 * The port's decode is instant and MDEC_out_sync returned at once: the
 * 20 slices' ~830,000 ticks were missing.  The wait is absolute (the
 * DMA runs on while an interrupt is taken).  =0: no wait. */
#define MDEC_STILL_SLICE_TICKS 41660u
/* MDEC_OUT_STILL_BY_MB: the wait is the slice's macroblocks', not one
 * constant.  The 41,660 above is a 15-macroblock slice (240 rows); the
 * lesson / holiday backgrounds are 320 x 88 (decdct: (88 + 8) >> 4 = 6
 * macroblocks a column), and retail waits 15,846 / 15,898 there (median
 * DecDCTout entry -> LoadImage entry over the 19 / 20 uninterrupted
 * slices; the reference emulator, date_save phase 1, host frames 5514 and
 * 5710, 2026-10-31 timing round 6), against the name entry's 41,682 in the
 * same run.  Per macroblock (41,682 - 15,872) / 9 = 2,867.8 ticks: the
 * emulated MDEC's 6 x 448 a colour macroblock plus its out-DMA; the
 * intercept is -1,335 (the MDEC decodes ahead of DecDCTout by about half
 * a macroblock).  The port charged 41,660 for each 6-macroblock slice:
 * +25,800 a slice, +516,000 ticks a background (one VBlank in scene 64
 * and at each lesson / holiday start).  =0: the constant again. */
#define MDEC_STILL_MB_TICKS_X10 28719u
#define MDEC_STILL_SLICE_OFFSET 1461u
static u32 still_slice_ticks(s32 nmb) {
    static int on = -1;
    u32 t;
    if (on < 0) {
        const char *e = getenv("MDEC_OUT_STILL_BY_MB");
        on = !(e && e[0] == '0');
    }
    if (!on || nmb <= 0) return MDEC_STILL_SLICE_TICKS;
    t = (u32)(((u64)MDEC_STILL_MB_TICKS_X10 * (u32)nmb) / 10u);
    return t > MDEC_STILL_SLICE_OFFSET ? t - MDEC_STILL_SLICE_OFFSET : 0u;
}
static u64 g_still_slice_done;
extern void hal_cpu_spend(u32 ticks);
void libpress_mdec_out_wait(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("MDEC_OUT_STILL_WAIT");
        on = !(e && e[0] == '0');
    }
    if (!on || !g_still_slice_done) return;
    while (hal_clock_ticks() < g_still_slice_done) {
        u64 left = g_still_slice_done - hal_clock_ticks();
        hal_cpu_spend(left < 64u ? (u32)left : 64u);
    }
    g_still_slice_done = 0;
}

/* nwords = slice words: one 16-pixel-wide column of the frame (w*h/2 words
 * for a 16 x h slice).  Copy it, then "DMA complete" -> out callback. */
void DecDCTout(u16 *buf, s32 nwords) {
    u32 sw, y, x;
    if (g_col == 0) {           /* the frame's first slice: start the train */
        g_mdec_t0 = hal_clock_ticks();
        /* a slice's width in pixels: nwords words of 16-bit pixels, or of
         * 24-bit ones (the boot movie: DecDCTin mode 3, 2,880-word slices
         * 16 pixels wide, 20 per 320-wide frame -- the 20 slice DMA
         * completions retail takes per movie frame; MDEC_TRAIN_RGB24=0 is
         * the negative control, the old 16-bit count of 14) */
        u32 slice_w = g_cur && g_cur->h && nwords > 0 ? (u32)nwords * 2 / g_cur->h : 0;
        if (g_rgb24 && slice_w) {
            const char *e = getenv("MDEC_TRAIN_RGB24");
            if (!(e && e[0] == '0')) slice_w = slice_w * 2 / 3;
        }
        g_mdec_slices = slice_w ? (g_cur->w + slice_w - 1) / slice_w : 20;
        if (getenv("PC_DEBUG") && strstr(getenv("PC_DEBUG"), "cd"))
            fprintf(stderr, "[libpress] MDEC-out train t%llu, %u slices (w %u h %u nwords %d)\n",
                    (unsigned long long)g_mdec_t0, g_mdec_slices, g_cur ? (unsigned)g_cur->w : 0u, g_cur ? (unsigned)g_cur->h : 0u, (int)nwords);
    }
    if (getenv("PC_DEBUG") && strstr(getenv("PC_DEBUG"), "fmv")) {
        extern void *movie_state;
        unsigned long ms = (unsigned long)movie_state;
        fprintf(stderr, "[libpress] DecDCTout buf=%p nwords=%d g_cur=%p img=%p ms=%p dec8=%016lx dec10=%016lx dec18=%016lx dec20=%016lx\n",
                (void *)buf, nwords, (void *)g_cur, g_cur ? (void *)g_cur->img : NULL,
                movie_state,
                ms ? *(unsigned long *)(ms + 0x08) : 0, ms ? *(unsigned long *)(ms + 0x10) : 0,
                ms ? *(unsigned long *)(ms + 0x18) : 0, ms ? *(unsigned long *)(ms + 0x20) : 0);
        fprintf(stderr, "[libpress]   ms28=%016lx ms30=%016lx ms38=%016lx ms40=%016lx\n",
                ms ? *(unsigned long *)(ms + 0x28) : 0, ms ? *(unsigned long *)(ms + 0x30) : 0,
                ms ? *(unsigned long *)(ms + 0x38) : 0, ms ? *(unsigned long *)(ms + 0x40) : 0);
    }
    if (g_cur && g_cur->still) {
        /* nwords words = nwords / 128 macroblocks, stacked in a 16-wide
         * slice (decdct: (h + 8) >> 4 of them per 16-pixel column) */
        s32 mb, nmb = nwords / 128;
        g_still_slice_done = hal_clock_ticks() + still_slice_ticks(nmb);
        for (mb = 0; mb < nmb; mb++) {
            u16 *out = buf + (size_t)mb * 256;
            u32 k;
            if (mdec_bs_stream_macroblock(g_cur->still, out, 16) != 0)
                memset(out, 0, 256 * sizeof(u16));
            for (k = 0; k < 256; k++) out[k] |= g_stp;
        }
        g_col++;                /* one out-DMA train per decode, as for movies */
    } else if (!g_cur || !g_cur->img) {
        if (buf && nwords > 0) memset(buf, 0, (size_t)nwords * 4);
    } else if (g_rgb24 && g_cur->rgb) {
        /* a 24-bit slice: sw halfwords per row hold sw * 2 bytes, R G B
         * packed low byte first, sw * 2 / 3 pixels of the frame */
        u32 row_bytes, px0, b;
        u8 *dst = (u8 *)buf;
        sw = (g_cur->h) ? ((u32)nwords * 2) / g_cur->h : 24;
        row_bytes = sw * 2;
        px0 = g_col * (row_bytes / 3);
        for (y = 0; y < g_cur->h; y++)
            for (b = 0; b < row_bytes; b++) {
                u32 sx = px0 + b / 3;
                dst[y * row_bytes + b] = (sx < g_cur->w)
                    ? g_cur->rgb[((size_t)y * g_cur->w + sx) * 3 + b % 3] : 0;
            }
        g_col++;
    } else {
        sw = (g_cur->h) ? ((u32)nwords * 2) / g_cur->h : 16;
        if (sw == 0) sw = 16;
        for (y = 0; y < g_cur->h; y++)
            for (x = 0; x < sw; x++) {
                u32 sx = g_col * sw + x;
                buf[y * sw + x] = (u16)(((sx < g_cur->w) ? g_cur->img[y * g_cur->w + sx] : 0) | g_stp);
            }
        g_col++;
    }
    if (g_out_cb) g_out_cb();
}
