/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * cdxa_decode.h -- PS1 CD-ROM XA ADPCM streaming decoder.
 *
 * Decodes Sony CD-XA (Mode 2 Form 2) ADPCM sectors to interleaved stereo
 * 16-bit PCM at 44100 Hz (linear-interpolated from 37800 Hz or 18900 Hz).
 *
 * Input format assumptions:
 *   - File is a flat concatenation of 2336-byte Mode-2 sectors
 *     (no 16-byte sync/header prefix; no 288-byte EDC/ECC per 2352-byte form).
 *   - Layout per sector:
 *       bytes   0..  7 : subheader (file, channel, submode, coding)x2
 *       bytes   8..2311: 18 sound groups x 128 bytes audio
 *       bytes 2312..2335: EDC + reserved (ignored)
 *   - coding byte (byte 19 in raw 2352-sector; byte 3 here) selects:
 *       bit 0   : stereo (1) vs mono (0)
 *       bit 2   : 18.9kHz (1) vs 37.8kHz (0)
 *       bit 4   : 8-bit (1) vs 4-bit (0)
 *
 * Public domain / MIT. No emulator code copied; independent
 * reimplementation following the psx-spx specification.
 */
#ifndef CDXA_DECODE_H
#define CDXA_DECODE_H

#include "types.h"

typedef struct CdxaStream CdxaStream;

/* Open an XA file. Returns NULL on error. */
CdxaStream *cdxa_open(const char *path);

/* Release resources. Safe on NULL. */
void cdxa_close(CdxaStream *s);

/* Decode up to max_samples stereo 16-bit samples at 44100 Hz into out.
 * out is interleaved L,R,L,R,... so max_samples must be the total scalar
 * count (i.e. frames * 2). Returns the number of scalar samples written.
 * Returns 0 at EOF. */
int cdxa_decode(CdxaStream *s, s16 *out, int max_samples);

/* Seek back to the start for looping. */
void cdxa_rewind(CdxaStream *s);

#endif /* CDXA_DECODE_H */
