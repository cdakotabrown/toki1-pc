/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc/hal/file_id.h — file_id → file content resolver.
 *
 * The PSX game's cd_read_start2 takes (sector_count, dest, lba). The PC
 * port currently substitutes TITLE.NBN for any LBA-based load (see
 * src/main/pc-port/cd_read_start2.c), which is wrong for everything
 * except file 41. This module fixes that:
 *
 *   1. Build a static LBA → (path, size) table from the disc image at
 *      asset-extraction time (pc/tools/build_lba_map.py emits a C file
 *      checked into pc/src/file_lba_table.c).
 *   2. At runtime, file_id_load_by_lba() looks up the LBA, opens the
 *      corresponding extracted file, and returns its bytes.
 *
 * This way, cd_read_start2(sec_count, dest, lba) just calls
 * file_id_load_by_lba(lba, dest, sec_count * 2048) and the right per-
 * character archive (or other file content) ends up at dest.
 */
#ifndef PC_HAL_FILE_ID_H
#define PC_HAL_FILE_ID_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *path;     /* relative path inside pc/assets/raw/ */
    u32         lba;      /* CD LBA at which this file lives */
    u32         size;     /* file size in bytes */
} FileLbaEntry;

extern const FileLbaEntry g_file_lba_table[];
extern const u32          g_file_lba_table_count;

/* Resolve a file by LBA. Returns the entry pointer, or NULL if no file
 * starts at this LBA. */
const FileLbaEntry *file_id_lookup_lba(u32 lba);

/* Load up to `cap` bytes from the file at `lba` into `dest`. Returns
 * bytes copied, or 0 on miss / open failure. */
u32 file_id_load_by_lba(u32 lba, void *dest, u32 cap);

#ifdef __cplusplus
}
#endif

#endif
