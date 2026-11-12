/* SPDX-License-Identifier: GPL-3.0-or-later */
/* music_decode.h -- streaming decoders for music-pack files (pc/hal/music_decode.c).
 *
 * Ogg Vorbis (stb_vorbis), FLAC (dr_flac) and WAV (dr_wav), vendored in
 * pc/3rd/audio-decoders (public domain / MIT-0; licences in that README).
 * Output is interleaved stereo s16 at the file's own rate (mono is
 * duplicated, channels past the second are dropped); the music-pack mixer
 * resamples.  Loop points come from the file's tags: LOOPSTART with
 * LOOPEND or LOOPLENGTH (Vorbis comments in .ogg / .flac), or the first
 * loop of a WAV 'smpl' chunk.  SDL-free and game-free, so a unit harness
 * can link it alone (pc/tests/music_pack_harness.c).
 */
#ifndef PC_MUSIC_DECODE_H
#define PC_MUSIC_DECODE_H

#include <stdint.h>

typedef struct MusicDecoder MusicDecoder;

typedef struct {
    uint32_t rate;          /* frames per second */
    int channels;           /* in the file (output is always 2) */
    uint64_t frames;        /* total length in frames, 0 when unknown */
    int64_t loop_start;     /* frames; -1 = not tagged */
    int64_t loop_end;       /* frames, exclusive; -1 = not tagged */
    int64_t loop_length;    /* LOOPLENGTH as tagged, -1 = none (loop_end is derived from it) */
    const char *format;     /* "ogg" / "flac" / "wav" */
} MusicFileInfo;

/* Open `path` (format chosen by the extension, case-insensitive).
 * NULL when it cannot be opened or decoded. */
MusicDecoder *music_decoder_open(const char *path, MusicFileInfo *info);
/* Up to `frames` stereo frames into `out` (2 s16 each); returns the number
 * read, 0 at the end of the file. */
int music_decoder_read(MusicDecoder *d, int16_t *out, int frames);
/* Position at frame `frame`; 0 on success. */
int music_decoder_seek(MusicDecoder *d, uint64_t frame);
void music_decoder_close(MusicDecoder *d);

/* Parse a Vorbis-comment style "NAME=value" for the loop tags into `info`
 * (exposed for the unit test). */
void music_decode_loop_tag(const char *comment, int len, MusicFileInfo *info);

#endif
