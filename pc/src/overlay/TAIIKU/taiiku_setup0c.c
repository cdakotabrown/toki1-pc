/* SPDX-License-Identifier: GPL-3.0-or-later */
/* taiiku_setup0c -- PC override of src/overlay/TAIIKU/taiiku_setup0c.c
 * (0x80145534).
 *
 * Statement for statement the matched function except the end test of the
 * case-0 copy-out loop.  The matched C ends it with
 * `bsrc != (Blk16 *)buf108`: bsrc walks buf72 (9 words) from buf72 + 1 in
 * 16-byte steps, and on the console buf72 + 9 IS buf108 -- IDO lays the two
 * frame arrays out adjacently (the header of the matched file explains why
 * that spelling was needed for the byte match).  GCC neither places the
 * locals that way nor lets a pointer into buf72 compare equal to buf108:
 * it compiled the loop with no exit at all (an unconditional jmp), so the
 * copy ran off the stack and faulted (fault_addr 0x44e00000) in the
 * sports-festival entry right after an event is chosen (scene 114
 * sub-step 1; playthrough spring_events.txt).  The end test here is
 * `bsrc != (Blk16 *)(buf72 + 9)`, the address the console reaches: two
 * iterations, 32 bytes into taiiku_spoon_scene_tpg[1..8], as retail.
 *
 * @DivergesFrom: src/overlay/TAIIKU/taiiku_setup0c.c -- the copy-out loop's
 * end pointer only (same address on the console, defined on the host).
 */
#include "types.h"

typedef struct { s32 a, b, c; } Vec3;
typedef struct { s32 w[4]; } Blk16;
typedef struct { s32 w[12]; } Blk48;
typedef struct { s32 w[9]; } Blk36;

extern s32 sub_step_up();
extern s32 sub_sub_step_up();
extern s32 palette_upload_enqueue();
extern s32 get_CD_State2();
extern s32 cd_read_start2();
extern s32 game_reset();
extern s32 search_load_tpage_buf_lock();
extern s32 class_clear_all();
extern s32 set_flag_b3_high_bit();
extern s32 date_x_taku_set();
extern s32 Fade_Out_Init();
extern s32 bri_fade_out();
extern s32 taiiku_bufs_set();
extern s32 taiiku_tbl_init();
extern void taiiku_module_init();
extern void taiiku_panel_init();
extern s32 taiiku_race_init4();

extern s32 taiiku_setup0c_pl_tpg;
extern Blk48 taiiku_setup0c_tbl0;
extern Blk36 taiiku_setup0c_tbl1;
extern u8 now_sub_sub_step;
extern s32 screen_w[];
extern s32 bukatu_tbl_end[];
extern s32 bukatu_tbl[];
extern s32 taiiku_spoon_pl_tpg;
extern s32 taiiku_spoon_sprite_tpg[12];
extern s32 taiiku_spoon_scene_tpg[];
extern s32 taiiku_spoon_buf_1;

void taiiku_setup0c(void) {
    s32 buf108[13];
    s32 buf72[9];
    s32 *p;
    s32 *so;
    s32 *dstp;
    Blk16 *bsrc;
    Blk16 *bdst;
    u8 sel;
    s32 r;
    s32 i;
    s32 c0;
    s32 c1;
    s32 c2;
    s32 c3;

    buf108[12] = taiiku_setup0c_pl_tpg;
    sel = now_sub_sub_step;
    *(Blk48 *)buf108 = taiiku_setup0c_tbl0;
    *(Blk36 *)buf72 = taiiku_setup0c_tbl1;

    switch ((u32)sel) {
    case 0:
        date_x_taku_set(1, 1);
        p = screen_w;
        do {
            p++;
            p[1166] = 8;
        } while (p < bukatu_tbl_end);
        p = screen_w;
        do {
            p++;
            p[1198] = 8;
        } while (p < bukatu_tbl);
        taiiku_spoon_pl_tpg = buf108[12];
        so = buf108;
        dstp = taiiku_spoon_sprite_tpg;
        do {
            *dstp = *so;
            so++;
            dstp++;
        } while (so < &buf108[12]);
        taiiku_spoon_scene_tpg[0] = buf72[0];
        bsrc = (Blk16 *)(buf72 + 1);
        bdst = (Blk16 *)(&taiiku_spoon_scene_tpg[1]);
        do {
            c1 = bsrc->w[1];
            c2 = bsrc->w[2];
            c3 = bsrc->w[3];
            c0 = bsrc->w[0];
            bsrc++;
            bdst->w[1] = c1;
            bdst->w[2] = c2;
            bdst->w[3] = c3;
            bdst->w[0] = c0;
            bdst++;
        } while (bsrc != (Blk16 *)(buf72 + 9));
        class_clear_all(0);
        set_flag_b3_high_bit(0);
        sub_sub_step_up();
        break;
    case 1:
        cd_read_start2(30, (s32)0x80180000, 0xBAD7);
        Fade_Out_Init();
        sub_sub_step_up();
        break;
    case 2:
        bri_fade_out();
        r = get_CD_State2();
        if (r & 1) {
            cd_read_start2(38, (s32)0x8018F000, 0xB970);
            sub_sub_step_up();
        }
        break;
    case 3:
        r = bri_fade_out();
        if (r != 0) {
            r = get_CD_State2();
            if (r & 1) {
                taiiku_tbl_init();
                taiiku_bufs_set();
                search_load_tpage_buf_lock(taiiku_spoon_pl_tpg, 1);
                for (i = 0; i != 16; i++) {
                    palette_upload_enqueue(taiiku_spoon_buf_1, (i + 16) & 0xFF, 1, 1, i);
                }
                taiiku_race_init4();
                taiiku_module_init();
                taiiku_panel_init();
                cd_read_start2(185, (s32)0x8018F000, 0xB8B7);
                sub_step_up();
            }
        }
        break;
    default:
        game_reset();
        break;
    }
}
