/*
 * SpuVmFlush -- flush the SPU voice-register mirror (PC override of
 * src/main/matched/SpuVmFlush.c).
 *
 * Statement for statement the matched function; the ONE divergence is
 * addressing (src/main/pc-port/mv_state.h doctrine -- the pointer width is
 * a property of the host, not of the game):
 *
 *   libsnd_spu_regs_ptr is a PS1 pointer GLOBAL -- 4 bytes on the console, holding
 *   0x1F801C00 (the SPU voice register block).  A native `volatile u16 *`
 *   load folds the neighbouring retail word (the ramp table at 0x800E2EC4)
 *   into the pointer's high half and the vreg walks run off into unmapped
 *   space (measured: SpuVmFlush+0x80 SIGSEGV from _SsInit).  Here the
 *   global is read as its PS1-width u32 and zero-extended -- the value the
 *   console's `lw` produces.
 *
 * Every other access is layout-stable on the host: _svm_voice is reached
 * through byte offsets (24-byte records either way) and the rest are
 * scalars or u32/u16 arrays in the mapped PS1 RAM window.
 */
#include "types.h"

extern s32 _svm_envx_ptr;
extern u32 _svm_envx_hist[16];
extern s8 spuVmMaxVoice;
extern u32 libsnd_spu_regs_ptr;      /* PS1-width pointer handle (see header) */
extern u8 _svm_voice[];
extern s8 _svm_auto_kof_mode;
extern u16 _svm_vg;
extern u16 libsnd_kon_hi_shadow;
extern u16 _svm_okof1;
extern u16 _svm_okof2;
extern u16 _svm_orev1;
extern u16 _svm_orev2;
extern u8 _svm_sreg_dirty[24];
extern u16 _svm_sreg_buf[];

void SpuVmFlush(void)
{
    volatile u16 *spu;
    u32 *slot;
    s32 mask;
    s32 i;
    s32 j;
    s32 k;
    u8 *rec;
    volatile u16 *vreg;
    u32 flags;
    s32 n;
    s32 m;
    s32 cnt;

    cnt = spuVmMaxVoice;
    n = (_svm_envx_ptr + 1) & 0xF;
    slot = &_svm_envx_hist[n];
    *slot = 0;
    _svm_envx_ptr = n;

    i = 0;
    spu = (volatile u16 *)(unsigned long)libsnd_spu_regs_ptr;
    if (cnt <= 0) {
    } else {
        rec = _svm_voice;
        vreg = spu;
        do {
            *(volatile u16 *)(rec + 6) = vreg[6];
            if (*(volatile u16 *)(rec + 6) != 0) {
                cnt = spuVmMaxVoice;
                i++;
            } else {
                cnt = spuVmMaxVoice;
                *slot |= 1 << i;
                i++;
            }
            rec += 24;
            vreg += 8;
        } while (i < cnt);
    }

    if (_svm_auto_kof_mode == 0) {
        mask = -1;
        for (k = 0; k < 15; k++) {
            mask &= _svm_envx_hist[k];
        }
        j = 0;
        if (cnt > 0) {
            do {
                if (mask & (1U << j)) {
                    _svm_voice[j * 24 + 23] = 0;
                }
                j++;
            } while (j < cnt);
        }
    }

    _svm_vg &= ~_svm_okof1;
    libsnd_kon_hi_shadow &= ~_svm_okof2;

    for (m = 0; m < 24; m++) {
        flags = _svm_sreg_dirty[m];
        if (flags & 1) {
            spu[(u32)(m * 8) + 0] = _svm_sreg_buf[m * 8 + 0];
            spu[(u32)(m * 8) + 1] = _svm_sreg_buf[m * 8 + 1];
            flags = _svm_sreg_dirty[m];
        }
        if (flags & 4) {
            spu[(u32)(m * 8) + 2] = _svm_sreg_buf[m * 8 + 2];
            flags = _svm_sreg_dirty[m];
        }
        if (flags & 8) {
            spu[(u32)(m * 8) + 3] = _svm_sreg_buf[m * 8 + 3];
            flags = _svm_sreg_dirty[m];
        }
        if (flags & 0x10) {
            spu[(u32)(m * 8) + 4] = _svm_sreg_buf[m * 8 + 4];
            spu[(u32)(m * 8) + 5] = _svm_sreg_buf[m * 8 + 5];
        }
        _svm_sreg_dirty[m] = 0;
    }

    spu[0xC6] = _svm_okof1;
    spu[0xC7] = _svm_okof2;
    spu[0xC4] = _svm_vg;
    spu[0xC5] = libsnd_kon_hi_shadow;
    spu[0xCC] = _svm_orev1;
    spu[0xCD] = _svm_orev2;
    _svm_okof1 = 0;
    _svm_okof2 = 0;
    _svm_vg = 0;
    libsnd_kon_hi_shadow = 0;
}
