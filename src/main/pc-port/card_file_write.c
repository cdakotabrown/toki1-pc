/*
 * card_file_write -- PC override of src/main/matched/card_file_write.c
 * (0x80054C74).
 *
 * Statement for statement the matched function; the divergence is the
 * pointer WIDTH of two retail .data tables.  card_file_save_tbl0
 * (0x800B97B8) is a 4-byte PS1 pointer (0x800B9778) to a table of 4-byte
 * PS1 string pointers, and card_file_save_tbl2 (0x800BBDB0) is a table of
 * 4-byte PS1 pointers to the 384-byte records.  The matched C declares
 * them `u8 **` / `u8 *[]`, 8 bytes on LP64: card_file_save_tbl0 loads as
 * 0xB292CC91800B9778 (its neighbour in the high half), so an album save
 * (slot != 0) dereferenced a non-canonical pointer, and
 * card_file_save_tbl2[idx] loads two entries at once and strides 8 bytes.
 * In the scripted playthrough's system-file writes (slot 0, idx 0) the
 * 384-byte record source was 0x800BABB0800BAA30: the PC memcpy trampoline
 * found it unmapped and skipped the copy ("[BIOS] memcpy: ... is not
 * mapped on the PC; skipped", 4x per playthrough), so the file carried a
 * stale icon block (CARD_FILE icon, 0x8012B680).  Here the tables are
 * read as u32 and widened, as the console reads them.
 *
 * @DivergesFrom: matched card_file_write -- table read width only
 * (pc/tools/audit_ps1_wide_access.py).
 */
#include <stdint.h>
#include "types.h"
#include "tokimeki/card_file.h"
/* open/write/close/memcpy/... -> the BIOS trampolines (memcard HAL), as in
 * the matched TU: pc-port objects do not get the force-include */
#include "sony_bios_alias.h"

typedef struct {
    u8 b[3840];
} SaveBlk;

extern u8 card_file_write_str0[];
extern u8 card_file_write_str1[];
extern u8 card_file_write_str2[];
extern u32 card_file_save_tbl0;     /* PS1 pointer to PS1 string pointers */
extern u8 card_file_save_tbl1[];
extern u32 card_file_save_tbl2[];   /* PS1 pointers */
extern u8 album_chr[];
extern SaveBlk album_chr_rec0_sum;
extern SaveBlk card_save_blk;
extern s32 card_fd;

extern void card_w_clear(void);
extern s32 card_file_make(s32 slot);
extern void save_sys_set(s32 arg0);
extern void save_game_set(s32 arg0);
extern void save_name_set(void *buf, s32 slot);
extern u8 rec_table_trim(u8 *a, u8 *b, s32 slot);
extern void bzero(void *dst, s32 n);
extern void strncpy(void *dst, void *src, s32 n);
extern void memcpy(void *dst, void *src, s32 n);
extern void strncat(void *dst, void *src, s32 n);
extern void close(s32 fd);
extern s32 open(void *path, s32 mode);
extern s32 write(s32 fd, void *buf, s32 size);

s32 card_file_write(s32 slot)
{
    u8 buffer[28];
    s32 idx;
    volatile s32 pad[2];
    u8 hi;
    u8 lo;
    s32 retry;
    s32 result;

    retry = 0;
    card_w_clear();
    save_name_set(buffer, slot);
    close(card_fd);
    result = open(buffer, 0x8002);
    card_fd = result;

    if (result == -1) {
        do {
            retry++;
            if (card_file_make(slot) == -1) {
                return -1;
            }
            if (retry >= 16) {
                return -1;
            }
            result = open(buffer, 0x8002);
            card_fd = result;
        } while (result == -1);
        retry = 0;
    }

    strncpy(album_chr, card_file_write_str0, 2);
    (*(CARD_FILE *)album_chr).type = 19;
    (*(CARD_FILE *)album_chr).block_entry = 1;
    bzero((&(*(CARD_FILE *)album_chr).title[0]), 64);
    strncpy((&(*(CARD_FILE *)album_chr).title[0]), card_file_write_str1, 20);
    if (slot != 0) {
        strncat((&(*(CARD_FILE *)album_chr).title[0]),
                (u8 *)(uintptr_t)((u32 *)(uintptr_t)card_file_save_tbl0)[slot - 1], 6);
    } else {
        strncat((&(*(CARD_FILE *)album_chr).title[0]), card_file_write_str2, 12);
    }
    bzero((&(*(CARD_FILE *)album_chr).reserve[0]), 28);

    rec_table_trim(&hi, &lo, slot);
    idx = hi;
    if (idx >= 13) {
        idx = 0;
    }
    memcpy((&(*(CARD_FILE *)album_chr).clut[0]), &card_file_save_tbl1[idx * 32], 32);
    memcpy((&(*(CARD_FILE *)album_chr).icon[0][0]), (u8 *)(uintptr_t)card_file_save_tbl2[idx], 384);

    if (slot != 0) {
        save_game_set(0);
    } else {
        save_sys_set(0);
    }

    card_save_blk = album_chr_rec0_sum;

    result = write(card_fd, album_chr, 0x2000);
    if (result == -1) {
        do {
            retry++;
            if (retry >= 16) {
                return -1;
            }
            result = write(card_fd, album_chr, 0x2000);
        } while (result == -1);
    }

    return 1;
}
