/* Native ABI adapter for matched/_mk_spr_packet.c, retail 0x8009E2F0.
 * @DivergesFrom: src/main/matched/_mk_spr_packet.c -- explicit argument
 * forwarding only. Retail tests params+24, calls _mk_xpndsp with the
 * incoming out/params/matrix registers or _mk_normsp with out/params.
 * The caller's fourth cell argument is retained, although neither worker
 * reads it. No return value is consumed by GsSortBg. Adapters keep the two
 * builder call shapes separate; metadata contains host pointers only.
 */
#include "bg_render.h"

static void bg_packet_normal(u8 *out, BgCellParams *params, MATRIX *matrix)
{
    (void)matrix;
    _mk_normsp(out, (u8 *)params);
}

static void bg_packet_transformed(u8 *out, BgCellParams *params, MATRIX *matrix)
{
    _mk_xpndsp(out, (u8 *)params, matrix);
}

const BgPacketHandlerInfo bg_packet_handlers[BG_PACKET_HANDLER_COUNT] = {
    { "_mk_normsp", BG_PACKET_PAYLOAD_WORDS, 0, bg_packet_normal },
    { "_mk_xpndsp", BG_PACKET_PAYLOAD_WORDS, 1, bg_packet_transformed }
};

void _mk_spr_packet(u8 *out, BgCellParams *params, MATRIX *matrix,
                    GsCELL *cell)
{
    u32 kind;

    (void)cell;
    kind = params->useMatrix != 0 ? BG_PACKET_TRANSFORMED : BG_PACKET_NORMAL;
    bg_packet_handlers[kind].emit(out, params, matrix);
}
