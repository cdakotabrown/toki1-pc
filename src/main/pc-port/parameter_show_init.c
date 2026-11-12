/*
 * parameter_show_init -- PC override of
 * src/main/matched/parameter_show_init.c (0x80063B78).
 *
 * Statement for statement the matched function; the divergence is the
 * RECORD WIDTH (the src/main/pc-port/bg_set.c / mv_state.h doctrine, same
 * defect as src/main/pc-port/basyo_init.c).  The matched SpriteEntry
 * declares the class_w slot's three data handles (ptrA, ptrB, ptrC) as
 * native pointers: 4 bytes on the console, 8 bytes (8-aligned) on LP64.
 * There the 68-byte slot becomes an 88-byte record, so `class_w[n]`
 * strides 88 bytes, every field after +12 moves, and the writes meant
 * for slots 32..59 (the stat-panel digit sprites)
 * landed across the wrong slots with their handles at host offsets
 * +16/+24/+56.  Measured by an LP64-vs-PS1 slot oracle: the matched code
 * leaves ~1,500 bytes of class_w different from the console image; this
 * override leaves none.
 *
 * Here every slot access goes through the PS1-width ClassSlot
 * (pc/include/game/class_slot.h), fields named by PS1 offset:
 *   mode -> f1, flags0 -> f2, flags1 -> f3, stripW -> f4,
 *   stripH -> f5, ptrA -> f12, ptrB -> f16, texPage -> f20,
 *   nodePtr -> f24, y1 -> f38, y2 -> f42, frameFlag -> f46,
 *   ptrC -> f52, flags2 -> f56, tileFrame -> f67.
 * The handle stores zero-extend the .set-aliased PS1 addresses
 * (pc/src/ps1_addr_aliases.c), the value the console stores.  The body
 * below is the matched body with exactly those renames (generated, then
 * diffed against the matched file: no other line differs).
 *
 * @DivergesFrom: matched parameter_show_init -- record field widths only.
 */
#include "types.h"
#include "class_slot.h"

extern ClassSlot class_w[];
extern u8 hud_palette_init_tbl2[];
extern u32 basyo_init_tbl1;
extern u32 basyo_init_tbl2;
extern u32 _sys_default_tpage_set_tbl2;
extern s16 basyo_init_tbl3;
extern s16 player_rec[];
extern s16 old_player_rec[];
extern s16 para_tai;
extern s16 old_para_tai;
extern s16 para_bun;
extern s16 old_para_bun;
extern s16 para_ri;
extern s16 old_para_ri;
extern s16 para_gei;
extern s16 old_para_gei;
extern s16 para_un;
extern s16 old_para_un;
extern s16 para_zat;
extern s16 old_para_zat;
extern s16 para_you;
extern s16 old_para_you;
extern s16 para_kon;
extern s16 old_para_kon;
extern s16 para_str;
extern s16 old_para_str;

extern void load_image_rect(s32, s32, s32, s32, void *);
extern void class_clear_n(s32);
extern s32 keta_n(s16, s32);
extern void memcpy(void *, void *, s32);

#define SET_DIGIT(slot, stat, pos, yy1, yy2) \
    digit = keta_n(stat, pos); class_w[slot].f24 = digit + 68; class_w[slot].f38 = yy1; class_w[slot].f42 = yy2

#define SET_ARROW(slot, stat, prev) \
    if (prev < stat) { \
        class_w[slot].f67 = class_w[slot + 1].f67 = class_w[slot + 2].f67 = 67; \
        class_w[slot].f46 = class_w[slot + 1].f46 = class_w[slot + 2].f46 = 1; \
    } else if (stat < prev) { \
        class_w[slot].f67 = class_w[slot + 1].f67 = class_w[slot + 2].f67 = 68; \
        class_w[slot].f46 = class_w[slot + 1].f46 = class_w[slot + 2].f46 = 1; \
    }

void parameter_show_init(void)
{
    s32 i;
    s32 j;
    s32 digit;
    s32 k;

    load_image_rect(288, 480, 16, 5, hud_palette_init_tbl2);
    for (i = 0; i < 28; i++) {
        class_clear_n(i + 32);
        class_w[i + 32].f1 = 11;
        class_w[i + 32].f2 = 64;
        class_w[i + 32].f56 = 0x40000000;
        class_w[i + 32].f3 = 132;
        class_w[i + 32].f12 = CLASS_SLOT_HANDLE(&basyo_init_tbl1);
        class_w[i + 32].f16 = CLASS_SLOT_HANDLE(&basyo_init_tbl2);
        class_w[i + 32].f52 = CLASS_SLOT_HANDLE(&_sys_default_tpage_set_tbl2);
        class_w[i + 32].f20 = basyo_init_tbl3;
        class_w[i + 32].f4 = 8;
        class_w[i + 32].f5 = 1;
    }
    SET_DIGIT(32, para_tai, 3, -73, -88);
    SET_DIGIT(33, para_tai, 2, -65, -88);
    SET_DIGIT(34, para_tai, 1, -57, -88);
    SET_ARROW(32, para_tai, old_para_tai);
    SET_DIGIT(35, para_bun, 3, 15, -88);
    SET_DIGIT(36, para_bun, 2, 23, -88);
    SET_DIGIT(37, para_bun, 1, 31, -88);
    SET_ARROW(35, para_bun, old_para_bun);
    SET_DIGIT(38, para_ri, 3, 103, -88);
    SET_DIGIT(39, para_ri, 2, 111, -88);
    SET_DIGIT(40, para_ri, 1, 119, -88);
    SET_ARROW(38, para_ri, old_para_ri);
    SET_DIGIT(41, para_gei, 3, -73, -74);
    SET_DIGIT(42, para_gei, 2, -65, -74);
    SET_DIGIT(43, para_gei, 1, -57, -74);
    SET_ARROW(41, para_gei, old_para_gei);
    SET_DIGIT(44, para_un, 3, 15, -74);
    SET_DIGIT(45, para_un, 2, 23, -74);
    SET_DIGIT(46, para_un, 1, 31, -74);
    SET_ARROW(44, para_un, old_para_un);
    SET_DIGIT(47, para_zat, 3, 103, -74);
    SET_DIGIT(48, para_zat, 2, 111, -74);
    SET_DIGIT(49, para_zat, 1, 119, -74);
    SET_ARROW(47, para_zat, old_para_zat);
    SET_DIGIT(50, para_you, 3, -73, -60);
    SET_DIGIT(51, para_you, 2, -65, -60);
    SET_DIGIT(52, para_you, 1, -57, -60);
    SET_ARROW(50, para_you, old_para_you);
    SET_DIGIT(53, para_kon, 3, 15, -60);
    SET_DIGIT(54, para_kon, 2, 23, -60);
    SET_DIGIT(55, para_kon, 1, 31, -60);
    SET_ARROW(53, para_kon, old_para_kon);
    SET_DIGIT(56, para_str, 3, 103, -60);
    SET_DIGIT(57, para_str, 2, 111, -60);
    SET_DIGIT(58, para_str, 1, 119, -60);
    SET_ARROW(56, para_str, old_para_str);
    class_w[59].f24 = 61;
    class_w[59].f38 = 0;
    class_w[59].f42 = -72;
    memcpy(old_player_rec, player_rec, 124);
}
