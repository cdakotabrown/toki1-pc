/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "types.h"
extern void option_row_pos_set(s16 *);

int test_option_rows(void)
{
    struct { s16 before[8]; s16 rows[4][8]; s16 after[8]; } buf;
    int i;
    for (i = 0; i < 8; i++) buf.before[i] = buf.after[i] = 0x1234;
    option_row_pos_set(buf.rows[0]);
    for (i = 0; i < 8; i++) {
        if (buf.before[i] != 0x1234 || buf.after[i] != 0x1234 ||
            buf.rows[0][i] != -120 || buf.rows[1][i] != -88 + i * 20 ||
            buf.rows[2][i] != 96 || buf.rows[3][i] != 20) return __LINE__;
    }
    return 0;
}
