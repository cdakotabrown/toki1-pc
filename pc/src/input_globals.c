/* SPDX-License-Identifier: GPL-3.0-or-later */
/* input_globals.c -- the PS1 input state, laid out at retail byte offsets.
 *
 * Three objects the matched game code addresses THROUGH ONE BASE SYMBOL
 * with fixed offsets, so separate C variables (whose placement the native
 * toolchain chooses) cannot stand in for them:
 *
 *   0x800EF6C4..0x800EF6EF  the cursor / pad state block
 *       mouse_pad_set (0x80042F20) reads it as `Sys mouse_pad_st` (speed at
 *       +0x0B through rel at +0x28); pad_scan (0x80043330) as `Sys pad_type`
 *       (cur/prev/trig/rel at +0x0C..+0x18 from pad_type);
 *       step_directional_scroll_once reaches csr_speed as icon_mode+3;
 *       save_game_set / save_game_restore persist its first four bytes.
 *   0x800F10D0..0x800F10D7  the port-1 response buffer that InitPAD(&pad1_buf,
 *       8, &pad2_buf, 8) registers (input_events_init 0x800425E8,
 *       pad_type_check 0x80042D48): status, device id, two button bytes,
 *       then the mouse's signed X/Y deltas at +4/+5 (pad1_ax / pad1_ay).
 *   0x800F1110..0x800F1117  the port-2 response buffer.
 *
 * Every decomp name for one of these bytes is a linker VIEW of the block
 * (`.set name, block + offset`), so `pad_type`, `mouse_pad_st.key`,
 * `icon_mode + 8` and `(&pad1_buf)[4]` are the same byte here as on the
 * console.  The views live in this TU so the assembler resolves them
 * against the block's own label.  Before this file, globals.c declared
 * each name as its own variable: pad_type then sat where mouse_pad_set
 * expected `rkey`, and every `cnt`/`rkey` write clobbered the detector's
 * mode byte (docs/pc-port/PC-PORT-MOUSE.md).
 *
 * The same shape (struct-offset reads of unrelated globals) exists
 * elsewhere in the matched code -- step_directional_scroll_once reaches
 * menu_mode as icon_mode+0x126, OPTION reaches option_on (`.bits`) from
 * csr_speed / click_mode / mouse_btn_mode -- and is NOT covered here; see
 * the doc's follow-up list.
 *
 * ONE RULE FOR CODE THAT TOUCHES THESE: the native compiler treats two
 * different symbols as two different objects, so within ONE function it may
 * hoist a read of `ps1_input_state[0x10]` above a store to `pad_type` (or
 * a read of `mouse_pad_st.key` above a store to `pad_type`).  Use one
 * spelling per byte per function.  Audited 2026-10-15 over every file that
 * declares mouse_pad_st / icon_mode / pad_type / csr_speed / click_mode /
 * mouse_btn_mode / pad_data as a struct (mouse_pad_set, pad_scan -- now a
 * pc-port override --, step_directional_scroll_once, save_game_set/restore,
 * magazine_flag_set, TEL/func_80132FCC, icon_init, OPTION/func_801361C0,
 * option_input_step, option_click_mode, option_mouse_mode): none also
 * stores the same byte through a plain name.  The HAL itself writes only
 * the record arrays (pc/hal/pad_device.c) and reads only plain names
 * (pc/hal/pad.c), in different translation units.
 */
#include "types.h"

#include "pc_asm.h"

/* A linker view: NAME is the byte at BLOCK + OFF.  Typed by its users. */
#define PS1_VIEW(name, block, off) \
    __asm__(".globl " #name "\n\t" \
            PC_ASM_TYPE_OBJ(#name) \
            ".set " #name ", " #block " + " #off "\n")

/* ---- 0x800EF6C4..0x800EF6EF: cursor / pad state ------------------------ */

u8 ps1_input_state[0x2C] __attribute__((aligned(4))) = {
    [0x09] = 14,            /* icon_num: title sprite slot count */
};

PS1_VIEW(mouse_pad_st,   ps1_input_state, 0x00); /* 0x800EF6C4 u32 head; the struct base */
PS1_VIEW(sched_icon,     ps1_input_state, 0x04); /* 0x800EF6C8 s32 */
PS1_VIEW(icon_mode,      ps1_input_state, 0x08); /* 0x800EF6CC u8 (also a struct base) */
PS1_VIEW(icon_num,       ps1_input_state, 0x09); /* 0x800EF6CD u8 = 14 */
PS1_VIEW(bustup_sw,      ps1_input_state, 0x0A); /* 0x800EF6CE u8 */
PS1_VIEW(csr_speed,      ps1_input_state, 0x0B); /* 0x800EF6CF u8  mouse_pad_st.speed  */
PS1_VIEW(click_mode,     ps1_input_state, 0x0C); /* 0x800EF6D0 u8  mouse_pad_st.repeat */
/*                                   0x0D     0x800EF6D1 u8  mouse_pad_st.cnt    (no decomp name) */
/*                                   0x0E     0x800EF6D2 u8  mouse_pad_st.rkey   (no decomp name) */
PS1_VIEW(click_speed,    ps1_input_state, 0x0F); /* 0x800EF6D3 u8  mouse_pad_st.delay  */
PS1_VIEW(pad_type,       ps1_input_state, 0x10); /* 0x800EF6D4 u8  mouse_pad_st.key: 0 mouse, 1 pad, 0xFF none */
PS1_VIEW(mouse_btn_mode, ps1_input_state, 0x11); /* 0x800EF6D5 u8  mouse_pad_st.swap   */
/*                                   0x12     0x800EF6D6 u8  mouse_pad_st.last   (no decomp name) */
PS1_VIEW(click_flags,    ps1_input_state, 0x13); /* 0x800EF6D7 u8  mouse_pad_st.fresh  */
PS1_VIEW(D_800EF6D8,     ps1_input_state, 0x14); /* 0x800EF6D8 u8  mouse_pad_st.held   */
PS1_VIEW(mouse_x,     ps1_input_state, 0x16); /* 0x800EF6DA s16 mouse_pad_st.x      */
PS1_VIEW(mouse_y,     ps1_input_state, 0x18); /* 0x800EF6DC s16 mouse_pad_st.y      */
PS1_VIEW(pad_data,       ps1_input_state, 0x1C); /* 0x800EF6E0 u32 mouse_pad_st.cur  / pad_type.cur  */
PS1_VIEW(pad_prev,       ps1_input_state, 0x20); /* 0x800EF6E4 u32 mouse_pad_st.prev / pad_type.prev */
PS1_VIEW(pad_trig,       ps1_input_state, 0x24); /* 0x800EF6E8 u32 mouse_pad_st.trig / pad_type.trig */
PS1_VIEW(pad_rel,        ps1_input_state, 0x28); /* 0x800EF6EC u32 mouse_pad_st.rel  / pad_type.rel  */

/* ---- 0x800F10D0 / 0x800F1110: the two libpad response buffers ---------- */

/* Both start "no controller": status 0xFF, as an empty port reads before
 * the first poll.  pc/hal/pad_device.c rewrites them once per game tick,
 * standing in for the BIOS VBLANK poll StartPAD arms. */
u8 ps1_pad1_rec[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0 };
u8 ps1_pad2_rec[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0 };

PS1_VIEW(pad1_buf,    ps1_pad1_rec, 0); /* 0x800F10D0 status: 0 = device answered, 0xFF = none */
PS1_VIEW(pad1_buf_1,  ps1_pad1_rec, 1); /* 0x800F10D1 id/len: 0x41 digital pad, 0x12 mouse */
PS1_VIEW(pad1_btn_lo, ps1_pad1_rec, 2); /* 0x800F10D2 active-low SELECT/START/UP/RIGHT/DOWN/LEFT (0xFF on a mouse) */
PS1_VIEW(pad1_btn_hi, ps1_pad1_rec, 3); /* 0x800F10D3 active-low L2/R2/L1/R1/TRI/CIRCLE/CROSS/SQUARE; mouse: 0x08 left, 0x04 right */
PS1_VIEW(pad1_ax,     ps1_pad1_rec, 4); /* 0x800F10D4 s8 mouse X delta */
PS1_VIEW(pad1_ay,     ps1_pad1_rec, 5); /* 0x800F10D5 s8 mouse Y delta */

PS1_VIEW(pad2_buf,    ps1_pad2_rec, 0); /* 0x800F1110 */
PS1_VIEW(pad2_id,     ps1_pad2_rec, 1); /* 0x800F1111 */
PS1_VIEW(pad2_btn_lo, ps1_pad2_rec, 2); /* 0x800F1112 */
PS1_VIEW(pad2_btn_hi, ps1_pad2_rec, 3); /* 0x800F1113 */

/* ---- 0x800E92A0..0x800E92C4: the libetc mouse helper state --------------
 * The matched InitMouse owns the two four-byte PS1 record handles.
 * Readers zero-extend each handle; native pointer arrays would overlap the
 * accumulators in the mapped PS1 RAM. */
extern u32 mouse_rec_tbl[2];        /* 0x800E92A0 */
s32 mouse_x_acc[2];                 /* 0x800E92A8 */
s32 mouse_y_acc[2];                 /* 0x800E92B0 */
s32 mouse_x_min;                    /* 0x800E92B8 */
s32 mouse_x_max;                    /* 0x800E92BC */
s32 mouse_y_min;                    /* 0x800E92C0 */
s32 mouse_y_max;                    /* 0x800E92C4 */
