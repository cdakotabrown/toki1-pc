/* thunk_entry_aliases.c — PC-port forwarders for PS1 cross-prologue thunks.
 *
 * On PS1, IDO hoists a global-load instruction above the body's prologue,
 * creating a two-entry function:
 *   func_XXX (thunk): lui/lbu/lw into a scratch register, falls through...
 *   func_XXY (body):  the actual function, relies on the register preload.
 *
 * GCC on PC cannot reproduce cross-prologue scheduling. The body compiles
 * standalone and reads the global itself. Without this file, callers of
 * the thunk entry hit empty stubs in pc/src/missing_funcs.c and the body
 * never runs. These forwarders bridge the entry to the body.
 *
 */
#include "types.h"

extern u8 palette_queue_n;

extern void palette_load_vram(s32 arg0);
extern s32  func_80086D60(void);
extern s32  func_8008FBD8(s32 arg0, s16 arg1, s32 arg2, s32 mask);

extern s32 _snd_openflag;

/* (self-forwarder removed: head and host are one symbol now) */

s32 MDEC_in_sync(void) {
    return func_80086D60();
}

/* 0x80086DF0 -> func_80086DF4 (MDEC_out_sync.c names the body by the
 * prologue address).  Until 2026-10-31 a `return 0` stub in
 * pc/src/missing_funcs.c answered; DecDCToutSync reaches it 20 times per
 * still decode.  Its register cell is adapted in pc/Makefile. */
extern s32 func_80086DF4(void);
extern void libpress_mdec_out_wait(void);
s32 MDEC_out_sync(void) {
    /* the port decodes at once; the slice's out-DMA time is the HAL's
     * (pc/hal/libpress.c MDEC_OUT_STILL_WAIT) */
    libpress_mdec_out_wait();
    return func_80086DF4();
}

/* (SsSeqOpen head forwarder removed 2026-10-15: the head address took the
 * Sony name, and pc/hal/spu.c's SsSeqOpen is the PC implementation; the
 * forwarder would have shadowed it.) */

/* 0x800984BC is NOT a thunk: it is SpuFree's own entry (IDO -O2 hoisted
 * its first global read above the prologue), and nothing in retail references
 * 0x800984C4.  The matched C at src/main/matched/SpuFree.c defines the
 * symbol itself, so the forwarder that used to live here is gone (2026-09-07).
 */

/* ---- P0 thunk forwarders (runbook 2026-04-19) ------------------------- */

/* (2026-11-10: basyo_show, bustup_modes20_step, device_load_cmd_send,
 * initView, load_tpage_buf_lock, step34_init0/1, step34_disp and
 * week_day_bg_load used to be forwarded here to a matched body spelled
 * func_<prologue>; each matched file now defines its body under the
 * registry name, so the forwarders are gone.  Before 2026-10-30/31 a
 * `return 0` stub answered for those entries; pc/tools/audit_pc_stubs.py
 * guards the class.) */

/* cal_bio_disp (0x80068F6C): its matched C, cal_bio_disp.c, defines the
 * body as system_moji_set. */
extern void system_moji_set(void);
void cal_bio_disp(void) {
    system_moji_set();
}

/* (week_day_main0 and vacation_day_init forwarders removed 2026-10-31:
 * their matched C absorbed the hoisted head and defines the entry name
 * itself (week_day_main0.c @0x80074710, vacation_day_init.c @0x80074D5C),
 * but this tier precedes matched on the link line, so the forwarders to
 * the pc-port bodies func_80074714 / bust_up_face_c kept winning.) */

#if 0
/* 0x8009CAB0 → 0x8009CAC8: compute (idx*2, PSDOFSX[idx]) from PSDIDX */
extern void func_8009CAC8(s32 indexOffset, s16 value);
extern s16 PSDIDX;
extern u16 PSDOFSX[];
void GsSwapDispBuff(void) {
    s16 idx = PSDIDX;
    func_8009CAC8((s32)(idx * 2), (s16)PSDOFSX[idx]);
}
#endif

/* 0x8007A3E4 = SD_ChCmd's true entry.  Since 2026-10-03 the
 * matched C absorbs the hoisted head and switches on (u8)arg0 itself (case
 * '!'..'$', '.', '/'), so the raw argument is forwarded unchanged -- the old
 * forwarder subtracted 33 for a host that no longer expects it. */
extern void SD_ChCmd(s32 arg0);
void func_8007A3E4(s32 a0) {
    SD_ChCmd(a0);
}

/* 0x80075674 = holiday_screen_init's true entry.  Since 2026-10-03 the matched C
 * (src/main/matched/holiday_screen_init.c) covers the whole window from this
 * address with no parameters (it absorbed the hoisted holiday_screen_init_tbl0 fill);
 * the old no-op stub src/main/semi-matched/holiday_screen_init.c is gone. */
extern void holiday_screen_init(void);
/* (self-forwarder removed: head and host are one symbol now) */

#if 0  /* PARKED: body search_tpage8 truncates PS1 pointers on x86-64 */
/* 0x80048DD0 → 0x80048DD8: sign-extend s16 arg0 */
extern s32 search_tpage8(s32 arg0, s32 arg1, u8 arg2);
/* (self-forwarder removed: head and host are one symbol now) */

#endif

/* 0x8004E534 → 0x8004E54C: TileData pointer from index (40-byte stride),
 * base D_80129CF0. Body is semi-matched. */
extern s32 srn_set(void *data);
extern u8 D_80129CF0;
/* (self-forwarder removed: head and host are one symbol now) */

/* 0x80073D64 → 0x80073D8C: 10-insn cross-prologue thunk that falls
 * through into week_day_bustup_set (title-show sound-file selector). The PC
 * port translation in src/main/pc-port/week_day_bustup_set.c rolls the
 * thunk's prologue computation into the body, so this alias just
 * forwards. */
extern void week_day_bustup_set(void);
/* (self-forwarder removed: head and host are one symbol now) */

