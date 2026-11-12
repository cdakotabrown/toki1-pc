/* SPDX-License-Identifier: GPL-3.0-or-later */
/* class_slot.h -- the 68-byte class_w sprite slot at its PS1 width.
 *
 * class_w (0x801271B0) is 160 sprite slots of 68 bytes; sprite_slot_disp,
 * class_clear_n and the tpage cache (tpage_cache.h) all address it at
 * that stride.  Several matched initialisers declare their own view of a
 * slot with the three data handles as native pointers (`s32 *` / `u32 *`
 * / `void *` at +12, +16 and +52).  On the console those are 4 bytes; on
 * an LP64 host they are 8 bytes and 8-aligned, so the record grows to 88
 * bytes, every field after +12 moves, and `class_w[i]` strides 88 bytes:
 * each writer lands across two slots (the src/main/pc-port/mv_state.h /
 * bg_set.c doctrine -- the pointer width is a host property, not game
 * logic).
 *
 * This is the one PS1-width view the pc-port overrides of those
 * initialisers share.  Fields are named by their PS1 byte offset; the
 * handles are u32 and hold the PS1 address (the symbols are .set-aliased
 * to their console addresses in pc/src/ps1_addr_aliases.c, so storing
 * `(u32)(uintptr_t)&sym` is the value the console stores).  Overrides
 * that use it, with the matched field name each offset carries:
 *   basyo_init           src/main/pc-port/basyo_init.c
 *   icon_init            src/main/pc-port/icon_init.c
 *   config_screen_init   src/main/pc-port/config_screen_init.c
 *   parameter_show_init  src/main/pc-port/parameter_show_init.c
 *   schedule_mark_on     src/main/pc-port/schedule_mark_on.c
 * and, in the overlays (pc/src/overlay/<OV>/, substituted into the
 * overlay's module by pc/tools/generate_overlay_modules.py):
 *   date_spring_init     DATE      name_ent_grid_init   NAME_ENT
 *   option_screen_init   OPTION    tel_girl_status_init TEL
 *   taiiku_sprite_init, taiiku_plate_init, taiiku_sprite_init2  TAIIKU
 *
 * Layout cross-checked against the field map in config_screen_init's
 * matched Slot (the most complete one) and class_clear_n's stores.
 * Test: pc/tools/test_class_slot_width.py (each override vs the matched
 * function's output moved from host to -m32 member offsets).
 */
#ifndef PC_GAME_CLASS_SLOT_H
#define PC_GAME_CLASS_SLOT_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    u8  f0;             /*  0 */
    u8  f1;             /*  1 layer / mode */
    u8  f2;             /*  2 */
    u8  f3;             /*  3 flags */
    u8  f4;             /*  4 */
    u8  f5;             /*  5 */
    u8  f6;             /*  6 rate */
    u8  f7;             /*  7 brightness */
    s16 f8;             /*  8 tick */
    s16 f10;            /* 10 */
    u32 f12;            /* 12 PS1 handle: cel header table */
    u32 f16;            /* 16 PS1 handle: animation table */
    s16 f20;            /* 20 */
    s16 f22;            /* 22 anim */
    s16 f24;            /* 24 frame */
    s16 f26;            /* 26 scale x */
    s16 f28;            /* 28 scale y */
    s16 f30;            /* 30 */
    s32 f32;            /* 32 timing */
    u16 f36;            /* 36 x, 16.16 fraction */
    s16 f38;            /* 38 x, integer part */
    u16 f40;            /* 40 y, 16.16 fraction */
    s16 f42;            /* 42 y, integer part */
    u16 f44;            /* 44 vx low half */
    s16 f46;            /* 46 vx high half */
    s32 f48;            /* 48 vy */
    u32 f52;            /* 52 PS1 handle: tpage-set table */
    u32 f56;            /* 56 base / flags */
    s32 f60;            /* 60 */
    u8  f64;            /* 64 r */
    u8  f65;            /* 65 g */
    u8  f66;            /* 66 b */
    u8  f67;            /* 67 clut / tile frame */
} ClassSlot;            /* 68 */

/* C89 compile-time layout checks: the PS1 slot is 68 bytes, the three
 * handles sit at +12/+16/+52 and the 16-bit coordinate halves at +38/+42. */
typedef char class_slot_is_68[(sizeof(ClassSlot) == 68) ? 1 : -1];
typedef char class_slot_f12_at_12[(offsetof(ClassSlot, f12) == 12) ? 1 : -1];
typedef char class_slot_f16_at_16[(offsetof(ClassSlot, f16) == 16) ? 1 : -1];
typedef char class_slot_f24_at_24[(offsetof(ClassSlot, f24) == 24) ? 1 : -1];
typedef char class_slot_f38_at_38[(offsetof(ClassSlot, f38) == 38) ? 1 : -1];
typedef char class_slot_f42_at_42[(offsetof(ClassSlot, f42) == 42) ? 1 : -1];
typedef char class_slot_f46_at_46[(offsetof(ClassSlot, f46) == 46) ? 1 : -1];
typedef char class_slot_f52_at_52[(offsetof(ClassSlot, f52) == 52) ? 1 : -1];
typedef char class_slot_f56_at_56[(offsetof(ClassSlot, f56) == 56) ? 1 : -1];
typedef char class_slot_f67_at_67[(offsetof(ClassSlot, f67) == 67) ? 1 : -1];

/* A PS1 handle to a .set-aliased console symbol. */
#define CLASS_SLOT_HANDLE(sym) ((u32)(uintptr_t)(sym))

#endif /* PC_GAME_CLASS_SLOT_H */
