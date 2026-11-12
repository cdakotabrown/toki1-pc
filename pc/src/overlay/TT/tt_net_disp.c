/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tt_net_disp: widen retail pointers without widening their cells.
 * @DivergesFrom: src/overlay/TT/tt_net_disp.c -- state/table pointers and
 * the drawing cursor stay four bytes. Mesh, UVs and OT ordering are unchanged.
 */
#include "types.h"
#include "tt_draw_buffer.h"

typedef struct {
    u32 addr : 24;
    u32 len : 8;
} P_TAG;

typedef struct {
    u8 pad0[24];
    u32 tab_addr;
    u8 pad1[22];
    u16 phase;
    s16 sway;
} NET;

extern NET *tt_step_wk;
extern u8 *tt_disp_wk;
extern u8 *tt_stage_wk;
extern u8 *tt_db_buf;

void tt_net_disp()
{
    u8 *p;
    s32 ctr;
    u8 *w;
    NET *g;
    u8 *base;
    u8 *table;
    u8 *entry;
    s32 y0, y1, y2;
    s32 b;
    s32 y3, x0, v, x3;
    s16 s, x1, x2;
    s16 ax[17][17];
    s16 ay[17][17];

    w = tt_stage_wk;
    g = tt_step_wk;
    base = tt_db_buf + *(s32 *)(tt_disp_wk + 8) * sizeof(TtDrawBuffer);
    for (b = 0; b < 17; b++) {
        for (ctr = 0; ctr < 17; ctr++) {
            ax[ctr][b] = (ctr - 8) * (34816 - g->sway) / 1920;
            ay[ctr][b] = (b - 8) * (34816 - g->sway) / 1920;
            ax[ctr][b] += rcos(g->phase * 32 + b * 512) / g->sway;
            ay[ctr][b] += (s16)(rsin(g->phase * 32 + ctr * 512) / g->sway);
        }
    }
    s = 768 - *(u16 *)(w + 2);
    for (b = 0; b < 256; b += 16) {
        for (ctr = 0; ctr < 16; ctr++) {
            p = (u8 *)(unsigned long)*(u32 *)(base + 4);
            *(u32 *)(base + 4) = (u32)(unsigned long)(p + 40);
            p[7] = 45;
            p[3] = 9;
            y0 = ay[ctr][b / 16] - (s16)(s & 15);
            y0 = y0 + 128;
            y1 = ay[ctr + 1][b / 16] - (s16)(s & 15);
            y1 = y1 + 128;
            y2 = ay[ctr][b / 16 + 1] - (s16)(s & 15);
            y3 = ay[ctr + 1][b / 16 + 1] - (s16)(s & 15);
            x0 = ax[ctr][b / 16];
            x1 = ax[ctr + 1][b / 16];
            x2 = ax[ctr][b / 16 + 1];
            x3 = ax[ctr + 1][b / 16 + 1];
            *(s16 *)(p + 34) = y3 + 128;
            *(s16 *)(p + 26) = y2 + 128;
            *(s16 *)(p + 18) = y1;
            *(s16 *)(p + 10) = y0;
            *(s16 *)(p + 8) = x0;
            *(s16 *)(p + 16) = x1;
            *(s16 *)(p + 24) = x2;
            *(s16 *)(p + 32) = x3;
            table = (u8 *)(unsigned long)g->tab_addr;
            entry = (u8 *)(unsigned long)*(u32 *)(table + 4);
            entry += ((u8 *)(unsigned long)*(u32 *)table)[
                (b + ctr + (s & -16)) & 1023] * 3;
            y0 = entry[1] & 7;
            y0 = (y0 << 4) & 255;
            v = entry[1] >> 3;
            v = (v << 4) & 255;
            if (entry[0] == 1) {
                v = (v + 128) & 255;
            }
            p[29] = v + 15;
            p[37] = v + 15;
            p[20] = y0 + 15;
            p[36] = y0 + 15;
            p[12] = y0;
            p[13] = v;
            p[21] = v;
            p[28] = y0;
            *(s16 *)(p + 22) = 14;
            *(s16 *)(p + 14) = (((entry[2] >> 4) + 240) << 6) + (entry[2] & 15);
            ((P_TAG *)p)->addr = ((P_TAG *)(base + 0x8000 + 60))->addr;
            ((P_TAG *)(base + 0x8000 + 60))->addr = (u32)(unsigned long)p;
        }
    }
}
