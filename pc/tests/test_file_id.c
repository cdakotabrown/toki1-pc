/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc/tests/test_file_id.c — tests for file_id LBA resolver.
 */
#include "../include/game/types.h"
#include "../hal/file_id.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int test_file_id_lba(void) {
    /* The table must be non-empty. */
    if (g_file_lba_table_count == 0) {
        printf("(empty table) ");
        return 1;
    }
    if (g_file_lba_table_count < 100) {
        printf("(table too small: %u) ", g_file_lba_table_count);
        return 2;
    }

    /* Lookup by name: TOKIMEKI.BS must be present and at a known LBA. */
    const FileLbaEntry *tokimeki = NULL;
    for (u32 i = 0; i < g_file_lba_table_count; i++) {
        if (strstr(g_file_lba_table[i].path, "TOKIMEKI.BS")) {
            tokimeki = &g_file_lba_table[i];
            break;
        }
    }
    if (!tokimeki) {
        printf("(TOKIMEKI.BS not found) ");
        return 3;
    }
    if (tokimeki->size != 18432) {
        printf("(TOKIMEKI.BS wrong size: %u, expected 18432) ", tokimeki->size);
        return 4;
    }

    /* Round-trip: lookup by LBA returns the same entry. */
    const FileLbaEntry *by_lba = file_id_lookup_lba(tokimeki->lba);
    if (by_lba != tokimeki) {
        printf("(LBA roundtrip mismatch) ");
        return 5;
    }

    /* Lookup miss returns NULL. */
    if (file_id_lookup_lba(0xDEADBEEF) != NULL) {
        printf("(unexpected miss-hit) ");
        return 6;
    }

    /* file_id_load_by_lba reads bytes correctly. */
    u8 buf[64];
    u32 got = file_id_load_by_lba(tokimeki->lba, buf, sizeof(buf));
    if (got != sizeof(buf)) {
        printf("(load got %u, expected %zu) ", got, sizeof(buf));
        return 7;
    }
    /* TOKIMEKI.BS header: bytes 2-3 = 0x3800 (BS magic). */
    if (buf[2] != 0x00 || buf[3] != 0x38) {
        printf("(bad BS magic at LBA load) ");
        return 8;
    }

    return 0;
}
