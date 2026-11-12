/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pc_backlog.h -- the enhanced profile's dialogue backlog and voice replay.
 *
 * Three pieces, all in the PC layer:
 *
 *   pc/hal/backlog_capture.c  watches the game (read only) and turns each
 *                             completed dialogue page into a BacklogPage:
 *                             the glyphs the text engine put on screen
 *                             (bitmaps copied from the game's own font
 *                             slots in VRAM, positions from its string
 *                             table, Shift-JIS codes by matching each
 *                             bitmap against the game's font in RAM) and
 *                             the XA voice stream the drive played for it.
 *                             Machine-side state: a quickload restores it
 *                             with the rest of the machine.
 *   pc/hal/backlog.c          the session's list (200 pages), the view over
 *                             the paused game, its keys, the settings-menu
 *                             entries and the test logs.  Kept across
 *                             quickloads (pc/hal/dbg_state.c KEEP_FILES).
 *   pc/hal/voice_replay.c     replays a page's voice from the disc asset
 *                             with a private copy of the controller's XA
 *                             decoder, mixed into the PC output at the
 *                             player's voice volume.  Never touches the
 *                             drive, the controller's FIFO or the SPU.
 *
 * Off under the parity profile (TOKI_BACKLOG, pc/hal/profile.c): every
 * entry point then returns at once without reading anything.
 */
#ifndef PC_BACKLOG_H
#define PC_BACKLOG_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BACKLOG_MAX_PAGES  200
#define BACKLOG_MAX_GLYPHS 192
#define BACKLOG_MAX_SEGS   52      /* the text engine's string table */
#define BACKLOG_GLYPH_PX   12      /* the game's font: 12x12, 1 bit */
#define BACKLOG_GLYPH_BYTES 18

typedef struct {
    s16 x, y;                   /* screen position, the text engine's units */
    u16 code;                   /* Shift-JIS, 0 = not found in the font */
    u16 fg, shadow;             /* RGB555 from the string's CLUT pair */
    u8  bits[BACKLOG_GLYPH_BYTES];   /* 12x12, MSB first, row major */
} BacklogGlyph;

typedef struct {
    s16 x, y;                   /* the string's origin */
    u16 first, count;           /* its glyphs in BacklogPage.g */
} BacklogSeg;

typedef struct {
    char path[40];              /* under pc/assets/raw, e.g. CDROM/XA_G/FUJISAKI.XA */
    u32 file_lba;               /* the file's first LBA */
    u32 lba_first, lba_last;    /* the sectors the drive read for the line */
    u32 req_first, req_last;    /* the line as the game asked for it (set_XAsector
                                 * -> SD_XACmd's read window), 0 = unknown */
    u8  file, channel;          /* the controller's filter */
    u16 sectors;                /* audio sectors of that channel taken */
} BacklogVoice;

typedef struct {
    u32 serial;                 /* 1, 2, ... in capture order */
    u32 frame;                  /* frame_counter when the page completed */
    u32 vblank;
    u32 screen;                 /* now_step << 16 | sub << 8 | sub_sub */
    u8  girl;                   /* now_girl */
    u8  has_voice;
    s16 speaker;                /* the name segment (index into seg), -1 none */
    BacklogVoice voice;
    u16 nseg, nglyph;
    BacklogSeg seg[BACKLOG_MAX_SEGS];
    BacklogGlyph g[BACKLOG_MAX_GLYPHS];
} BacklogPage;

/* ---- capture (backlog_capture.c) ---- */
int  backlog_enabled(void);                 /* TOKI_BACKLOG */
void backlog_capture_frame(void);           /* hal_frame_end, every game frame */
/* pc/hal/cd.c: a realtime XA audio sector read with ADPCM on */
void backlog_xa_sector(u32 lba, const u8 *raw, int filter);
/* pc/hal/cd_audio.c: the controller's decoder was reset (a read, pause, seek) */
void backlog_xa_reset(void);
/* Font lookup: the Shift-JIS code whose glyph in the game's font is
 * `bits`, 0 if none; and the reverse (1 = found). */
u16  backlog_font_code(const u8 bits[BACKLOG_GLYPH_BYTES]);
int  backlog_font_glyph(u16 code, u8 bits[BACKLOG_GLYPH_BYTES]);

/* ---- the session list and view (backlog.c) ---- */
void backlog_add(const BacklogPage *p);
void backlog_game_voice(void);              /* the game started a voice line */
int  backlog_count(void);
const BacklogPage *backlog_page(int i);     /* 0 = oldest */
void backlog_clear(void);
int  backlog_is_open(void);
void backlog_open(void);
void backlog_close(void);
void backlog_replay_last(void);             /* the replay key */
/* The view's input (keys arrive one per redraw, as in the settings menu). */
enum { BACKLOG_UP, BACKLOG_DOWN, BACKLOG_PAGE_UP, BACKLOG_PAGE_DOWN,
       BACKLOG_HOME, BACKLOG_END, BACKLOG_OK, BACKLOG_BACK };
void backlog_input(int in);
int  backlog_selected(void);                /* page index, -1 none */
/* The presenter registers its settings-menu entries once. */
void backlog_menu_register(void);

/* ---- voice replay (voice_replay.c) ---- */
/* Decode the line from the disc asset (`asset_root`/path, NULL = the
 * drive's ./pc/assets/raw) and start it; returns its 44.1 kHz frame
 * count, 0 when nothing could be read (the line is not played). */
int  voice_replay_start(const BacklogVoice *v, const char *asset_root);
void voice_replay_stop(void);
int  voice_replay_active(void);
/* Decode only (tests): up to `cap` frames into `out` (L,R interleaved). */
int  voice_replay_decode(const BacklogVoice *v, const char *asset_root, s16 *out, int cap);
/* The PC mixer (pc/hal/spu.c): add the replay to `out` at `gain`. */
void voice_replay_mix(s16 *out, int frames, float gain);

#ifdef __cplusplus
}
#endif
#endif /* PC_BACKLOG_H */
