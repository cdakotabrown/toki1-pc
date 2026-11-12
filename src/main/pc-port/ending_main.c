/*
 * ending_main -- PC override of src/main/matched/ending_main.c (0x80079628).
 *
 * Statement for statement the matched function except the
 * block_checksum_check call in sub-step 2.  block_checksum_check(blocks,
 * buf) sums `blocks` sectors at `buf`; the matched C passes only the block
 * count.  The listing loads a1 = 0x80132000 at entry (lui a1,0x8013; ori
 * a1,a1,0x2000 -- the store `ending_main_tbl1 = 0x80132000`) and nothing
 * writes a1 again on the way through the jump table to the jal 0x800466B4
 * of sub-step 2, so the console checksums the overlay window at
 * 0x80132000 that sub-step 0 had cd_read_start2 load.  On x86-64 the
 * missing argument was whatever %esi held: the sum read another address
 * (or faulted) and a mismatch sends the scene back to sub-step 0.
 * pc/tools/audit_ps1_call_args.py SHORT-READ.
 *
 * @DivergesFrom: src/main/matched/ending_main.c -- the argument the console
 * passes in $a1 (ending_main_tbl1's value) is written out.
 */
#include "types.h"

extern u8 now_sub_step;
extern u8 bunkaken_flags;
extern u8 _debug_number_show_str0;
extern u8 ending_main_tbl0;
extern u32 ending_main_tbl1;
extern u32 overlay_load_init_tbl0;
extern void (*game_mode_step)(void);
extern u32 VB;
extern u8 option_on;
extern u8 ret_step;
extern u8 ret_sub_step;
extern u8 ret_sub_sub_step;
extern void D_8013E830(void);

extern void vram_area_clear(s32);
extern void class_clear_all(s32);
extern void prim_init(void);
extern void cd_read_start2(s32, u32, s32);
extern void sub_step_up(void);
extern s32 get_CD_State2(void);
extern void goto_sub_step(s32);
extern void SetDispMask(s32);
extern void DrawSync(s32);
extern void ResetGraph(s32);
extern void CloseEvent(u32);
extern s32 block_checksum_check(s32 blocks, u32 buf);
extern void cache_flush_critical(void);
extern void goto_step(s32);
extern void goto_sub_sub_step(s32);
extern void draw2d3d(s32, s32);
extern void tpage_buf_clear(void);
extern void vblank_counter_event_init(void);
extern void device_state_init(void);
extern void dec_bg_reset(void);
extern void CDQ_init(void);
extern void device_cmd_send(s32);
extern void change_trans_pointer(u32);
extern void ending_card_step(void);

void ending_main(void) {
    s32 mode = now_sub_step;

    ending_main_tbl1 = 0x80132000;
    switch (mode) {
    case 0:
        bunkaken_flags |= 1;
        _debug_number_show_str0 = 0;
        vram_area_clear(255);
        class_clear_all(0);
        prim_init();
        ending_main_tbl0 = 96;
        overlay_load_init_tbl0 = 0x9A6A;
        cd_read_start2(96, ending_main_tbl1, 0x9A6A);
        sub_step_up();
        break;
    case 1:
        if (get_CD_State2() & 1) {
            sub_step_up();
        }
        break;
    case 2:
        if (block_checksum_check(ending_main_tbl0, ending_main_tbl1) != 0) {
            sub_step_up();
            cache_flush_critical();
            SetDispMask(0);
        } else {
            goto_sub_step(0);
        }
        break;
    case 3:
        game_mode_step = D_8013E830;
        DrawSync(0);
        ResetGraph(3);
        CloseEvent(VB);
        game_mode_step();
        vram_area_clear(255);
        sub_step_up();
        break;
    case 4:
        DrawSync(0);
        ResetGraph(3);
        vblank_counter_event_init();
        device_state_init();
        dec_bg_reset();
        CDQ_init();
        device_cmd_send(127);
        change_trans_pointer(0x801B8000);
        sub_step_up();
        break;
    case 5:
        ending_card_step();
        break;
    case 6:
        if (option_on == 0) {
            goto_step(ret_step);
            goto_sub_step(ret_sub_step);
            goto_sub_sub_step(ret_sub_sub_step);
        } else {
            goto_step(0);
            class_clear_all(0);
            draw2d3d(0, 0);
            prim_init();
            tpage_buf_clear();
        }
        break;
    }
}
