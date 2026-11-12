/*
 * SetRCnt -- PC override of src/main/matched/SetRCnt.c (0x800B5780,
 * libetc).
 *
 * Statement for statement the matched function; the divergence is the
 * read WIDTH of libapi_rcnt_regs_ptr, libetc's 4-byte cell holding the root-counter
 * register base (0x1F801100).  The matched C declares it `volatile s16 *`,
 * so LP64 reads the cell and its neighbour as one 8-byte pointer
 * (pc/tools/audit_ps1_wide_access.py WIDE-LOAD; allow-listed "unreached",
 * but the reach-instrumented runs enter it).  Here the cell is read 4
 * bytes wide and zero-extended, as the console reads it; the writes land
 * in the MMIO page main() maps (pc/src/ps1_regions.c).
 *
 * @DivergesFrom: matched SetRCnt -- cell read width only.
 */
#include "types.h"

extern u32 libapi_rcnt_regs_ptr;   /* 4-byte PS1 cell: the counters' register base */
#define RCNT_BASE ((volatile s16 *)(unsigned long)*(volatile u32 *)&libapi_rcnt_regs_ptr)

s32 SetRCnt(s32 a0, s32 a1, s32 a2)
{
    s32 idx;
    volatile s16 *p;
    s32 attr;

    idx = a0 & 0xffff;
    attr = 72;
    if (idx >= 3) {
        return 0;
    }
    p = RCNT_BASE + idx * 8;
    p[2] = 0;
    p[4] = (s16)a1;
    if ((u32)idx < 2) {
        if (a2 & 0x10) {
            attr = 73;
        }
        if (!(a2 & 1)) {
            attr |= 0x100;
        }
    } else if (idx == 2) {
        if (!(a2 & 1)) {
            attr = 584;
        }
    }
    if (a2 & 0x1000) {
        attr |= 0x10;
    }
    p = RCNT_BASE + idx * 8;
    p[2] = (s16)attr;
    return 1;
}
