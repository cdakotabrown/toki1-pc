/* SPDX-License-Identifier: GPL-3.0-or-later */
/* music_decode.c -- streaming decoders for music-pack files (music_decode.h).
 *
 * Compiles the vendored single-file decoders (pc/3rd/audio-decoders/):
 *   stb_vorbis.c  v1.22  -- public domain (Unlicense) or MIT
 *   dr_flac.h     v0.13.3 -- public domain (Unlicense) or MIT-0
 *   dr_wav.h      v0.14.5 -- public domain (Unlicense) or MIT-0
 * (revisions and hashes in pc/3rd/audio-decoders/README.md), and wraps
 * them in one small interface: open / read stereo s16 / seek / close, with
 * the file's loop tags.  Used only by pc/hal/music_pack.c (enhanced
 * profile music packs).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define STB_VORBIS_NO_PUSHDATA_API
#include "../3rd/audio-decoders/stb_vorbis.c"
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_WCHAR
#include "../3rd/audio-decoders/dr_flac.h"
#define DR_WAV_IMPLEMENTATION
#include "../3rd/audio-decoders/dr_wav.h"

/* stb_vorbis defines these short macro names; they are not used below and
 * would collide with anything that follows. */
#undef L
#undef C
#undef R

#include "music_decode.h"

enum { KIND_OGG, KIND_FLAC, KIND_WAV };

#define SCRATCH_FRAMES 1024

struct MusicDecoder {
    int kind;
    int channels;
    stb_vorbis *ogg;
    drflac *flac;
    drwav wav;
    int16_t scratch[SCRATCH_FRAMES * 8];   /* up to 8 channels */
};

static int64_t parse_count(const char *v, int len) {
    int64_t n = 0;
    int i, any = 0;
    for (i = 0; i < len && v[i] >= '0' && v[i] <= '9'; i++) {
        n = n * 10 + (v[i] - '0');
        any = 1;
    }
    return any ? n : -1;
}

void music_decode_loop_tag(const char *c, int len, MusicFileInfo *info) {
    static const struct { const char *name; int which; } tags[] = {
        { "LOOPSTART=", 0 }, { "LOOP_START=", 0 },
        { "LOOPEND=", 1 }, { "LOOP_END=", 1 },
        { "LOOPLENGTH=", 2 }, { "LOOP_LENGTH=", 2 },
    };
    unsigned i;
    if (!c || len <= 0) return;
    for (i = 0; i < sizeof tags / sizeof tags[0]; i++) {
        int n = (int)strlen(tags[i].name);
        int64_t v;
        if (len <= n || strncasecmp(c, tags[i].name, (size_t)n)) continue;
        v = parse_count(c + n, len - n);
        if (v < 0) return;
        if (tags[i].which == 0) info->loop_start = v;
        else if (tags[i].which == 1) info->loop_end = v;
        else info->loop_length = v;
        return;
    }
}

static void flac_meta(void *user, drflac_metadata *m) {
    MusicFileInfo *info = (MusicFileInfo *)user;
    drflac_vorbis_comment_iterator it;
    const char *c;
    drflac_uint32 len;
    if (m->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) return;
    drflac_init_vorbis_comment_iterator(&it, m->data.vorbis_comment.commentCount,
                                        m->data.vorbis_comment.pComments);
    while ((c = drflac_next_vorbis_comment(&it, &len)) != NULL)
        music_decode_loop_tag(c, (int)len, info);
}

static const char *ext_of(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot ? dot + 1 : "";
}

MusicDecoder *music_decoder_open(const char *path, MusicFileInfo *info) {
    MusicDecoder *d;
    MusicFileInfo tmp;
    const char *ext = ext_of(path);
    if (!info) info = &tmp;
    memset(info, 0, sizeof *info);
    info->loop_start = info->loop_end = info->loop_length = -1;
    d = (MusicDecoder *)calloc(1, sizeof *d);
    if (!d) return NULL;
    if (!strcasecmp(ext, "ogg")) {
        int err = 0;
        stb_vorbis_info vi;
        stb_vorbis_comment vc;
        int i;
        d->kind = KIND_OGG;
        d->ogg = stb_vorbis_open_filename(path, &err, NULL);
        if (!d->ogg) goto fail;
        vi = stb_vorbis_get_info(d->ogg);
        info->rate = vi.sample_rate;
        info->channels = d->channels = vi.channels;
        info->frames = stb_vorbis_stream_length_in_samples(d->ogg);
        info->format = "ogg";
        vc = stb_vorbis_get_comment(d->ogg);
        for (i = 0; i < vc.comment_list_length; i++)
            music_decode_loop_tag(vc.comment_list[i], (int)strlen(vc.comment_list[i]), info);
    } else if (!strcasecmp(ext, "flac")) {
        d->kind = KIND_FLAC;
        d->flac = drflac_open_file_with_metadata(path, flac_meta, info, NULL);
        if (!d->flac) goto fail;
        info->rate = d->flac->sampleRate;
        info->channels = d->channels = d->flac->channels;
        info->frames = d->flac->totalPCMFrameCount;
        info->format = "flac";
    } else if (!strcasecmp(ext, "wav")) {
        drwav_uint32 i;
        d->kind = KIND_WAV;
        if (!drwav_init_file_with_metadata(&d->wav, path, 0, NULL)) goto fail;
        info->rate = d->wav.sampleRate;
        info->channels = d->channels = d->wav.channels;
        info->frames = d->wav.totalPCMFrameCount;
        info->format = "wav";
        for (i = 0; i < d->wav.metadataCount; i++) {
            const drwav_metadata *m = &d->wav.pMetadata[i];
            if (m->type == drwav_metadata_type_smpl && m->data.smpl.sampleLoopCount > 0 &&
                m->data.smpl.pLoops) {
                info->loop_start = m->data.smpl.pLoops[0].firstSampleOffset;
                info->loop_end = (int64_t)m->data.smpl.pLoops[0].lastSampleOffset + 1;
                break;
            }
        }
    } else {
        goto fail;
    }
    if (d->channels < 1 || d->channels > 8 || info->rate < 1000 || info->rate > 384000)
        goto fail;
    if (info->loop_end < 0 && info->loop_start >= 0 && info->loop_length > 0)
        info->loop_end = info->loop_start + info->loop_length;
    return d;
fail:
    music_decoder_close(d);
    return NULL;
}

int music_decoder_read(MusicDecoder *d, int16_t *out, int frames) {
    int got = 0, i;
    if (!d || frames <= 0) return 0;
    if (frames > SCRATCH_FRAMES) frames = SCRATCH_FRAMES;
    switch (d->kind) {
    case KIND_OGG:
        /* stb_vorbis mixes any channel count down to the 2 asked for */
        return stb_vorbis_get_samples_short_interleaved(d->ogg, 2, out, frames * 2);
    case KIND_FLAC:
        got = (int)drflac_read_pcm_frames_s16(d->flac, (drflac_uint64)frames, d->scratch);
        break;
    case KIND_WAV:
        got = (int)drwav_read_pcm_frames_s16(&d->wav, (drwav_uint64)frames, d->scratch);
        break;
    }
    for (i = 0; i < got; i++) {
        const int16_t *f = d->scratch + i * d->channels;
        out[i * 2] = f[0];
        out[i * 2 + 1] = d->channels > 1 ? f[1] : f[0];
    }
    return got;
}

int music_decoder_seek(MusicDecoder *d, uint64_t frame) {
    if (!d) return -1;
    switch (d->kind) {
    case KIND_OGG: return stb_vorbis_seek(d->ogg, (unsigned)frame) ? 0 : -1;
    case KIND_FLAC: return drflac_seek_to_pcm_frame(d->flac, frame) ? 0 : -1;
    case KIND_WAV: return drwav_seek_to_pcm_frame(&d->wav, frame) ? 0 : -1;
    }
    return -1;
}

void music_decoder_close(MusicDecoder *d) {
    if (!d) return;
    if (d->ogg) stb_vorbis_close(d->ogg);
    if (d->flac) drflac_close(d->flac);
    if (d->kind == KIND_WAV && d->wav.onRead) drwav_uninit(&d->wav);
    free(d);
}
