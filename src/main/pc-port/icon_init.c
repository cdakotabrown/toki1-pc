/*
 * icon_init -- background/date-panel sprite table init (PC override of
 * src/main/matched/icon_init.c, 0x8006523C).
 *
 * Statement for statement the matched function; the divergence is the
 * RECORD WIDTH (the src/main/pc-port/bg_set.c / mv_state.h doctrine,
 * same defect as src/main/pc-port/basyo_init.c).  The matched Slot68
 * declares the class_w slot's cel-header, animation and tpage-set handles
 * as `void *p12, *p16, *p52`: 4 bytes on the console, 8 bytes (8-aligned)
 * on LP64.  There the 68-byte slot becomes an 88-byte record, so
 * `class_w[i + 80]` / `class_w[i + 64]` index at an 88-byte stride from
 * class_w+7040 / +5632 instead of +5440 / +4352, every field after +12
 * moves, and the boot-time icon slots (64..95 for 16 icons) were written
 * across slots 82..124 with their handles at host offsets +16/+24/+56,
 * leaving 64..95 as class_clear_n left them (flags 0: never drawn).
 * Test: pc/tools/test_class_slot_width.py.
 *
 * Here every slot store goes through the PS1-width ClassSlot
 * (pc/include/game/class_slot.h).  Matched field -> PS1 offset:
 *   b1..b6 -> f1..f6, p12 -> f12, p16 -> f16, h20 -> f20, h22 -> f22,
 *   h24 -> f24, p52 -> f52, w56 -> f56.
 * The three handle stores zero-extend the .set-aliased PS1 addresses
 * (pc/src/ps1_addr_aliases.c), which is the value the console stores.
 *
 * @DivergesFrom: matched icon_init -- record field widths only.
 */
#include "types.h"
#include "class_slot.h"

typedef struct {
    u8  pad0[3948];
    u8  mode;               /* 0x800EF6CC */
    u8  count;              /* 0x800EF6CD */
} BgState;

typedef struct {
    s16 x;
    s16 y;
} Pos;

extern BgState screen_w;
extern volatile u8 icon_mode;
extern volatile u8 icon_num;
extern u8 icon_can_use[];
extern ClassSlot class_w[];
extern u8 basyo_init_tbl1[];
extern u8 basyo_init_tbl2[];
extern u8 _sys_default_tpage_set_tbl2[];
extern s16 basyo_init_tbl3;
extern u8 cw94_prio;
extern u8 cw78_prio;
extern s16 cw94_anim;
extern s16 cw94_x;
extern s16 cw94_y;
extern s16 cw78_anim;
extern s16 cw78_x;
extern s16 cw78_y;
extern Pos icon_xy[];
extern Pos icon_init_tbl1[];
extern Pos icon_init_tbl0[];
extern u8 hud_palette_init_tbl1[];
extern u8 hud_palette_init_tbl2[];
extern u8 icon_init_tbl2[];

extern void class_clear_n(s32);
extern void icon_menu_set(s32);
extern void load_image_rect(s32, s32, s32, s32, void *);

void icon_init(void)
{
    BgState *st;
    u32 i;
    s32 j;
    s32 cnt;

    st = &screen_w;
    if (icon_mode == 2) {
        icon_num = 15;
    }
    i = 0;
    if (icon_num != 0) {
        do {
            icon_can_use[i] = 1;
            i++;
        } while (i < st->count);
    }
    i = 0;
    if (icon_num != 0) {
        do {
            class_clear_n(i + 80);
            class_w[i + 80].f1 = 3;
            class_w[i + 80].f2 = 1;
            class_w[i + 80].f56 = 0x40000000;
            class_w[i + 80].f3 = 132;
            class_w[i + 80].f12 = CLASS_SLOT_HANDLE(basyo_init_tbl1);
            class_w[i + 80].f16 = CLASS_SLOT_HANDLE(basyo_init_tbl2);
            class_w[i + 80].f52 = CLASS_SLOT_HANDLE(_sys_default_tpage_set_tbl2);
            class_w[i + 80].f20 = basyo_init_tbl3;
            class_w[i + 80].f4 = 8;
            class_w[i + 80].f5 = 1;
            class_w[i + 80].f22 = i;
            class_w[i + 80].f6 = 1;
            class_clear_n(i + 64);
            class_w[i + 64].f1 = 4;
            class_w[i + 64].f2 = 64;
            class_w[i + 64].f56 = 0x40000000;
            class_w[i + 64].f3 = 132;
            class_w[i + 64].f12 = CLASS_SLOT_HANDLE(basyo_init_tbl1);
            class_w[i + 64].f16 = CLASS_SLOT_HANDLE(basyo_init_tbl2);
            class_w[i + 64].f52 = CLASS_SLOT_HANDLE(_sys_default_tpage_set_tbl2);
            class_w[i + 64].f20 = basyo_init_tbl3;
            class_w[i + 64].f4 = 8;
            class_w[i + 64].f5 = 1;
            class_w[i + 64].f24 = 2;
            i++;
        } while (i < st->count);
    }
    cw94_prio = 3;
    cw78_prio = 4;
    cw94_anim = 15;
    cw94_x = 0;
    cw94_y = 0;
    cw78_anim = 15;
    cw78_x = 0;
    cw78_y = 0;
    switch ((u32)icon_mode) {
    case 0:
        i = 0;
        if (icon_num != 0) {
            do {
                icon_xy[i].x = icon_init_tbl1[i].x;
                icon_xy[i].y = icon_init_tbl1[i].y;
                i++;
            } while (i < st->count);
        }
        break;
    case 1:
        i = 0;
        if (icon_num != 0) {
            do {
                icon_xy[i].x = icon_init_tbl0[i].x;
                icon_xy[i].y = icon_init_tbl0[i].y;
                i++;
            } while (i < st->count);
        }
        break;
    case 2:
        cnt = 0;
        for (i = 0; i < 15; i++) {
            if (icon_xy[i].x == 0 && icon_xy[i].y == 0) {
                cnt++;
            }
            if (cnt >= 2) {
                for (j = 0; j < 15; j++) {
                    icon_xy[j].x = icon_init_tbl1[j].x;
                    icon_xy[j].y = icon_init_tbl1[j].y;
                }
                break;
            }
        }
        break;
    }
    icon_menu_set(icon_num);
    load_image_rect(288, 497, 16, 1, hud_palette_init_tbl1);
    load_image_rect(288, 498, 16, 1, hud_palette_init_tbl2);
    load_image_rect(288, 499, 16, 1, icon_init_tbl2);
}
