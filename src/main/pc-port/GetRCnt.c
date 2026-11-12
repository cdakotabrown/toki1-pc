/*
 * GetRCnt -- PC override of src/main/matched/GetRCnt.c (0x800B581C,
 * libetc).
 *
 * Statement for statement the matched function; the divergence is the
 * read WIDTH of libapi_rcnt_regs_ptr, libetc's 4-byte cell holding the root-counter
 * register base (0x1F801100).  The matched C declares it `s16 *`,
 * so LP64 reads the cell and its neighbour as one 8-byte pointer
 * (pc/tools/audit_ps1_wide_access.py WIDE-LOAD; not entered by the
 * routes, the same cell as SetRCnt).  Here the cell is read 4
 * bytes wide and zero-extended, as the console reads it.
 *
 * @DivergesFrom: matched GetRCnt -- cell read width only.
 */
#include "types.h"

extern u32 libapi_rcnt_regs_ptr;   /* 4-byte PS1 cell: the counters' register base */
#define RCNT_BASE ((volatile s16 *)(unsigned long)*(volatile u32 *)&libapi_rcnt_regs_ptr)

u16 GetRCnt(s32 a0) {
    s32 idx;
    u16 ret;

    idx = a0 & 0xffff;
    if (idx >= 3) {
        return 0;
    }
    ret = (u16)RCNT_BASE[idx * 8];
    return ret;
}
