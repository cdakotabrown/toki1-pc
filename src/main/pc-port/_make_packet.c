/*
 * _make_packet -- PC pointer-width adapter for matched/_make_packet.c.
 * Statement-for-statement matched body; only host pointer width diverges.
 * GsOUT_PACKET_P is a four-byte PS1 handle, never a native pointer cell.
 * Ordering-table records remain 12 bytes, with org at +4 and offset at +8.
 * Handles are zero-extended at use sites; packet links retain PS1 width.
 * See mv_state.h for the host-adaptation doctrine.
 */
#include "types.h"
#ifdef PC_PORT
#include "sony_bios_alias.h"   /* printf -> the BIOS A(0x3F) trampoline */
#endif

extern s32 printf(const char *format, ...);
extern char _make_packet_tbl0[];

typedef struct {
    s32 field_0;
    u32 field_4;
    s32 field_8;
} AutoCtrl;

void *_make_packet(s32 *node, AutoCtrl *ctrl, s32 panval, s32 pan)
{
    s32 idx;
    s32 *entry;
    s32 prev;
    s32 pmask;

    idx = (s32)(u16)panval - ctrl->field_8;
    if (idx < 0) {
        printf(_make_packet_tbl0);
    }
    entry = (s32 *)(unsigned long)ctrl->field_4 + idx;
    prev = *entry;
    pmask = pan & 0xFF;
    node[0] = prev;
    *((u8 *)node + 3) = (u8)pan;
    *entry = (s32)(unsigned long)node;
    *((u8 *)entry + 3) = 0;
    return &node[pmask + 1];
}
