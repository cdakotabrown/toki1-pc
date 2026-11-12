/* SPDX-License-Identifier: GPL-3.0-or-later */
/* rpg_bat_msg_build -- PC override of src/overlay/RPG_BAT/rpg_bat_msg_build.c.
 *
 * Statement for statement the matched function; the divergence is the
 * RECORD WIDTH (the src/main/pc-port/basyo_init.c doctrine).  The matched
 * StrSet is `u8 *v[4]`: 16 bytes on the console, 32 on LP64.  The tables
 * it copies (rpg_bat_msg_build_tbl0/F0) are 16-byte
 * RPG_BAT .data records of four PS1 string handles (in the EXN), so the
 * host struct copy took 32 bytes: v[0] fused handles 0 and 1, v[1]
 * handles 2 and 3, and v[2]/v[3] came from the NEXT table.  The line
 * buffer table rpg_bat_msg_tbl4 is ten 4-byte handles as well.  Here StrSet is
 * four u32 handles and every handle is dereferenced as the PS1 address it
 * holds.
 *
 * @DivergesFrom: src/overlay/RPG_BAT/rpg_bat_msg_build.c -- record field widths only.
 * Test: pc/tools/test_ps1_handle_records.py.
 */
#include "types.h"

extern s32  get_rand_mod(s32);
extern void strcpy(u8 *, u8 *);
extern void strcat(u8 *, u8 *);

typedef struct {
    u32 v[4];   /* PS1 string handles */
} StrSet;

/* C89 compile-time layout check: the console's 16-byte table. */
typedef char rpg_bat_msg_build_strset_is_16[(sizeof(StrSet) == 16) ? 1 : -1];

/* the string at a PS1 handle */
#define RPG_STR(h) ((u8 *)(unsigned long)(h))

typedef struct {
    s32 v[4];
} FlagSet;

typedef struct {
    FlagSet set;
    s32     spare[3];
} FlagArea;

extern StrSet  rpg_bat_msg_build_tbl0;
extern StrSet  rpg_bat_msg_build_tbl1;
extern FlagSet rpg_bat_msg_build_tbl2;

extern u32  rpg_bat_msg_tbl4[];
extern u8   rpg_bat_msg_build_str0[];
extern u8   rpg_bat_msg_build_str1[];
extern s32  rpg_bat_seq_work[];

void rpg_bat_msg_build(void)
{
    StrSet   setA;
    StrSet   setB;
    FlagArea flags;
    s32 cnt;
    s32 i;
    s32 n;

    setA = rpg_bat_msg_build_tbl0;
    setB = rpg_bat_msg_build_tbl1;
    flags.set = rpg_bat_msg_build_tbl2;
    cnt = 0;
    if (get_rand_mod(5) == 0) {
        n = 3;
    } else {
        n = 2;
    }
    for (i = 0; i < n; i++) {
        switch ((u32)get_rand_mod(10)) {
        case 0:
        case 1:
        case 2:
        case 3:
            flags.set.v[0] = 1;
            break;
        case 4:
        case 5:
        case 6:
            flags.set.v[1] = 1;
            break;
        case 7:
        case 8:
            flags.set.v[2] = 1;
            break;
        case 9:
            flags.set.v[3] = 1;
            break;
        }
    }
    for (i = 0; i < 4; i++) {
        if (flags.set.v[i] == 1) {
            strcpy(RPG_STR(rpg_bat_msg_tbl4[cnt + 3]), rpg_bat_msg_build_str0);
            strcat(RPG_STR(rpg_bat_msg_tbl4[cnt + 3]), RPG_STR(setA.v[i]));
            cnt++;
        }
    }
    strcpy(RPG_STR(rpg_bat_msg_tbl4[cnt + 3]), rpg_bat_msg_build_str1);
    strcat(RPG_STR(rpg_bat_msg_tbl4[cnt + 3]), RPG_STR(setB.v[get_rand_mod(4)]));
    rpg_bat_seq_work[40] = cnt + 4;
}
