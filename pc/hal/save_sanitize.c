/* SPDX-License-Identifier: GPL-3.0-or-later */
/* save_sanitize.c -- range checks on what a memory-card save restores.
 *
 * The game trusts its own saves: save_check only compares a byte sum, and
 * the restore copies every field into PS1 RAM as it is.  A save made by
 * someone else (a shared card1.mcr or .mcs) controls every byte and can
 * recompute the sum.  On the console an out-of-range field drew garbage;
 * in this port the same field reaches native code: the player's names are
 * strcpy'd and sprintf'd into small buffers on the native stack
 * (msg_str_disp, date_name_set, get_p_name), and the club, album and date
 * fields index tables of PS1 pointers that are then copied from
 * (album_info_disp, name_ent_album_info_disp, holiday_club_select,
 * join_club_message, date_machiawase_name_set).
 *
 * The linker wraps the two restore functions (pc/Makefile WRAP_SYMS,
 * -Wl,--wrap): the matched function runs unchanged, then each field is
 * brought into the range the game's own writers keep it in.  A save the
 * game wrote is never changed:
 *   p_sei, p_mei, p_adana  NUL inside the field (name entry writes at most
 *                          6/6/10 bytes, the English entry 7/7/15, into
 *                          zeroed 8/8/16-byte fields: NAME_ENT
 *                          name_ent_record_set, patch pt_name_commit)
 *   basyo_pack club        bits 12..15 at most 11 (CLUBS, 11 = none)
 *   reserved_date place    bits 0..4 below 22 (the 22 date places)
 *   last_album             0..15 (save_state_set stores it & 0xF)
 *   sys_idx_tbl[16]        club numbers, at most 11
 *   album age/month/day    at most 99/12/31 (album_work +2136/+2152/+2168:
 *                          digits and month names from a 17-entry table)
 *
 * @DivergesFrom: matched save_game_restore / save_work_restore -- a range
 * clamp after the restore, a no-op for any save the game itself writes.
 */
#include <stdio.h>
#include "types.h"

extern s32 __real_save_game_restore(s32 arg0);
extern s32 __real_save_work_restore(s32 arg0);

extern u8 p_sei[], p_mei[], p_adana[];
extern u16 basyo_pack;
extern u32 reserved_date[];
extern u8 last_album;
extern u8 sys_idx_tbl[];
extern u8 album_work[];

#define CLUB_MAX 11
#define PLACE_COUNT 22
#define ALBUM_AGE 2136
#define ALBUM_MONTH 2152
#define ALBUM_DAY 2168

static int terminate(u8 *s, int n) {
    int i;
    for (i = 0; i < n; i++)
        if (!s[i]) return 0;
    s[n - 1] = 0;
    return 1;
}

static int clamp(u8 *v, u8 max) {
    if (*v <= max) return 0;
    *v = max;
    return 1;
}

static void report(const char *what, int fixed) {
    if (!fixed) return;
    printf("[SAVE] the loaded %s had %d field(s) out of the game's range; clamped "
           "(a save the game did not write)\n", what, fixed);
    fflush(stdout);
}

/* each returns the number of fields it changed */
int save_sanitize_game(void) {
    int i, fixed = 0;
    fixed += terminate(p_sei, 8);
    fixed += terminate(p_mei, 8);
    fixed += terminate(p_adana, 16);
    if ((basyo_pack >> 12) > CLUB_MAX) {
        basyo_pack = (u16)((basyo_pack & 0x0FFF) | (CLUB_MAX << 12));
        fixed++;
    }
    for (i = 0; i < 8; i++)
        if ((reserved_date[i] & 0x1F) >= PLACE_COUNT) {
            reserved_date[i] &= ~0x1Fu;
            fixed++;
        }
    return fixed;
}

int save_sanitize_work(void) {
    int i, fixed = 0;
    if (last_album > 15) { last_album &= 0x0F; fixed++; }
    for (i = 0; i < 16; i++) {
        fixed += clamp(&sys_idx_tbl[i], CLUB_MAX);
        fixed += clamp(&album_work[ALBUM_AGE + i], 99);
        fixed += clamp(&album_work[ALBUM_MONTH + i], 12);
        fixed += clamp(&album_work[ALBUM_DAY + i], 31);
    }
    return fixed;
}

s32 __wrap_save_game_restore(s32 arg0) {
    s32 r = __real_save_game_restore(arg0);
    report("album", save_sanitize_game());
    return r;
}

s32 __wrap_save_work_restore(s32 arg0) {
    s32 r = __real_save_work_restore(arg0);
    report("system file", save_sanitize_work());
    return r;
}
