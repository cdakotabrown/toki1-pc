/* snd_score.h -- PS1-width access to libsnd's sequence score records.
 *
 * libsnd keeps one 168-byte record per (sequence, track) in the table that
 * SsSetTableSize hands it (gSDSsTable), and `_ss_score[]` (0x80131380)
 * holds one 4-byte PS1 pointer per sequence.  The matched C reaches the
 * records through two spellings that agree only when a pointer is 4 bytes:
 * raw byte offsets (SeqPlay, SsSeqClose, Snd_SetPlayMode, ...) and typed
 * records whose read/loop pointers are `u8 *` members (InitSoundSeq,
 * GetSeqData, GetMetaEvent, SetControlChange, ReadDeltaValue,
 * SetPitchBend).  On a 64-bit host the typed members are 8 bytes wide, so
 * every field after +4 moves and the two spellings read different bytes.
 *
 * The PC overrides of those functions keep their statements and use the
 * record's PS1 layout: the three stream pointers are 32-bit handles into
 * the mapped PS1 RAM window, zero-extended on use (src/main/pc-port/
 * mv_state.h doctrine: the pointer width is a property of the host, not of
 * the game).  `_ss_score[]` is read as its PS1-width u32 table. */
#ifndef PC_SND_SCORE_H
#define PC_SND_SCORE_H
#include "types.h"
#include <stdint.h>

/* A 4-byte PS1 handle as a host pointer into the mapped RAM window.  The
 * console decodes main RAM in every segment and mirror (kuseg 0x00xxxxxx,
 * kseg0 0x80xxxxxx, kseg1 0xA0xxxxxx; 2 MiB mirrored to 8 MiB); the PC maps
 * it once at 0x80000000, so the handle is decoded the same way.  (A SEQ
 * handle of 0 -- table not loaded -- reads RAM from address 0 upward on the
 * console; see InitSoundSeq.c.) */
#define SND_PTR(h) ((u8 *)(uintptr_t)(0x80000000u | ((u32)(h) & 0x1FFFFFu)))
/* Handle of a host pointer that lies in the mapped PS1 RAM window. */
#define SND_HANDLE(p) ((u32)(uintptr_t)(p))

extern u32 _ss_score[];          /* 0x80131380: u32 PS1 pointer per seq */
#define SND_SCORE(seq) SND_PTR(_ss_score[(seq)])

#endif
