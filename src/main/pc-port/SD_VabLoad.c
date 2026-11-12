/*
 * SD_VabLoad -- open a VAB header and DMA its body to SPU RAM (PC override
 * of src/main/matched/SD_VabLoad.c).
 *
 * Statement for statement the matched function; the ONE divergence is the
 * calling convention of the SsVabTransBody call.  The matched TU declares
 * `SsVabTransBody(u32)`, but the callee is libsnd's two-argument
 * `SsVabTransBody(addr, vabid)` (src/main/matched/SsVabTransBody.c) and
 * retail passes the VAB id in $a1: the delay slot of the jal at
 * 0x80079E48 is `sh a1,56(sp)` after `lh a1,56(sp)` reloaded `idx`
 * (0x80079DF8), so $a1 == idx at the call.  On MIPS that register is the
 * second argument by construction; a native call through the one-argument
 * prototype leaves the host's second argument register undefined.  The id
 * is passed explicitly here.
 *
 * Retail disassembly of the call site (SLPS_000.65, 0x80079E24..0x80079E4C):
 *     lw    t9,gSDInfo        lui  t0,0x1        lui a0,%hi(gSDBodyAdrs)
 *     sllv  t1,t0,a1          addu a0,a0,v1      lui at,%hi(gSDInfo)
 *     lw    a0,%lo(gSDBodyAdrs)(a0)              or  t2,t9,t1
 *     sw    t2,%lo(gSDInfo)(at)
 *     jal   SsVabTransBody
 *     sh    a1,56(sp)                 <- a1 = idx (sign-extended halfword)
 */
#include "types.h"

extern u32 gSDHeadAdrs[];
extern u32 gSDBodyAdrs[];
extern u32 gSDBodyLAdrs[];
extern u32 gSDInfo;
extern s32 SsVabOpenHeadSticky(u32 a0, s32 a1, u32 a2);
extern s32 SsVabTransBody(u32 a0, s16 a1);
extern void SsVabTransCompleted(s32 a0);
extern void device_port_error_set(u32 code);

void SD_VabLoad(s16 a0) {
    s32 idx = (s32)a0;
    s32 ret;
    ret = SsVabOpenHeadSticky(gSDHeadAdrs[idx], idx, gSDBodyLAdrs[idx]);
    if (ret == -1) {
        device_port_error_set((((u32)idx << 8) & 0xFF00u) | 0x01010000u);
        return;
    }
    gSDInfo |= (0x10000 << idx);
    ret = SsVabTransBody(gSDBodyAdrs[idx], (s16)idx);
    if (ret == -1) {
        device_port_error_set((((u32)idx << 8) & 0xFF00u) | 0x01020000u);
        return;
    }
    SsVabTransCompleted(1);
}
