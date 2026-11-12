/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gte_test_tables.c -- host stand-ins for the retail LIBGTE tables that
 * pc/hal/gte.c reads from the PS1 RAM block in the game (rcossin_tbl at
 * 0x800E4C00, the square-root table at 0x800E45D0, rsin/rcos from the
 * matched C over rsin_tbl).  The retail tables are game data and stay out
 * of the repository; these are synthetic but deterministic, filled by
 * gte_test_tables_init() with the formulas pc/tools/gte_reference.py uses,
 * so harness results can be compared with that model exactly. */
#include <math.h>
#include "hal.h"

u32 rcossin_tbl[4096];
s16 gte_sqrt_tbl[192];

void gte_test_tables_init(void)
{
    int i;
    for (i = 0; i < 4096; i++) {
        double a = 2.0 * 3.14159265358979323846 * (double)i / 4096.0;
        s32 s = (s32)floor(4096.0 * sin(a) + 0.5);
        s32 c = (s32)floor(4096.0 * cos(a) + 0.5);
        rcossin_tbl[i] = ((u32)(u16)s) | ((u32)(u16)c << 16);
    }
    for (i = 0; i < 192; i++)
        gte_sqrt_tbl[i] = (s16)floor(4096.0 * sqrt((double)(i + 64) / 64.0)
                                     + 0.5);
}

/* Weak: an older gte.c (GTE_SOURCE negative controls) defines its own. */
__attribute__((weak)) s32 rsin(s32 a)
{
    s32 s = (s16)(rcossin_tbl[(a < 0 ? -a : a) & 0xFFF] & 0xFFFF);
    return a < 0 ? -s : s;
}

__attribute__((weak)) s32 rcos(s32 a)
{
    return (s32)rcossin_tbl[(a < 0 ? -a : a) & 0xFFF] >> 16;
}
