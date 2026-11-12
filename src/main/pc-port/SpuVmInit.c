/*
 * SpuVmInit -- SPU voice-manager init (PC override of
 * src/main/matched/SpuVmInit.c).
 *
 * Statement for statement the matched function; the ONE divergence is
 * addressing (the src/main/pc-port/mv_state.h doctrine: the pointer width
 * is a property of the host, not of the game):
 *
 *   libsnd_spu_regs_ptr is a PS1 pointer GLOBAL -- 4 bytes on the console, holding
 *   0x1F801C00 (the SPU voice register block).  The native build's
 *   `u16 *libsnd_spu_regs_ptr` loads EIGHT bytes at 0x800E2EC0 and folds in the
 *   neighbouring retail word (the 0x1000,0x100E,... ramp table at
 *   0x800E2EC4) as the pointer's high half, so the se[] stores run away to
 *   0x100E10001F801C00 and SIGSEGV (measured: SpuVmInit+0x1de, the first
 *   store through se).  Here the global is read as its PS1-width u32 and
 *   zero-extended -- the value the console's `lw` produces.
 *
 * Every other access is layout-stable on the host: VoiceEntry / VolEntry
 * carry no pointers (24 and 8 bytes either way) and the rest are scalar
 * globals in the mapped PS1 RAM window.
 */
#include "types.h"

extern s16 _svm_vcf;
extern s16 _svm_damper;
extern s16 _svm_vab_count;
extern s8  spuVmMaxVoice;
extern s16 D_8013160A;
extern s16 _svm_vg;
extern s16 libsnd_kon_hi_shadow;
extern s16 _svm_okof1;
extern s16 _svm_orev1;
extern s16 _svm_orev2;
extern s32 _svm_rattr;
extern s16 libsnd_reverb_depth_left;
extern s16 libsnd_reverb_depth;
extern s32 libsnd_reverb_type;
extern s8  _svm_auto_kof_mode;
extern s16 _svm_stereo_mono;
extern s16 kMaxPrograms;

extern u16 _svm_sreg_buf[];
extern u8  _svm_sreg_dirty[];
extern u8  _svm_vab_used[];
extern u32 libsnd_spu_regs_ptr;      /* PS1-width pointer handle (see header) */
extern u8  _ss_spu_vm_rec[];

typedef struct VoiceEntry {
    u16 f00;
    u16 f02;
    u16 f04;
    u16 f06;
    u8  pad08[2];
    s16 f0A;
    s16 f0C;
    s16 f0E;
    u16 f10;
    u8  pad12[5];
    s8  f17;
} VoiceEntry;

typedef struct VolEntry {
    s16 f0;
    s16 f2;
    s16 f4;
    s16 f6;
} VolEntry;

extern VoiceEntry _svm_voice[];
extern VolEntry libsnd_seq_vol_tbl[];

void _spu_setInTransfer();
void SpuInitMalloc();
void SpuVmKeyOffNow();
void SpuVmFlush();

void SpuVmInit(numVoices)
    s8 numVoices;
{
    u16 i;

    _spu_setInTransfer(0);
    _svm_vcf = 0;
    _svm_damper = 0;
    SpuInitMalloc(32, _ss_spu_vm_rec);

    for (i = 0; i < 192; i++) {
        _svm_sreg_buf[i] = 0;
    }
    for (i = 0; i < 24; i++) {
        _svm_sreg_dirty[i] = 0;
    }
    _svm_vab_count = 0;
    for (i = 0; i < 16; i++) {
        _svm_vab_used[i] = 0;
    }

    if ((u16)numVoices >= 24) {
        spuVmMaxVoice = 24;
    } else {
        spuVmMaxVoice = (u16)numVoices;
    }

    i = 0;
    if (spuVmMaxVoice != 0) {
        u32 p;
        u16 *se;

        p = libsnd_spu_regs_ptr;
        do {
            _svm_voice[i].f02 = 24;
            _svm_voice[i].f00 = 255;
            _svm_voice[i].f17 = 0;
            _svm_voice[i].f04 = 0;
            _svm_voice[i].f06 = 0;
            _svm_voice[i].f0A = -1;
            _svm_voice[i].f0C = 0;
            _svm_voice[i].f0E = 0;
            _svm_voice[i].f10 = 255;
            se = (u16 *)(unsigned long)(p + (u16)(i * 8) * 2);
            se[0] = 0;
            se[1] = 0;
            se[3] = 512;
            se[5] = 16384;
            se[2] = 4096;
            se[4] = 0x80ff;
            D_8013160A = i;
            SpuVmKeyOffNow(1);
            i++;
        } while (i < spuVmMaxVoice);
    }

    for (i = 0; i < 33; i++) {
        libsnd_seq_vol_tbl[i].f0 = 127;
        libsnd_seq_vol_tbl[i].f2 = 127;
        libsnd_seq_vol_tbl[i].f4 = 127;
        libsnd_seq_vol_tbl[i].f6 = 64;
    }

    _svm_vg = 0;
    libsnd_kon_hi_shadow = 0;
    _svm_okof1 = 0;
    _svm_orev1 = 0;
    _svm_orev2 = 0;
    _svm_rattr = 0;
    libsnd_reverb_depth_left = 16383;
    libsnd_reverb_depth = 16383;
    libsnd_reverb_type = 0;
    _svm_auto_kof_mode = 0;
    _svm_stereo_mono = 0;
    kMaxPrograms = 128;
    SpuVmFlush();
}
