/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * cdxa_decode.c -- PS1 CD-XA ADPCM streaming decoder.
 *
 * Independent C89 reimplementation of the psx-spx / Sony Green-Book spec.
 * Reference: psx-spx CDROM chapter (XA ADPCM section). Not derived from
 * DuckStation or Mednafen source.
 *
 * Algorithm summary:
 *   For each 2336-byte sector we decode 18 sound groups of 128 bytes.
 *   In 4-bit mode each group yields 8 blocks of 28 samples (= 224 samples
 *   per group per channel). In 8-bit mode: 4 blocks of 28 samples.
 *   Stereo interleaves block pairs -> L channel from block 0, R from 1, ...
 *   Each block uses two first-order IIR filter history samples plus a
 *   (filter, shift) code read from the block header byte.
 *
 * Output: linear-interpolated 44100 Hz interleaved stereo S16.
 */
#include "cdxa_decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR_SIZE        2336
#define AUDIO_OFFSET       8
#define GROUP_COUNT        18
#define GROUP_SIZE         128
#define WORDS_PER_GROUP    28

/* Coding-info byte bits (psx-spx) */
#define CODE_STEREO_MASK   0x01
#define CODE_18KHZ_MASK    0x04
#define CODE_8BIT_MASK     0x10

/* Upper bound for decoded samples per sector:
 *   4-bit stereo @ 37.8kHz: 18 groups * 8 blocks * 28 = 4032 mono samples,
 *   arranged as 2016 stereo frames. After 44.1/37.8 resample the frame
 *   count rises by ~7/6. Reserve plenty of headroom. */
#define MAX_SRC_FRAMES     4096
#define OUT_SCRATCH_FRAMES 5120

/* Standard XA ADPCM filter coefficients (scaled by 1/64).
 * Index = filter field (2 bits -> 4 entries). Psy-Q / psx-spx values. */
static const int g_flt_pos[4] = {  0, 60, 115,  98 };
static const int g_flt_neg[4] = {  0,  0, -52, -55 };

struct CdxaStream {
    FILE   *fp;
    long    file_bytes;
    long    total_sectors;
    long    cur_sector;

    /* IIR history per channel */
    int     prev1[2];
    int     prev2[2];

    /* Decoded frames from last sector (before resample) */
    s16     src_buf[MAX_SRC_FRAMES * 2]; /* L,R interleaved */
    int     src_frames;
    int     src_stereo;
    int     src_rate;   /* 37800 or 18900 */

    /* 44.1kHz resample state. We carry the last-frame-tail (one frame)
     * across sector boundaries so linear interpolation joins cleanly. */
    s16     tail_l, tail_r;
    int     have_tail;
    /* Fractional resample position inside a 1<<16 fixed-point grid. */
    u32     phase;      /* src-sample index * 65536 */
    u32     step;       /* (src_rate << 16) / 44100 */
    int     src_pos;    /* integer part of phase into src_buf */
};

/* Clip to s16 range. */
static s16 clip16(int x) {
    if (x < -32768) return -32768;
    if (x >  32767) return  32767;
    return (s16)x;
}

/* Decode one sector's audio payload at `sec` (pointing to the 8-byte
 * subheader). Writes planar-then-interleaved frames into self->src_buf.
 *
 * For 4-bit mode there are 8 blocks per group (total 18*8 = 144 blocks).
 * For 8-bit mode there are 4 blocks per group (total 72 blocks).
 * In stereo mode even blocks feed L, odd blocks feed R. So adjacent
 * block pairs produce 28 stereo frames. Non-stereo gives 28 mono samples
 * per block which we duplicate to both L/R for a uniform output path.
 */
static void decode_sector_payload(CdxaStream *s, const u8 *sec) {
    const u8 *payload = sec + AUDIO_OFFSET;
    int is_stereo = s->src_stereo;
    int is_8bit   = (sec[3] & CODE_8BIT_MASK) ? 1 : 0;
    int blocks_per_group = is_8bit ? 4 : 8;
    int frames_written = 0;
    int g, b, w;

    for (g = 0; g < GROUP_COUNT; g++) {
        const u8 *grp = payload + g * GROUP_SIZE;
        /* Block headers live in the first 16 bytes.
         * Layout (4-bit mode):  [h0 h1 h2 h3 h0 h1 h2 h3 h4 h5 h6 h7 h4 h5 h6 h7]
         * We can just read header[4 + b] to get the (non-duplicated) copy
         * for block b in [0..7]. Block b's samples live in the 28 4-byte
         * words starting at grp+16, nibble-interleaved by b. */
        const u8 *headers = grp + 4;  /* second (authoritative) copy */
        const u8 *words   = grp + 16;

        /* Per-group output slot: we lay out blocks linearly in time order
         * within the group. Pair stride: */
        for (b = 0; b < blocks_per_group; b++) {
            int hdr     = headers[b] & 0xFF;
            int shift   = hdr & 0x0F;
            int filter  = (hdr >> 4) & 0x03;
            int fpos    = g_flt_pos[filter];
            int fneg    = g_flt_neg[filter];
            int ch;        /* channel index into prev1/prev2 */
            int out_ch;   /* 0=L, 1=R */
            int src_frame_base;

            /* Determine which channel's history + output lane.
             * In stereo mode: even block -> L, odd block -> R.
             * The pair (b, b+1) produces 28 interleaved stereo frames. */
            if (is_stereo) {
                out_ch = b & 1;
                ch     = out_ch;
                /* Blocks 0&1 -> frames [0..27]; blocks 2&3 -> [28..55]; ... */
                src_frame_base = frames_written + (b / 2) * WORDS_PER_GROUP;
            } else {
                out_ch = -1;      /* write to both L and R below */
                ch     = 0;
                src_frame_base = frames_written + b * WORDS_PER_GROUP;
            }

            /* Shift > 12 is reserved per spec; treat as 9 (psx-spx). */
            if (shift > 12) shift = 9;

            for (w = 0; w < WORDS_PER_GROUP; w++) {
                /* Each audio word is 4 bytes. In 4-bit mode block b reads
                 * nibble (word >> (b*4)) & 0xF. In 8-bit mode block b
                 * reads byte (word >> (b*8)) & 0xFF. */
                u32 word_data =
                      (u32)words[w * 4 + 0]
                    | ((u32)words[w * 4 + 1] <<  8)
                    | ((u32)words[w * 4 + 2] << 16)
                    | ((u32)words[w * 4 + 3] << 24);
                int raw;
                int sample;
                int predicted;
                int dst_frame;

                if (is_8bit) {
                    raw    = (int)((word_data >> (b * 8)) & 0xFF);
                    /* Shift nibble into top of a 16-bit word and
                     * arithmetic-shift right by `shift`. */
                    sample = ((int)(s16)(raw << 8)) >> shift;
                } else {
                    raw    = (int)((word_data >> (b * 4)) & 0x0F);
                    sample = ((int)(s16)(raw << 12)) >> shift;
                }

                predicted = sample
                    + ((s->prev1[ch] * fpos) >> 6)
                    + ((s->prev2[ch] * fneg) >> 6);
                if (predicted < -32768) predicted = -32768;
                if (predicted >  32767) predicted =  32767;

                s->prev2[ch] = s->prev1[ch];
                s->prev1[ch] = predicted;

                dst_frame = src_frame_base + w;
                if (dst_frame >= MAX_SRC_FRAMES) continue;

                if (is_stereo) {
                    s->src_buf[dst_frame * 2 + out_ch] = (s16)predicted;
                } else {
                    s->src_buf[dst_frame * 2 + 0] = (s16)predicted;
                    s->src_buf[dst_frame * 2 + 1] = (s16)predicted;
                }
            }
        }

        /* Advance the frame cursor for this group.
         * Stereo: 4 pairs of blocks * 28 frames = 112 frames/group in 4-bit;
         *         2 pairs * 28 = 56 frames/group in 8-bit.
         * Mono:   8 blocks * 28 = 224 samples/group in 4-bit;
         *         4 blocks * 28 = 112 samples/group in 8-bit. */
        if (is_stereo) {
            frames_written += (blocks_per_group / 2) * WORDS_PER_GROUP;
        } else {
            frames_written += blocks_per_group * WORDS_PER_GROUP;
        }
    }

    if (frames_written > MAX_SRC_FRAMES) frames_written = MAX_SRC_FRAMES;
    s->src_frames = frames_written;
}

/* Read one valid XA audio sector into self. Skips sectors that aren't
 * audio (submode bit 2 = 0) or whose coding byte looks bogus. Returns 1
 * on success, 0 on EOF, -1 on read error. */
static int load_next_sector(CdxaStream *s) {
    u8 buf[SECTOR_SIZE];
    while (s->cur_sector < s->total_sectors) {
        size_t got;
        int submode, coding;
        if (fseek(s->fp, s->cur_sector * SECTOR_SIZE, SEEK_SET) != 0) return -1;
        got = fread(buf, 1, SECTOR_SIZE, s->fp);
        s->cur_sector++;
        if (got != SECTOR_SIZE) return 0;

        /* subheader bytes: file, channel, submode, coding at [0..3] and [4..7] */
        submode = buf[2];
        coding  = buf[3];
        /* submode bit 2 (0x04) = audio. Some authoring tools leave it
         * cleared on every sector; fall back to "coding != 0" if so. */
        if ((submode & 0x04) == 0 && coding == 0) continue;

        s->src_stereo = (coding & CODE_STEREO_MASK) ? 1 : 0;
        s->src_rate   = (coding & CODE_18KHZ_MASK)  ? 18900 : 37800;
        s->step       = (u32)(((u64)s->src_rate << 16) / 44100);

        decode_sector_payload(s, buf);
        s->src_pos = 0;
        s->phase   = 0;
        return 1;
    }
    return 0;
}

CdxaStream *cdxa_open(const char *path) {
    CdxaStream *s;
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    s = (CdxaStream *)calloc(1, sizeof(*s));
    if (!s) { fclose(fp); return NULL; }
    s->fp = fp;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); free(s); return NULL; }
    s->file_bytes = ftell(fp);
    rewind(fp);
    s->total_sectors = s->file_bytes / SECTOR_SIZE;
    s->cur_sector = 0;
    s->src_stereo = 0;
    s->src_rate = 37800;
    s->step = (u32)(((u64)37800 << 16) / 44100);
    s->have_tail = 0;
    return s;
}

void cdxa_close(CdxaStream *s) {
    if (!s) return;
    if (s->fp) fclose(s->fp);
    free(s);
}

void cdxa_rewind(CdxaStream *s) {
    if (!s) return;
    s->cur_sector = 0;
    s->src_frames = 0;
    s->src_pos = 0;
    s->phase = 0;
    s->prev1[0] = s->prev1[1] = 0;
    s->prev2[0] = s->prev2[1] = 0;
    s->have_tail = 0;
}

int cdxa_decode(CdxaStream *s, s16 *out, int max_samples) {
    int written = 0;           /* scalar samples (L+R count) */
    int max_frames = max_samples / 2;
    if (!s || !out || max_samples <= 0) return 0;

    while (written / 2 < max_frames) {
        int frames_room;
        int frame;
        /* Need source data? */
        if (s->src_pos >= s->src_frames) {
            int r = load_next_sector(s);
            if (r <= 0) break;  /* EOF or error */
        }

        /* Linear resample from src_rate -> 44100 using fixed-point phase.
         * phase advances by `step` per output frame; when phase >= 1<<16
         * we consume one source frame. To join across sector boundaries
         * we keep a one-frame tail. */
        frames_room = max_frames - written / 2;
        for (frame = 0; frame < frames_room; frame++) {
            s16 a_l, a_r, b_l, b_r;
            int ip = (int)(s->phase >> 16);
            u32 frac = s->phase & 0xFFFF;
            int src_idx;

            if (ip + s->src_pos >= s->src_frames) break;
            src_idx = s->src_pos + ip;
            a_l = s->src_buf[src_idx * 2 + 0];
            a_r = s->src_buf[src_idx * 2 + 1];
            if (src_idx + 1 < s->src_frames) {
                b_l = s->src_buf[(src_idx + 1) * 2 + 0];
                b_r = s->src_buf[(src_idx + 1) * 2 + 1];
            } else {
                b_l = a_l;  /* last-frame duplicate (avoids pop at boundary) */
                b_r = a_r;
            }

            /* out = a + (b - a) * frac / 65536 */
            {
                /* A valid s16 delta times a Q16 fraction can exceed s32. */
                int l = (int)a_l + (int)(((s64)b_l - a_l) * frac >> 16);
                int r = (int)a_r + (int)(((s64)b_r - a_r) * frac >> 16);
                out[written + 0] = clip16(l);
                out[written + 1] = clip16(r);
            }
            written += 2;

            s->phase += s->step;
            while (s->phase >= (1u << 16)) {
                s->src_pos++;
                s->phase -= (1u << 16);
            }
        }
    }

    return written;
}
