/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Test-only forwarders for retained head-address names.
 *
 * pad_main (0x80042C80) calls the detector by its hoisted-head address name
 * 0x80042D48 head name; the matched body is pad_type_check. The game link resolves
 * the head through pc/src/missing_funcs_gen.c (a naked jmp); the unit-test
 * link does not include that file, so this forwards the same call.
 *
 * The name is token-pasted on purpose: pc/tools/sync_symbol_names.py
 * replays landed renames whole-word over pc/, and would turn a literal
 * spelling into a second definition of pad_type_check.
 */
#include "types.h"

extern s32 pad_type_check(void);

#define HEAD_ADDR_NAME(addr) func_##addr

s32 HEAD_ADDR_NAME(80042D48)(void)
{
    return pad_type_check();
}
