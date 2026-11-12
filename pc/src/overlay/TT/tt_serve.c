/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tt_serve: keep 20-byte unit records and enclosing retail offsets.
 * @DivergesFrom: src/overlay/TT/tt_serve.c -- shared four-byte script cells
 * replace native pointers. State transitions and script semantics are unchanged.
 */
#include "types.h"
#include "tt_serve_unit.h"

typedef TtServeUnit Unit;

typedef struct {
    u8  b0;         /* 0x000 */
    u8  pad001[3];
    u16 h4;         /* 0x004 */
    s16 h6;         /* 0x006 */
    u16 h8;         /* 0x008 */
    u16 h10;        /* 0x00A */
    u8  pad00C;
    u8  b13;        /* 0x00D */
    u8  b14;        /* 0x00E */
    u8  b15;        /* 0x00F */
    u8  pad010[40];
    Unit unit[24];  /* 0x038 */
    s32 w536;       /* 0x218 */
    u8  pad21C[44];
    s16 h584[24];   /* 0x248 */
    s16 h632[24];   /* 0x278 */
    s16 h680[24];   /* 0x2A8 */
    s16 h728[4];    /* 0x2D8 */
    u8  pad2E0[44];
    s16 h780;       /* 0x30C */
    s16 h782;       /* 0x30E */
    u8  b784[24];   /* 0x310 */
    u8  pad328[43];
    s8  b851[4];    /* 0x353 */
    u8  pad357[15];
    s16 h870;       /* 0x366 */
    u8  pad368[60];
    s16 h932;       /* 0x3A4 */
    u8  pad3A6[4];
    s16 h938;       /* 0x3AA */
    s16 h940;       /* 0x3AC */
} Serve;

typedef struct {
    u8  pad00[2];
    u16 h2;         /* 0x02 */
    u16 h4;         /* 0x04 */
    u8  pad06[12];
    u8  b18;        /* 0x12 */
    u8  pad13[15];
    u8  b34;        /* 0x22 */
} TTState;

typedef struct {
    u8  pad00[82];
    s8  b82;        /* 0x52 */
} TTFlag;

typedef struct {
    u8  pad00[82];
    u8  b82;        /* 0x52 */
} TTMain;

typedef struct {
    u8  pad00[30];
    s16 h30;        /* 0x1E */
} TTTimer;

extern u8       tt_anim_tbl[];
extern TTMain  *tt_game_wk;
extern TTState *tt_step_wk;
extern TTTimer *tt_stage_wk;
extern TTFlag *tt_player_wk;
extern Serve   *tt_boss_wk;

tt_serve()
{
    TTMain *q70;
    TTState *q74;
    Serve *p;
    TTFlag *q84;
    s32 i;

    p = tt_boss_wk;
    q70 = tt_game_wk;
    q74 = tt_step_wk;
    q84 = tt_player_wk;
    switch (p->h4) {
    case 0:
        tt_sprite_pool_clear();
        if (q74->b34 != 0 && q70->b82 == 255) {
            device_cmd_send(113);
        } else if (q74->h4 == 0) {
            device_cmd_send(177);
            change_trans_pointer(0x8019A000);
            device_cmd_send(18);
            device_cmd_send(35);
            device_cmd_send(46);
            device_cmd_send(33);
            device_cmd_send(4);
            device_cmd_send(512);
        }
        tt_serve_init();
        p->h4 = 32;
        break;
    case 32:
        tt_serve_ball_step();
        tt_racket_disp();
        break;
    case 48:
        tt_serve_seq_step();
        tt_racket_disp();
        break;
    case 64:
        if (tt_boss_wk->h780 < 32) {
            p->h6 = 2;
        } else if (tt_boss_wk->h780 < 64) {
            p->h6 = 1;
        } else {
            p->h6 = 0;
        }
        tt_camera_ease();
        tt_racket_disp();
        if (p->h8 == 0) {
            tt_board_shot_step();
        }
        if (p->h780 < 0) {
            p->h4 = 80;
            p->h6 = p->h8 = p->h10 = 0;
            p->b13 = p->b14 = p->b15 = 0;
        } else if (p->h780 < 32) {
            p->unit[0].b6 = 58;
        } else if (p->h780 < 64) {
            p->unit[0].b6 = 54;
        }
        break;
    case 80:
        p->w536 = 0;
        tt_serve_walk_step();
        break;
    case 81:
        tt_serve_attack_cutscene_step();
        break;
    case 82:
        p->h4 = 83;
        p->h782 = 1;
        p->h932 = 0;
        p->h938 = 0;
        p->h940 = 1;
        p->h870 = 0;
        for (i = 0; i < 4; i++) {
            p->h728[i] = 24;
            p->b851[i] = 1;
        }
        for (i = 16; i < 21; i++) {
            p->unit[i].script_addr = (u32)(unsigned long)tt_anim_tbl;
            p->unit[i].b4 = 0;
            p->unit[i].b5 = 6;
            p->unit[i].b10 = 0;
            p->unit[i].b16 = 0;
            p->b784[i] = 0;
            p->h680[i] = 8;
            p->h584[i] = 0;
            p->h632[i] = 0;
            p->unit[i].b8 = 96;
        }
        break;
    case 83:
        tt_camera_ease();
        tt_swing_step();
        tt_seq6_step();
        tt_seq_group_step();
        tt_rally();
        if ((u8)p->b851[0] == 0 && (u8)p->b851[1] == 0 && (u8)p->b851[2] == 0 && (u8)p->b851[3] == 0) {
            p->h4 = 84;
            p->h782 = 2;
            p->h780 = 72;
        }
        break;
    case 84:
        tt_camera_ease();
        tt_swing_step();
        tt_seq6_step();
        tt_seq_group_step();
        tt_rally();
        if (p->h780 < 0) {
            tt_cmd_once(1284);
            p->h4 = 128;
            p->h782 = 3;
            p->unit[6].b8 = 128;
        }
        break;
    case 128:
        device_cmd_send(193);
        q84->b82 = 1;
        tt_serve_throw_cutscene_step();
        tt_seq6_step();
        tt_seq_group_step();
        break;
    case 160:
        device_cmd_send(177);
        p->b0 = 0;
        q74->b18 = 0;
        tt_stage_wk->h30 = 1;
        q74->h2 = 160;
        break;
    }
    tt_serve_hit_step();
}
