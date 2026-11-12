/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ps1_alias_refs_harness.c -- pc/tools/test_ps1_alias_refs.py.
 *
 * Two shapes from the overlays, over symbols the linker places the way
 * pc/src/layout_aliases.ld and missing_aliases.ld place them (--defsym):
 *
 *  birth  NAME_ENT name_ent_record_set: p_birthday (a u8 member at
 *         screen_w + 248) gets the month, then the u16 at screen_w + 248 is
 *         read-modify-written for the day.  The console keeps the month.
 *  flags  TT tt_main: D_800F022D = 0; D_800F022D = tt_config_flags_b | 1;
 *         D_800F022D = tt_config_flags_u | 2 (Db_/Dc_ are typed views at the same
 *         address).  The console ends with 3.
 *
 * -DDIRECT names the symbols directly (the old build); without it the
 * symbols go through pc/include/game/ps1_alias_refs.h's route -- a function
 * returning the address (ref_* below, defined in ps1_alias_refs_addr.c,
 * another translation unit, as pc/src/ps1_alias_refs.c is).
 */
#include <stdio.h>

typedef unsigned char u8;
typedef unsigned short u16;

#ifndef DIRECT
#define screen_w (*ps1_ref_screen_w())
#define p_birthday (*ps1_ref_p_birthday())
#define D_800F022D (*ps1_ref_D_800F022D())
#define tt_config_flags_b (*ps1_ref_Db_800F022D())
#define tt_config_flags_u (*ps1_ref_Dc_800F022D())
#endif

extern u8 screen_w[];
extern u8 p_birthday;
extern u8 D_800F022D;
extern u8 tt_config_flags_b;
extern u8 tt_config_flags_u;
u8 g_month = 2, g_day = 16;      /* menu indexes: March, the 17th */
volatile int g_one = 1;

__attribute__((noinline)) void birth(void)
{
    u8 *base = screen_w;
    p_birthday = (u8)(((g_month + 1) & 0xF) | (p_birthday & 0xFFF0));
    *(volatile u16 *)(base + 248) = (u16)((((g_day + 1) << 4) & 0x1F0)
                                    | (*(volatile u16 *)(base + 248) & 0xFE0F));
}

__attribute__((noinline)) void flags(void)
{
    u8 flag;
    D_800F022D = 0;
    flag = g_one ? 1 : 0;
    D_800F022D = tt_config_flags_b | flag;
    flag = g_one ? 2 : 0;
    D_800F022D = tt_config_flags_u | flag;
}

int main(void)
{
    extern u8 g_ram[];
    g_ram[248] = 0x11;         /* month 1, day 1 */
    g_ram[249] = 0x02;
    g_ram[300] = 0x80;
    birth();
    flags();
    printf("month %d day %d flags %d\n", g_ram[248] & 0xF,
           ((g_ram[248] | (g_ram[249] << 8)) >> 4) & 0x1F, g_ram[300]);
    return 0;
}
