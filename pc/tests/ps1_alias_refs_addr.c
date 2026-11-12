/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ps1_alias_refs_addr.c -- the addresses behind ps1_alias_refs_harness.c's
 * route (as pc/src/ps1_alias_refs.c for the port); g_ram stands for PS1 RAM
 * and the symbols are placed in it with --defsym. */
unsigned char g_ram[512] __attribute__((aligned(16)));
extern char screen_w[], p_birthday[], D_800F022D[], tt_config_flags_b[], tt_config_flags_u[];
void *ps1_ref_screen_w(void) { return screen_w; }
void *ps1_ref_p_birthday(void) { return p_birthday; }
void *ps1_ref_D_800F022D(void) { return D_800F022D; }
void *ps1_ref_Db_800F022D(void) { return tt_config_flags_b; }
void *ps1_ref_Dc_800F022D(void) { return tt_config_flags_u; }
