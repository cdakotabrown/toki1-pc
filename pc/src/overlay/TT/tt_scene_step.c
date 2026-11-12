/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tt_scene_step: retain packed state and timer callback cells on x64.
 * @DivergesFrom: src/overlay/TT/tt_scene_step.c -- the state cursor at +24
 * and timer callback at +12 stay u32; pointers widen only at use.
 * Callback selection and all state transitions follow the matched source.
 * tt_mode_set stores native non-PIE callback addresses in the retail cell.
 */
#include "types.h"

typedef struct {
    /* 0x00 */ u8  unk00;
    /* 0x01 */ u8  unk01;
    /* 0x02 */ u8  pad02[14];
    /* 0x10 */ u16 unk10;
    /* 0x12 */ s16 unk12;
    /* 0x14 */ s16 unk14;
    /* 0x16 */ s16 unk16;
    /* 0x18 */ u8  pad18[32];
    /* 0x38 */ u8  unk38;
    /* 0x39 */ u8  unk39;
    /* 0x3A */ u8  unk3A;
    /* 0x3B */ u8  unk3B;
    /* 0x3C */ s16 unk3C;
    /* 0x3E */ s16 unk3E;
    /* 0x40 */ s16 unk40;
    /* 0x42 */ s16 unk42;
    /* 0x44 */ u16 unk44;
    /* 0x46 */ u8  unk46;
    /* 0x47 */ u8  unk47;
    /* 0x48 */ u8  pad48[28];
} TTRec;

typedef struct {
    /* 0x00 */ u8  unk0;
    /* 0x01 */ u8  unk1;
    /* 0x02 */ u8  unk2;
    /* 0x03 */ u8  pad3[3];
    /* 0x06 */ u16 unk6;
    /* 0x08 */ s16 unk8;
    /* 0x0A */ s16 unkA;
    /* 0x0C */ s16 unkC;
    /* 0x0E */ s16 unkE;
    /* 0x10 */ s16 unk10;
    /* 0x12 */ s16 pad12;
} TTObj;

typedef struct {
    /* 0x00 */ u8  pad0[8];
    /* 0x08 */ u16 unk8;
    /* 0x0A */ u8  padA[10];
    /* 0x14 */ u16 unk14;
    /* 0x16 */ u8  pad16[58];
    /* 0x50 */ s16 unk50;
    /* 0x52 */ u8  unk52;
} TTMain;

typedef struct {
    /* 0x00 */ u16 unk0;
    /* 0x02 */ u16 unk2;
    /* 0x04 */ u16 unk4;
    /* 0x06 */ u16 unk6;
    /* 0x08 */ u8  pad8;
    /* 0x09 */ u8  unk9;
    /* 0x0A */ u8  unkA;
    /* 0x0B */ u8  unkB;
    /* 0x0C */ u8  padC[5];
    /* 0x11 */ u8  unk11;
    /* 0x12 */ u8  unk12;
    /* 0x13 */ u8  pad13[5];
    /* 0x18 */ u32 unk18;
    /* 0x1C */ u8  unk1C;
    /* 0x1D */ u8  pad1D[5];
    /* 0x22 */ u8  unk22;
} TTState;

typedef struct {
    /* 0x00 */ s32 unk0;
    /* 0x04 */ u8  pad4[4];
    /* 0x08 */ u16 unk8;
    /* 0x0A */ u8  padA[22];
} TTDelta;

typedef struct {
    /* 0x00 */ union { s32 w; struct { u16 lo; u16 hi; } h; } unk0;
    /* 0x04 */ u8  pad4[8];
    /* 0x0C */ u32 callback_addr;
    /* 0x10 */ u8  pad10[16];
} TTTimer;

typedef struct {
    /* 0x00 */ u8  pad0[4];
    /* 0x04 */ u16 unk4;
    /* 0x06 */ u16 unk6;
    /* 0x08 */ u16 unk8;
    /* 0x0A */ u16 unkA;
    /* 0x0C */ u8  unkC;
    /* 0x0D */ u8  unkD;
    /* 0x0E */ u8  unkE;
    /* 0x0F */ u8  unkF;
    /* 0x10 */ u8  pad10[71];
    /* 0x57 */ u8  unk57[9];
} TTPad;



extern void tt_mode_set();

extern TTMain  *tt_game_wk;
extern TTState *tt_step_wk;
extern TTDelta *tt_scroll_wk;
extern TTTimer *tt_stage_wk;
extern TTPad   *tt_player_wk;
extern TTPad   *tt_boss_wk;
extern TTRec   *tt_anim_buf;
extern TTObj   *tt_text_buf;
extern u8       tt_scene_seq_dat[];

void tt_scene_step()
{
    TTMain *pMain;
    TTState *pState;
    TTDelta *pDelta;
    TTTimer *pTimer;
    TTObj *pObj;
    TTRec *p;
    TTPad *pPad;
    TTPad *q;
    s32 i;
    u32 sel;
    s32 unused;
    s32 a;

    pMain = tt_game_wk;
    pDelta = tt_scroll_wk;
    pTimer = tt_stage_wk;
    pObj = tt_text_buf;
    pPad = tt_player_wk;
    pState = tt_step_wk;
    pTimer->unk0.w += pDelta->unk0;
    switch (pState->unk2) {
    case 0:
        if (pState->unk22 != 0) {
            sel = pMain->unk52;
            if (sel == 1 || sel == 2 || sel == 3 || sel == 4) {
                a = get_reads();
                if (((a - get_tt_read_prev() + 256) & 0xFF) < 5) {
                    return;
                }
                tt_load_wait();
                device_cmd_send(33);
                device_cmd_send(34);
                device_cmd_send(47);
                device_cmd_send(4);
                device_cmd_send(513);
            } else {
                device_cmd_send(177);
            }
        } else {
            device_cmd_send(33);
            device_cmd_send(34);
            device_cmd_send(47);
            device_cmd_send(4);
            device_cmd_send(513);
        }
        bzero(pDelta, 32);
        bzero(pTimer, 32);
        pMain->unk50 = 0;
        tt_record_clear();
        tt_text_clear();
        tt_active_clear();
        tt_player_shot_clear();
        tt_rec_clear();
        tt_recs_clear();
        trail_clear_all();
        tt_pool_clear();
        tt_obj_clear();
        tt_shot_clear();
        tt_sprite_pool_reset();
        tt_ent_clear();
        tt_slot_clear();
        if (pState->unk22 != 0) {
            pState->unk11 = 0;
        } else {
            pState->unk11 = 1;
        }
        tt_trail_pool_init();
        tt_player_shot_init();
        tt_recs_init();
        tt_scene_init();
        tt_pool_reset();
        tt_obj_reset();
        pState->unk12 = 1;
        pState->unk18 = (u32)(unsigned long)tt_scene_seq_dat;
        pState->unk1C = 2;
        tt_replay_set();
        tt_mode_set();
        pObj->unkA = -112;
        pObj->unkC = 16;
        pObj->unkE = 13;
        pObj->unk2 = 1;
        pObj->unk0 = 1;
        pObj->unk6 = 0;
        pObj->unk10 = 0;
        pObj++;
        pObj->unkA = -72;
        pObj->unkC = 16;
        pObj->unkE = 39;
        pObj->unk2 = 1;
        pObj->unk0 = 1;
        pObj->unk6 = 0;
        pObj->unk10 = 0;
        tt_time_set(pDelta->unk8, 39);
        p = tt_anim_buf;
        for (i = 0; i < 2; i++, p++) {
            p->unk00 = 1;
            p->unk01 = 0;
            p->unk44 = 0;
            p->unk14 = i * 16 - 112;
            p->unk16 = 24;
            p->unk3C = 16;
            p->unk3E = 16;
            p->unk46 = 0;
            p->unk47 = 112;
            p->unk40 = 10;
            p->unk42 = 15371;
            p->unk3B = 101;
            p->unk10 = 0;
        }
        tt_sprite_clear();
        for (i = 0; i < 6; i++, p++) {
            p->unk00 = 1;
            p->unk01 = 0;
            p->unk44 = 0;
            p->unk14 = -120;
            p->unk16 = 216 - i * 8;
            p->unk3E = 8;
            p->unk3C = 16;
            p->unk46 = (i / 2) * 16 + 80;
            p->unk47 = (i & 1) * 8 + 48;
            p->unk40 = 10;
            p->unk42 = 15369;
            p->unk3B = 101;
            p->unk10 = 0;
        }
        pPad->unk57[1] = pPad->unk57[2] = 0;
        pState->unk2 = 64;
        break;
    case 1:
        tt_load_wait2();
        device_cmd_send(33);
        device_cmd_send(34);
        device_cmd_send(47);
        device_cmd_send(4);
        device_cmd_send(513);
        tt_player_shot_clear();
        tt_rec_clear();
        tt_recs_clear();
        tt_pool_clear();
        tt_obj_clear();
        tt_shot_clear();
        tt_sprite_pool_reset();
        tt_ent_clear();
        tt_slot_clear();
        pState->unk11 = 1;
        pPad->unk57[0] = 0;
        pPad->unk57[1] = pPad->unk57[2] = 0;
        pPad->unk57[3] = 0;
        pPad->unk57[4] = 0;
        pPad->unk57[5] = 0;
        pPad->unk57[6] = 0;
        pPad->unk57[7] = 0;
        pPad->unk57[8] = 0;
        pPad->unk4 = 32;
        pPad->unkD = pPad->unkE = pPad->unkF = 0;
        pPad->unk6 = pPad->unk8 = pPad->unkA = 0;
        tt_player_shot_init();
        pState->unk12 = 1;
        pState->unk1C = 2;
        tt_mode_set();
        pState->unk2 = 64;
        break;
    case 64:
        tt_time_set(pDelta->unk8, 39);
        if (pTimer->unk0.h.hi > 1024) {
            q = tt_boss_wk;
            q->unk4 = 0;
            q->unkC = 0;
            q->unk6 = q->unk8 = q->unkA = 0;
            q->unkD = q->unkE = q->unkF = 0;
            pState->unk2 = 80;
        }
        break;
    case 80:
        tt_time_set(pDelta->unk8, 39);
        ((void (*)(void))(unsigned long)pTimer->callback_addr)();
        break;
    case 160:
        tt_time_set(pDelta->unk8, 39);
        pPad->unk4 = 160;
        break;
    }
    if (pState->unk22 != 0 && pMain->unk52 == 255) {
        pObj = tt_text_buf + 126;
        if ((pMain->unk8 >> 4) & 1) {
            pObj->unk6 = 1;
        } else {
            pObj->unk6 = 0;
        }
        if ((pMain->unk14 & 0x800) || (pMain->unk14 & 0x20)) {
            pState->unk0 = 1;
            pState->unk2 = pState->unk4 = pState->unk6 = 0;
            pState->unk9 = pState->unkA = pState->unkB = 0;
        }
    }
    trail_step_all();
    tt_player();
    tt_sprite_step();
    tt_rec_step();
    tt_recs_step();
    tt_sprite_pool_step();
    tt_ent_step_all();
    tt_slot_step();
    tt_actor_pool_step();
    obj_pool_step();
    tt_scroll_speed_step();
    tt_speed_mark_set();
    tt_scene_hit_step();
}
