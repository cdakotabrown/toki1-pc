/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PS1 image-upload queue wire layout, shared by the native adapter/tests.
 * Addresses in entries are 32-bit PS1 handles, not host pointers. Producers
 * reject the 33rd entry; declaring a one-entry tail lets modern optimizers
 * discard every upload after the first. The native renderer consumes these
 * same records, rather than substituting asset-derived upload metadata.
 */
#ifndef PC_IMAGE_UPLOAD_QUEUE_H
#define PC_IMAGE_UPLOAD_QUEUE_H

#include "types.h"
#include <stddef.h>

#define IMAGE_UPLOAD_CAPACITY 32
#define IMAGE_UPLOAD_COUNT_OFFSET 0x1429
#define IMAGE_UPLOAD_ENTRIES_OFFSET 0x142C

typedef struct {
    u32 source;
    u8 flags;
    u8 shift;
    u16 pad;
    u16 x, y, w, h;
} PcImageUploadEntry;

typedef struct {
    u8 prefix[IMAGE_UPLOAD_COUNT_OFFSET];
    u8 count;
    u8 pad[2];
    PcImageUploadEntry entries[IMAGE_UPLOAD_CAPACITY];
} PcImageUploadQueue;

typedef char pc_image_upload_entry_size[
    sizeof(PcImageUploadEntry) == 16 ? 1 : -1];
typedef char pc_image_upload_count_offset[
    offsetof(PcImageUploadQueue, count) == IMAGE_UPLOAD_COUNT_OFFSET ? 1 : -1];
typedef char pc_image_upload_entries_offset[
    offsetof(PcImageUploadQueue, entries) == IMAGE_UPLOAD_ENTRIES_OFFSET
    ? 1 : -1];

void image_upload_queue_flush(s32 mode);

#endif
