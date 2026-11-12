/* SPDX-License-Identifier: GPL-3.0-or-later */
/* soundfont.h -- SoundFont mode (enhanced): the game's sequenced music
 * rendered through TinySoundFont with a user-supplied SF2 (pc/hal/soundfont.c;
 * docs/pc-port/PLAYER-GUIDE.md "SoundFont mode").
 *
 * The game's libsnd keeps playing every song on the SPU; nothing here writes
 * PS1 memory or changes emulated time.  The note events are the SPU's own key
 * writes: at each register sync that keys voices (spu_mmio_sync) the game
 * thread takes a snapshot of the keyed voices' libsnd records (owner, note,
 * program, VAB) and resolves each music note to a SoundFont preset and key;
 * the snapshot travels with the KON write (the real-time queue carries its
 * tag) and is applied when the core applies the write -- the same sample
 * position, in the deterministic and the real-time mixer alike.  Volume, pan
 * and pitch follow the voice's SPU registers every mixed slice; a voice that
 * ends or is taken by a sound effect ends its note.  The SPU core's music
 * split leaves the music voices out of the output and the renderer's frames
 * are mixed into the split's rest mix before the main volume and reverb.
 */
#ifndef PC_SOUNDFONT_H
#define PC_SOUNDFONT_H

#include "types.h"

/* Game thread, once per VBlank after the sequencer tick (pc/hal/spu.c):
 * loads / unloads the SoundFont the settings ask for, the trace. */
void soundfont_tick(void);

/* Game thread, audio lock held (spu_mmio_sync), before KON lo/hi reach the
 * core or the queue: the keyed voices' notes.  Returns the tag the KON
 * writes carry (0: nothing to apply -- not active). */
u32  soundfont_kon_snapshot(u16 kon_lo, u16 kon_hi);
/* Audio lock held, when a KON (0x188/0x18A) or KOFF (0x18C/0x18E) write
 * reaches the core (direct, queued or flushed); `tag` from the snapshot. */
void soundfont_key_write(u32 off, u16 value, u32 tag);

/* Audio lock held, after a state restore (spu_hal_after_restore): the
 * notes of the old time line stop at once. */
void soundfont_after_restore(void);

/* Mixer side (audio lock held).  soundfont_rendering(): a SoundFont is
 * loaded and replaces the music.  soundfont_render(): the controls of the
 * sounding notes from the SPU registers, then `n` frames (4 s32 each: dry
 * L/R, wet L/R) at `gain`. */
int  soundfont_rendering(void);
void soundfont_render(s32 *frames, int n, float gain);

/* The split's share (pc/hal/music_pack.c): 1 while the music voices are
 * replaced, and the voices whose last key-on was a SoundFont note. */
int  soundfont_split_on(void);
u32  soundfont_split_mask(void);

/* The settings menu's entry (music_pack.c's MUSIC PACKS page). */
void soundfont_menu_register(int page);

/* Health / tests: counters since start. */
typedef struct {
    unsigned notes_on, notes_off, music_keys, unknown_tag, taken, ended, stale;
    unsigned long long frames;     /* frames rendered */
} SoundfontHealth;
void soundfont_health(SoundfontHealth *out);
/* Audio lock held: the SPU voices whose SoundFont note sounds (not released). */
u32  soundfont_notes_mask(void);

#endif
