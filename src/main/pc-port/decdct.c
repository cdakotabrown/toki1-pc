/*
 * decdct -- background/MDEC decode driver (PC override of
 * src/main/matched/decdct.c).
 *
 * Statement for statement the matched function; the ONE divergence is
 * addressing (src/main/pc-port/mv_state.h doctrine -- the pointer width is
 * a property of the host, not of the game):
 *
 *   mdec_bs / mdec_rl / mdec_image are three ADJACENT 4-byte PS1 pointer
 *   cells (0x8012DC54/58/5C, set at runtime by dec_env_set to
 *   0x80162000/0x80162000/0x80167000).  The native 8-byte loads fold the
 *   neighbouring cell into each pointer's high half and the header check /
 *   VLC / slice copies run off into unmapped space (measured: decdct+0x24,
 *   the pData[0] header read -- the scene matrix's top crash volume after
 *   the prim-arg wraps).  Here the cells are read as their PS1-width u32
 *   and zero-extended -- the addresses the console's `lw` produces.
 *
 * The matched header follows (its PostProc history is irrelevant to the PC
 * build; the body is transcribed as written).
 */
#include "types.h"
#ifdef PC_PORT
#include "sony_bios_alias.h"   /* printf -> the BIOS A(0x3F) trampoline */
#endif

extern u32 mdec_bs;      /* PS1-width pointer handles (see header) */
extern u32 mdec_rl;
extern u32 mdec_image;
extern char decdct_str0[];

extern s32 printf(const char *format, ...);
extern void DecDCTReset(s32);
extern void DecDCToutCallback(s32);
extern void DecDCTinCallback(s32);
extern void DecDCTvlc(s32 *, s32 *);
extern void DecDCTin(s32 *, s32);
extern void DecDCTout(s32 *, s32);
extern void DecDCToutSync(s32);
extern void LoadImage(s16 *, s32 *);

s32 decdct(s32 arg0, s32 arg1, s32 arg2, s32 arg3) {
    s16 rect[4];
    s32 pad[2];
    s32 *pData;
    s32 x;
    u32 end;

    pData = (s32 *)(unsigned long)mdec_bs;
    x = arg0 & 0xFFFF;
    arg3 &= 0xFFFF;

    if ((pData[0] & 0xFFFF0000u) != 0x38000000u ||
        (pData[1] & 0xFF010000u) != 0x00010000u) {
        printf(decdct_str0);
        return -1;
    }

    DecDCTReset(0);
    DecDCToutCallback(0);
    DecDCTinCallback(0);
    DecDCTvlc((s32 *)(unsigned long)mdec_bs, (s32 *)(unsigned long)mdec_rl);
    DecDCTin((s32 *)(unsigned long)mdec_rl, 2);

    rect[1] = *(u16 *)&arg1;
    rect[2] = 16;
    x = (s16)x;
    end = (arg0 & 0xFFFF) + *(u16 *)&arg2;
    rect[0] = x;
    rect[3] = arg3;

    if ((u32)x < end) {
        x = (((u32)arg3 + 8) >> 4) << 7;
        do {
            DecDCTout((s32 *)(unsigned long)mdec_image, x);
            DecDCToutSync(0);
            LoadImage(rect, (s32 *)(unsigned long)mdec_image);
            rect[0] = rect[0] + 16;
        } while ((u32)(s16)rect[0] < end);
    }

    return 1;
}
