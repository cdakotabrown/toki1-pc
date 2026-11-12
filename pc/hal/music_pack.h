/* SPDX-License-Identifier: GPL-3.0-or-later */
/* music_pack.h -- music packs: user-supplied files in place of the game's
 * sequenced music (pc/hal/music_pack.c; docs/pc-port/ENHANCED-MODE.md,
 * "Music packs"; docs/pc-port/PLAYER-GUIDE.md, "Music packs").
 *
 * The game plays its songs through libsnd (SsSeqOpen / SsSeqPlay / ...,
 * matched C).  This module only READS libsnd's state: the score records
 * (_ss_score, flags at +144, volume at +114/+116), the open-sequence bit
 * mask (_snd_openflag) and the voice owners (_svm_voice).  When a song with
 * a file in the selected pack starts, the SPU core leaves that sequence's
 * voices out of the output (spu_psx_music_split; the voices, the sequencer
 * and everything the game can read are unchanged) and the file is mixed in
 * instead, following the game's play / pause / replay / stop / close and
 * its volume and main-volume changes.
 *
 * Track identity: "seq-" + FNV-1a 32 of the SEQ's bytes from its 'pQES'
 * header to its end-of-track event (music_pack_seq_span), so a song has one
 * id whichever table, address or overlay plays it.
 */
#ifndef PC_MUSIC_PACK_H
#define PC_MUSIC_PACK_H

#include <stdint.h>

/* Bytes of the SEQ at `p` ('pQES' header .. FF 2F), parsing at most `max`
 * bytes; 0 when it is not a SEQ libsnd would play to its end. */
uint32_t music_pack_seq_span(const uint8_t *p, uint32_t max);
/* "seq-xxxxxxxx" for the SEQ at `p` into `out` (>= 13 bytes); 0 on success. */
int music_pack_track_id(const uint8_t *p, uint32_t max, char *out, int n);

/* Once per VBlank on the game thread, after the sequencer tick
 * (pc/hal/spu.c hal_snd_tick).  Does nothing at all unless a pack, the dump
 * or the log is configured. */
void music_pack_tick(void);
/* Under the audio lock, after the SPU register bridge forwarded the game's
 * writes (spu_mmio_sync): the core's voice mask from the voice owners. */
void music_pack_voice_sync(void);
/* Mixer side (audio lock held).  music_pack_mixing(): 1 while the core's
 * split is on (the callback then uses spu_psx_mix_split and calls
 * music_pack_mix).  music_pack_replacing(): 1 while the music voices are
 * left out of the output (the SPU bus then carries sound effects only).
 * music_pack_mix adds the replacement to `out` (stereo s16, after the
 * player's SPU gain) and writes the dump from `music` (the masked voices'
 * share from the core; may be NULL). */
int  music_pack_mixing(void);
int  music_pack_replacing(void);
void music_pack_mix(int16_t *out, const int16_t *music, int frames);

/* Finish the song dump being written (exit paths; also atexit). */
void music_pack_flush(void);

/* The settings menu's page (pc/include/pc_menu.h), registered by the
 * presenter after its built-in entries. */
void music_pack_menu_register(void);

#endif
