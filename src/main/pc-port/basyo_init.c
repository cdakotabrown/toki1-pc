/*
 * basyo_init -- rebuild the place-name row list (PC override of
 * src/main/matched/basyo_init.c).
 *
 * Statement for statement the matched function; the divergence is the
 * RECORD WIDTH (the src/main/pc-port/bg_set.c / mv_state.h doctrine).
 * The matched Rec declares the class_w slot's header, cel-table and
 * animation handles as `s32 *f12, *f16, *f52`: 4 bytes on the console,
 * 8 bytes (8-aligned) on LP64.  There the 68-byte class_w slot became an
 * 88-byte record starting at class_w+208, so rec[i] no longer lay on
 * slot i+3: every row after the first was written across two slots with
 * its handles at the wrong offsets.  Measured in scene 54 (ETC
 * basyo_load_init -> basyo_init): slot 9 (class_w+612 = rec[4]+52 in the
 * host layout) received rec[4].f42 = -145 (0xff6f) at +2, so its flags
 * byte +3 read 0xff; rec[4].f52 (0x800CFC30) at +12 where sprite_slot_disp
 * reads the header, f56 (0x40000000) at +20, and rec[5].f12/f16
 * (0x800CFC5C/0x800CFF14) at +52/+60; the resulting cel count 32704
 * overran the packet buffer in _make_packet.
 * The old 0x800C0000-0x800E0000 PC_MESH_PTR_OK window in
 * sprite_slot_disp happened to reject those slots.
 *
 * Here the record is the shared PS1-width slot (ClassSlot,
 * pc/include/game/class_slot.h; matched Rec field fN is ClassSlot fN)
 * carrying the PS1 handles as u32 at the console's exact offsets; the
 * three handle stores zero-extend the .set-aliased PS1 addresses
 * (pc/src/ps1_addr_aliases.c).  The row string argument is likewise a
 * 32-bit handle, as in the K&R matched definition (`s32 arg0`).
 *
 * @DivergesFrom: matched basyo_init -- record field widths only.
 */
#include "types.h"
#include <stdint.h>
#include <string.h>
#include "class_slot.h"

typedef struct {
    u8 pad0[204];
    ClassSlot rec[16];
} Obj;

/* C89 compile-time layout check: rec[0] is class_w slot 3 (ClassSlot is
 * checked at 68 bytes in class_slot.h). */
typedef char basyo_init_rec_at_204[(sizeof(Obj) == 204 + 16 * 68) ? 1 : -1];

extern Obj class_w;
extern s16 basyo_init_tbl0;
extern s16 mes_base_y;
extern s32 basyo_init_tbl1;
extern s32 basyo_init_tbl2;
extern s32 _sys_default_tpage_set_tbl2;
extern s16 basyo_init_tbl3;

extern void class_clear_n();
extern void set_kanji_string();

void basyo_init(s32 arg0)
{
    s32 i;

    mes_base_y = -64;
    for (i = 0; i < basyo_init_tbl0 + 2; i++) {
        class_clear_n(i + 3);
    }
    set_kanji_string(-206, -152, 0, arg0, 0);
    basyo_init_tbl0 = strlen((const char *)(uintptr_t)(u32)arg0) / 2;
    for (i = 0; i < basyo_init_tbl0 + 2; i++) {
        class_clear_n(i + 3);
        class_w.rec[i].f1 = 2;
        class_w.rec[i].f2 = 64;
        class_w.rec[i].f56 = 0x40000000;
        class_w.rec[i].f3 = 164;
        class_w.rec[i].f12 = CLASS_SLOT_HANDLE(&basyo_init_tbl1);
        class_w.rec[i].f16 = CLASS_SLOT_HANDLE(&basyo_init_tbl2);
        class_w.rec[i].f52 = CLASS_SLOT_HANDLE(&_sys_default_tpage_set_tbl2);
        class_w.rec[i].f20 = basyo_init_tbl3;
        class_w.rec[i].f4 = 8;
        class_w.rec[i].f5 = 1;
        class_w.rec[i].f24 = 114;
        class_w.rec[i].f38 = (basyo_init_tbl0 - i) * 14 + mes_base_y - 135;
        class_w.rec[i].f42 = mes_base_y - 81;
    }
    class_w.rec[basyo_init_tbl0 + 1].f24 = 113;
    class_w.rec[basyo_init_tbl0 + 1].f38 += 2;
    class_w.rec[0].f24 = 115;
    class_w.rec[0].f38 -= 2;
}
