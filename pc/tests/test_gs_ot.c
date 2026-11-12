/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PS1-width OT and cursor regression tests, no game assets required.
 * Include the adapters under private names so tests use their exact bodies
 * without pulling the game executable (or its absolute data symbols) in.
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "types.h"

#define printf gs_ot_diagnostic
#define _make_packet test_make_packet
#define _make_packet_tbl0 test_packet_message
#include "../../src/main/pc-port/_make_packet.c"
#undef printf
#define GsSortFastSprite test_sort_fast_sprite
#define GsOUT_PACKET_P test_packet_cursor
#define GsORGOFSX test_org_x
#define GsORGOFSY test_org_y
#include "../../src/main/pc-port/GsSortFastSprite.c"

static int diagnostics;

#define printf gs_tmd_diagnostic
#define libgs_gpu_code_unassigned_msg test_tmd_message
#define GsLinkObject4 test_link_object4
#include "../../src/main/pc-port/GsLinkObject4.c"
#undef printf

char test_tmd_message[] = "unknown primitive";
s32 gs_tmd_diagnostic(const char *message, ...)
{
    (void)message;
    diagnostics++;
    return 0;
}

u32 test_packet_cursor;
u16 test_org_x, test_org_y;
char test_packet_message[] = "negative OT index";

s32 gs_ot_diagnostic(const char *message, ...)
{
    (void)message;
    diagnostics++;
    return 0;
}

#define CHECK(value) do { if (!(value)) { result = __LINE__; goto done; } } while (0)

int test_gs_ot_width(void)
{
    u32 *memory;
    u32 *packet;
    u32 *entries;
    AutoCtrl ctrl;
    SlotHdr slot;
    PrimSpec sprite;
    u32 object[6];
    u32 *tmd;
    u8 *primitive;
    int result;

    result = 0;
    /* Bit 31 set: a signed widening must fail this test. Never replace
     * an existing mapping (the game executable is not linked here).
     */
    memory = mmap((void *)0x81000000UL, 4096, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (memory == MAP_FAILED) {
        return __LINE__;
    }
    packet = memory;
    entries = memory + 128;
    CHECK(sizeof(AutoCtrl) == 12 && offsetof(AutoCtrl, field_4) == 4);
    CHECK(offsetof(AutoCtrl, field_8) == 8);
    CHECK(sizeof(SlotHdr) == 12 && offsetof(SlotHdr, p04) == 4);
    CHECK(offsetof(SlotHdr, v08) == 8);

    ctrl.field_0 = 3;
    ctrl.field_4 = (u32)(unsigned long)entries;
    ctrl.field_8 = 2;
    entries[1] = 0x12abcdef;
    diagnostics = 0;
    CHECK(test_make_packet((s32 *)packet, &ctrl, 3, 5) == packet + 6);
    CHECK(packet[0] == 0x05abcdef);
    CHECK(entries[1] == ((u32)(unsigned long)packet & 0xffffff));
    CHECK(diagnostics == 0);

    /* u16 priority truncation, low-byte length, and negative-index warning
     * all retain the matched behavior (including the splice after warning).
     */
    CHECK(test_make_packet((s32 *)packet, &ctrl, 0x10003, 0x109)
          == packet + 10);
    ctrl.field_4 = (u32)(unsigned long)(entries + 1);
    CHECK(test_make_packet((s32 *)packet, &ctrl, 1, 5) == packet + 6);
    CHECK(diagnostics == 1);

    memset(&sprite, 0, sizeof(sprite));
    memset(&slot, 0, sizeof(slot));
    sprite.u08 = 16;
    sprite.u0A = 8;
    slot.p04 = (u32)(unsigned long)entries;
    slot.v08 = 2;
    entries[1] = 0x00ffffff;
    test_packet_cursor = (u32)(unsigned long)packet;
    CHECK(sizeof(test_packet_cursor) == 4);
    test_sort_fast_sprite(&sprite, &slot, 3);
    CHECK(packet[0] == 0x05ffffff);
    CHECK(entries[1] == ((u32)(unsigned long)packet & 0xffffff));
    CHECK(test_packet_cursor == (u32)(unsigned long)(packet + 6));
    CHECK(packet[5] == (16 | (8 << 16)));
    sprite.flags = 0x80000000;
    test_sort_fast_sprite(&sprite, &slot, 3);
    CHECK(test_packet_cursor == (u32)(unsigned long)(packet + 6));
    /* GsLinkObject4 must write tmd at +8, not native pointer slot +16.
     * Guard the following PS1 object and count a real primitive run.
     */
    memset(object, 0xa5, sizeof(object));
    tmd = memory + 256;
    primitive = (u8 *)(memory + 512);
    tmd[4] = (u32)(unsigned long)primitive;
    tmd[5] = 1;
    primitive[3] = 0x20;
    test_link_object4((u8 *)tmd, object, 0);
    CHECK(object[2] == (u32)(unsigned long)tmd);
    CHECK(object[1] == 0xa5a5a5a5 && object[3] == 0xa5a5a5a5);
    CHECK(object[4] == 0xa5a5a5a5 && object[5] == 0xa5a5a5a5);
    CHECK(*(u16 *)primitive == 1 && primitive[3] == 0x20);
done:
    munmap(memory, 4096);
    return result;
}
