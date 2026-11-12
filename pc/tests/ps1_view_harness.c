/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ps1_view_harness.c -- pc/tools/test_ps1_view.py.
 *
 * TAIIKU's taiiku_slot and get_taiiku_seg (pc/src/overlay_TAIIKU.c), with
 * the same statements.  Two PS1 symbols share one record array: D_A (a
 * halfword at +2, D_80149BA2) is indexed as 4-byte records whose .lo of
 * record i+1 is the high half of D_B[i] (D_80149BA4).  The loop adds to
 * D_B[i] and then reads record i+1 through the D_A view.  The linker places
 * both at fixed offsets inside one buffer (--defsym), as
 * pc/ld/psx_globals.syms places PS1 globals.
 *
 * VIEW(name) is `&name` (the old generated view, -DPLAIN_VIEW) or
 * PS1_VIEW(name) (pc/include/ps1_view.h).  The program prints how many
 * rand() rolls the loop made (records in segments 1/2/4/5 AFTER the add);
 * the console's answer is 1.
 */
#include <stdio.h>
#include "ps1_view.h"

typedef union { int w; struct { short lo; short hi; } h; } Slot;
typedef struct { short lo; short hi; } Rec;
typedef struct { int w[3]; } Rec12;
typedef struct { Rec12 r; int pad; } Frame;

extern unsigned short D_A;
extern int D_B[];
Rec12 g_delta;
unsigned short g_timer[4];
unsigned char g_flag[4];
unsigned char g_list[8] = { 255, 255, 255, 255, 255, 255, 255, 255 };
unsigned char g_ram[64] __attribute__((aligned(16)));
static int g_rolls;

#ifdef PLAIN_VIEW
#define VIEW(name) ((void *)&(name))
#else
#define VIEW(name) PS1_VIEW(name)
#endif
typedef Rec D_A__t1[];
#define D_A__v1 (*(D_A__t1 *)VIEW(D_A))
typedef Slot D_B__t1[];
#define D_B__v1 (*(D_B__t1 *)VIEW(D_B))

__attribute__((noinline)) int roll(void) { g_rolls++; return 1; }

static int seg(int i)
{
    short v = D_A__v1[i].lo;
    if (v < 1024) return 0;
    if (v < 1280) return 1;
    if (v < 1536) return 2;
    if (v < 2560) return 3;
    if (v < 2816) return 4;
    if (v < 3072) return 5;
    return 6;
}

__attribute__((noinline)) void slot(void)
{
    unsigned mark;
    Frame buf;
    Slot *acc;
    unsigned char *flag, *scan;
    int i, j, off, t;

    buf.r = g_delta;
    flag = g_flag;
    acc = D_B__v1;
    mark = 0xFFFF;
    i = 0;
    off = 0;
    do {
        if (acc->h.hi < 3584) {
            if (*flag == 0)
                acc->w = acc->w + *(int *)((unsigned char *)&buf + off);
        } else if (g_timer[i] != mark) {
            g_timer[i] = (unsigned short)mark;
            j = 0;
            if (g_list[0] != 255) {
                scan = g_list;
                do { j = j + 1; scan = scan + 1; } while (*scan != 255);
            }
            g_list[j] = (unsigned char)(i + 1);
        }
        if (seg(i + 1) == 1 || seg(i + 1) == 2 || seg(i + 1) == 4 || seg(i + 1) == 5) {
            if ((roll() & 0xFFF) == 0)
                *flag = 1;
        }
        off = off + 4;
        acc = acc + 1;
        if (*flag == 1U) {
            t = g_timer[i] + 1;
            g_timer[i] = (unsigned short)t;
            if ((unsigned)(t & 0xFFFF) >= 241) { *flag = 0; g_timer[i] = 0; }
        }
        i = i + 1;
        flag = flag + 1;
    } while (i != 3);
}

int main(void)
{
    /* taiiku_slot at game frame 25,404 of spring_events.txt */
    D_B[0] = 0x03450000; D_B[1] = 0x03A20000; D_B[2] = 0x03FF0000;
    g_delta.w[0] = 0x00010000; g_delta.w[1] = 0x00010000; g_delta.w[2] = 0x00010000;
    slot();
    printf("%d\n", g_rolls);
    return 0;
}
