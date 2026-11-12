/* missing_globals.c — Auto-generated stubs for unresolved globals */
#include "types.h"

/* now_icon_c / kuchi_stop_w / mes_base_y_h (mascot_sel_exit,
 * bustup_req_clear_step, basyo_init2) and the CSE-breaking views below
 * (D_800BC0C8b, D_800BCC48b..58b, _sys_default_tpage_set_tbl0_w..514_2) are the PS1 objects
 * at those addresses: PROVIDE()s in missing_aliases.ld.  As host u32s
 * here they took the stores and loads retail makes to PS1 RAM
 * (pc/tools/audit_host_globals.py). */

/* Q1 cleanup — added for SpuVmDoAllocate (libsnd voice fade-in),
 * InitSoundSeq (SEQ header parser), and _spu_setVoiceAttr. */
s16 D_8013160C;
s16 D_8013160E;
u8  D_800EBC78[1];   /* end-of-mask sentinel addr */
u8  libsnd_not_seq_data_msg[1];
u8  libsnd_old_seq_format_msg[1];
s32 spu_addrMode;


/* Previously extracted missing globals */
u32 libcd_dir_ent_lba_view; /* TODO: determine correct type and initial value */
unsigned long _SpuMemList;   /* pointer-sized: `extern u32 *_SpuMemList` in SpuFree.c (SpuInitMalloc.c's `extern s32` only ever stores the low half) */ /* TODO: determine correct type and initial value */
u32 D_800EBD61b; /* TODO: determine correct type and initial value */
u32 D_800EE760b; /* TODO: determine correct type and initial value */
u32 D_800EE79Ab; /* TODO: determine correct type and initial value */
u32 D_800EE79Ac; /* TODO: determine correct type and initial value */
u32 D_800EE79Eb; /* TODO: determine correct type and initial value */
u32 D_800EE79Ec; /* TODO: determine correct type and initial value */
u32 D_800EE854b; /* TODO: determine correct type and initial value */
u32 D_800EE858a; /* TODO: determine correct type and initial value */
u32 D_800EE858b; /* TODO: determine correct type and initial value */
u32 D_800EEDFCd; /* TODO: determine correct type and initial value */
u32 D_800EEE7Cd; /* TODO: determine correct type and initial value */
u32 D_800F01F4b; /* TODO: determine correct type and initial value */
u32 D_800F0216b; /* TODO: determine correct type and initial value */
u32 tt_config; /* TODO: determine correct type and initial value */
u32 taco_rec; /* TODO: determine correct type and initial value */
u32 ot_work0_org; /* TODO: determine correct type and initial value */
u32 ot_work1_org; /* TODO: determine correct type and initial value */
/* 0x800E8CC4..D0: the timer/counter block VSync reads (the names
 * were D_800F8Cxx until 2026-09-04 -- a negative %lo misread). */
volatile s32 *libapi_vsync_gpustat_ptr;
volatile s32 *libapi_vsync_rcnt1_count_ptr;
s32 Hcount;
s32 libapi_vsync_last_vcount;
u32 D_800F92F0; /* TODO: determine correct type and initial value */
u32 D_800FE760; /* TODO: determine correct type and initial value */
u32 D_80127180b; /* TODO: determine correct type and initial value */
u32 yazirushi_cw_img; /* TODO: determine correct type and initial value */
u32 yazirushi_cw_anim_tbl; /* TODO: determine correct type and initial value */
u32 yazirushi_cw_hdr; /* TODO: determine correct type and initial value */
u32 konami_mark_cw_img; /* TODO: determine correct type and initial value */
u32 konami_mark_cw_anim_tbl; /* TODO: determine correct type and initial value */
u32 konami_mark_cw_hdr; /* TODO: determine correct type and initial value */
u32 mod_w; /* TODO: determine correct type and initial value */
u32 pslt; /* TODO: determine correct type and initial value */
u32 pslt_1_vx; /* TODO: determine correct type and initial value */
u32 pslt_2_vx; /* TODO: determine correct type and initial value */
/* coord_buf_r: a view of coord_buf (title_init_Coordinate), see
 * missing_aliases.ld.  A host u32 here took 1,280 bytes of coordinate
 * stores and left the PS1 GsCOORDINATE2 records 13-15 all zero. */
static void _noop_overlay(void) { }
void (*game_mode_step)(void) = _noop_overlay; /* overlay dispatch — no-op until overlays loaded */
u32 album_chr_rec0_sys_tt_config_sav0; /* TODO: determine correct type and initial value */
u32 album_chr_rec0_sys_taco_rec_sav0; /* TODO: determine correct type and initial value */
unsigned long movie_state;   /* pointer-sized: the game declares it as a pointer (2026-10-16) */ /* TODO: determine correct type and initial value */
u32 D_8012DCF0w; /* TODO: determine correct type and initial value */
u32 D_8012DD60; /* TODO: determine correct type and initial value */
u32 D_8012DD70; /* TODO: determine correct type and initial value */
u32 D_8012DD74; /* TODO: determine correct type and initial value */
/* gSDInfo_frame: SD_SerialFrame's view of gSDInfo (0x8012DD80, 32 bytes),
 * PROVIDE()d in missing_aliases.ld.  A host u32 here took stores at +0,
 * +4 and +0x1C (pc/tools/audit_host_globals.py: OVERFLOW). */
s16 voice_vol_l; /* pair with seq_vol_l/A2/A6 — SD_StereoModeSet render position */
s16 voice_vol_r; /* pair with seq_vol_l/A2/A4 — SD_StereoModeSet render position */
u32 serial_vol_arg; /* TODO: determine correct type and initial value */
u32 D_80130000_array; /* TODO: determine correct type and initial value */
u32 D_80131314b; /* TODO: determine correct type and initial value */
u32 GsIDMATRIX2; /* TODO: determine correct type and initial value */
u32 DSTACK; /* TODO: determine correct type and initial value */
u32 D_80138964; /* TODO: determine correct type and initial value */
u32 D_8013897C; /* TODO: determine correct type and initial value */
u32 D_80138980; /* TODO: determine correct type and initial value */
unsigned long libcd_stintr_index_reg_ptr;   /* pointer-sized: the game declares it as a pointer (2026-10-16) */ u32 D_800BA464_w;
u32 msg_str_disp_str0;
u32 msg_str_disp_str1;
u32 msg_str_disp_str2;
u32 msg_str_disp_str3;
u32 msg_str_disp_str4;
u32 telephone_choice_menu_init_tbl0;
u32 D_800B9C54;
u32 D_800B9E64;
u32 near_click_check_tbl0;
u8 disp_string_tbl0[424]; /* sprite table: 53 entries x 8 bytes */
unsigned long x_taku_menu_set_str0;   /* pointer-sized: the game declares it as a pointer (2026-10-16) */
u32 sndisp_tbl0;
unsigned long card_file_save_tbl2;   /* pointer-sized: the game declares it as a pointer (2026-10-16) */
u32 D_800BD178;
unsigned long now_s;   /* pointer-sized: the game declares it as a pointer (2026-10-16) */
unsigned long now_s_in;   /* pointer-sized: the game declares it as a pointer (2026-10-16) */
unsigned long ss_s;   /* pointer-sized: the game declares it as a pointer (2026-10-16) */
u32 serifu_cmd_exec_tbl56;
/* libpress_dma0_madr_reg_ptr-09C0 moved to globals.c as PSX-MMIO pointer-typed
 * globals so libsnd register-deref calls land in the 0x1F801000
 * mmap instead of garbage-faulting. */
u32 libcd_unsetring_index_reg_ptr;
u32 libcd_unsetring_irq_reg_ptr;
unsigned long libcd_stintr_response_reg_ptr;   /* pointer-sized: the game declares it as a pointer (2026-10-16) */
u32 libcd_dma_exec_dpcr_ptr;
u32 libcd_dma_exec_dicr_ptr;
unsigned long libcd_stintr_dma1_chcr_ptr;   /* pointer-sized: the game declares it as a pointer (2026-10-16) */
u32 libcd_stintr_dma3_chcr_ptr;
/* PS1 SPU voice register pointers. On PS1 these are initialized by
 * SD_Init/CD_init to hardware voice register addresses (0x1F801C00+).
 * On PC they have no hardware; point them at a scratch buffer so
 * CD_vol / CD_init / normal_date_bg_in_sub byte-writes do not
 * null-deref. libcd_init_spu_regs_ptr is used as s16[] (see CD_init); give it
 * 512 halfwords (1KB) to cover index 221 = 442 bytes. */
static u8 g_spu_scratch_DAC[16];
static u8 g_spu_scratch_DB0[16];
static u8 g_spu_scratch_DB4[16];
static u8 g_spu_scratch_DB8[16];
static s16 g_spu_scratch_DBC[512];
u8 *libcd_reg_index_ptr = g_spu_scratch_DAC;
u8 *libcd_reg_cmd_resp_ptr = g_spu_scratch_DB0;
u8 *libcd_reg_param_ptr = g_spu_scratch_DB4;
u8 *libcd_reg_irq_ptr = g_spu_scratch_DB8;
s16 *libcd_init_spu_regs_ptr = g_spu_scratch_DBC;
unsigned long libcd_getsector_cdrom_delay_ptr;   /* pointer-sized: the game declares it as a pointer (2026-10-16) */
unsigned long libcd_getsector_com_delay_ptr;   /* pointer-sized: the game declares it as a pointer (2026-10-16) */
unsigned long libcd_getsector_dpcr_ptr;   /* pointer-sized: the game declares it as a pointer (2026-10-16) */
unsigned long libcd_getsector_dma3_madr_ptr;   /* pointer-sized: the game declares it as a pointer (2026-10-16) */ u32 libcd_getsector_dma3_bcr_ptr; u32 libcd_dataready_index_reg_ptr; u32 libcd_dataready_irq_reg_ptr;

/* Globals in blob range that conflict with #define macros */
unsigned long D_800E39A8;   /* pointer-sized: the game declares it as a pointer (2026-10-16) */
unsigned long libspu_dma4_madr_reg_ptr;   /* pointer-sized: the game declares it as a pointer (2026-10-16) */
unsigned long libspu_dma4_bcr_reg_ptr;   /* pointer-sized: the game declares it as a pointer (2026-10-16) */
unsigned long libspu_dma4_chcr_reg_ptr;   /* pointer-sized: the game declares it as a pointer (2026-10-16) */
unsigned long libspu_dma_dpcr_reg_ptr;   /* pointer-sized: the game declares it as a pointer (2026-10-16) */
unsigned long libspu_dma_dicr_reg_ptr;   /* pointer-sized: the game declares it as a pointer (2026-10-16) */
/* graph_jtb holds a function-pointer-table base. PS1 stored it as a 32-bit
 * absolute address; on x86-64, callers (e.g. ClearImage) read it as a
 * pointer (8 bytes), so we MUST give it pointer storage. Previously this
 * was `u32`, which truncated the high 32 bits of any pointer assigned by
 * gpu_init() and caused a NULL-fnptr SIGSEGV on first sound dispatch
 * (scene 17 sub=0 frame 0). */
void *graph_jtb;
unsigned long GPU_status;   /* pointer-sized: the game declares it as a pointer (2026-10-16) */
unsigned long libgpu_dma2_madr_ptr;   /* pointer-sized: the game declares it as a pointer (2026-10-16) */
unsigned long libgpu_dma2_chcr_ptr;   /* pointer-sized: the game declares it as a pointer (2026-10-16) */
unsigned long _qlog;   /* pointer-sized: the game declares it as a pointer (2026-10-16) */
u32 libgpu_dma6_bcr_ptr;
u32 libgpu_dma6_chcr_reg_ptr;
unsigned long libgpu_dma_dpcr_reg_ptr;   /* pointer-sized: the game declares it as a pointer (2026-10-16) */
/* GPU_printf — Sound/SEQ callback function pointer. Called by many sound
 * functions with varying argument counts. Stub it to a no-op. */
static void seq_callback_noop(void) { }
void (*GPU_printf)() = seq_callback_noop;
/* libetc_intr_vtbl_ptr — GsTMD fast primitive dispatch vtable pointer. On PS1 it's
 * initialized by GsInitGraph/ResetGraph to point to a table of 6+ slots
 * (f0/f4/f8/f12/f16/f20). Null on PC → sig11 at offset 0x18 when
 * the f12 slot (libetc_intr_vtbl_ptr->f12) dispatches. Point it at a no-op
 * vtable so callers short-circuit silently. */
static void gs_vtable_noop(void) { }
typedef void (*gs_fptr)(void);
static gs_fptr g_gs_vtable[8] = {
    gs_vtable_noop, gs_vtable_noop, gs_vtable_noop, gs_vtable_noop,
    gs_vtable_noop, gs_vtable_noop, gs_vtable_noop, gs_vtable_noop
};
gs_fptr *libetc_intr_vtbl_ptr = g_gs_vtable;
unsigned long libapi_trapintr_stat_reg_ptr;   /* pointer-sized: the game declares it as a pointer (2026-10-16) */
unsigned long _mask;   /* pointer-sized: the game declares it as a pointer (2026-10-16) */
unsigned long libapi_rcnt1_mode_reg_ptr;   /* pointer-sized: the game declares it as a pointer (2026-10-16) */
unsigned long libetc_dma_dicr_ptr;   /* pointer-sized: the game declares it as a pointer (2026-10-16) */
/* cdq_que - 48-slot table, each entry 24 bytes = 1152 bytes total.
 * An ARRAY, as every user declares it (`extern Entry cdq_que[48]`); it was a
 * pointer to a static table, so once the matched queue worker started
 * writing slot 0 (2026-10-15) the first enqueue overwrote the pointer and
 * the libapi callback vtable after it (libetc_intr_vtbl_ptr -> ResetCallback crash). */
u8 cdq_que[1152];
unsigned long libcd_getsector_dma3_chcr_ptr;   /* pointer-sized: the game declares it as a pointer (2026-10-16) */

/* CSE-breaking aliases and missing globals */
u32 D_800B9C34b;
u32 D_800B9C34c;
u32 D_800B9C34d;
u32 D_800B9C34e;
u32 D_800B9C34f;
u32 D_800B9C34g;
u32 D_800B9C34h;
u32 D_800B9C34i;
u32 D_800B9C34j;
u32 D_800B9C34k;
u32 D_800B9C34l;
u32 D_800B9C34m;

/* Menu/view system globals (dec_bg_show, icon_cursor_move) */
s32 D_800BDF24;            /* menu state storage */
s8  D_800EE793;            /* current view index */
u8  D_800EE78D_alt;        /* alt motion speed */
u8  D_800EE78E;            /* motion type */
s8  D_800BBEDC;            /* previous view index */
u8  D_800BBEE0;            /* transition counter */
u8  card_busy;            /* menu visibility */

/* Missing globals from hand-implemented functions */
u16 D_80128B6E;            /* text animation */
u16 D_80128B70;            /* text animation */
u32 D_800F6CCC;            /* evaluation display */
u32 D_800F6CC8;            /* evaluation display */
u32 D_8012B234;            /* girl_menu_select init */
u32 D_8012B23C;            /* girl_menu_select init */
u32 D_8012B240;            /* girl_menu_select init */
u32 D_8012A548;            /* girl_menu_select sprite */
u16 D_8012A592;            /* girl_menu_select sprite */
u16 D_8012A56E;            /* girl_menu_select sprite */
u16 D_8012A584;            /* girl_menu_select sprite */
u16 D_8012A586;            /* girl_menu_select sprite */
u32 D_800EB72C;            /* sound effect */
u32 D_800EB730;            /* sound effect */
u32 D_800E2B88;            /* VAB address */
u32 D_800E2B8C_arr;        /* VAB array */
u32 D_800E2B84;            /* VAB address */
u32 D_800E2DA4;            /* VAB address */
u32 D_800E2DA0;            /* VAB address */

/* Trace-build (TRACE=1) unmasks latent undef refs in overlay dead code
 * paths. These are weak symbols so the real definition (if any) wins. */
__attribute__((weak)) u32 D_8015F610;
__attribute__((weak)) u32 taiiku_end;
__attribute__((weak)) u32 D_8015EBB0;
__attribute__((weak)) u32 D_8015EBAC;
__attribute__((weak)) void func_8013753C(void) {}
__attribute__((weak)) void func_801487C0(void) {}
u32 libspu_revclear_regs_ptr;
