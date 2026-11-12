/* SPDX-License-Identifier: GPL-3.0-or-later */
/* retail_rodata_placeholders.c -- PC port
 *
 * The memory-card dispatcher at 0x8007F08C and the transition step at
 * 0x8007C968 read their on-screen message text straight out of retail
 * .rodata (0x800C87C0 and up).  Those bytes are Shift-JIS game text: they
 * are COPYRIGHTED ASSETS and must never be committed to this repository,
 * so they cannot simply be transcribed here.
 *
 * These are therefore EMPTY placeholders whose only job is to let the port
 * link.  The consequence is visible and deliberate: memory-card prompts
 * render with blank text rather than with fabricated stand-in strings.  A
 * blank prompt is an honest missing asset; an invented one would be the
 * asset cheating pc/README.md forbids.
 *
 * PROPER FIX: load this block from the user's own disc image at runtime,
 * the way the port already sources its other extracted assets, and delete
 * this file.  Each symbol below is a NUL-terminated string at its named
 * address in SLPS_000.65.
 */
#include "types.h"

u8 srn_vram_set_tbl0[1] = {0};
u8 D_800C87C0[1] = {0};
u8 D_800C87C4[1] = {0};
u8 D_800C87C8[1] = {0};
u8 D_800C87CC[1] = {0};
u8 D_800C87D0[1] = {0};
u8 D_800C87D4[1] = {0};
u8 D_800C87D8[1] = {0};
u8 D_800C87DC[1] = {0};
u8 D_800C87E0[1] = {0};
u8 D_800C87E4[1] = {0};
u8 D_800C87E8[1] = {0};
u8 D_800C87EC[1] = {0};
u8 D_800C87F0[1] = {0};
u8 D_800C87F4[1] = {0};
u8 D_800C87F8[1] = {0};
u8 D_800C87FC[1] = {0};
u8 D_800C8800[1] = {0};
u8 D_800C880C[1] = {0};
u8 D_800C8810[1] = {0};
u8 D_800C8814[1] = {0};
u8 D_800C881C[1] = {0};
u8 D_800C8824[1] = {0};
u8 D_800C882C[1] = {0};
u8 D_800C8838[1] = {0};
u8 D_800C883C[1] = {0};
u8 D_800C884C[1] = {0};
u8 D_800C8854[1] = {0};
u8 D_800C8858[1] = {0};
u8 D_800C885C[1] = {0};
u8 D_800C8868[1] = {0};
u8 D_800C886C[1] = {0};
u8 D_800C8870[1] = {0};
u8 D_800C8874[1] = {0};
u8 D_800C8878[1] = {0};
u8 D_800C887C[1] = {0};
u8 D_800C8880[1] = {0};
u8 D_800C8884[1] = {0};
u8 D_800C8888[1] = {0};
u8 D_800C8890[1] = {0};
u8 D_800C8894[1] = {0};
u8 D_800C8898[1] = {0};
u8 D_800C889C[1] = {0};
u8 D_800C88A0[1] = {0};
u8 D_800C88A4[1] = {0};
u8 D_800C88A8[1] = {0};
u8 D_800C88AC[1] = {0};
u8 D_800C88B0[1] = {0};
u8 D_800C88B4[1] = {0};
u8 D_800C88B8[1] = {0};
u8 D_800C88BC[1] = {0};
u8 D_800C88C0[1] = {0};
u8 D_800C88C4[1] = {0};
u8 D_800C88C8[1] = {0};
u8 D_800C88CC[1] = {0};
u8 D_800C88D0[1] = {0};
u8 D_800C88D4[1] = {0};
u8 D_800C88D8[1] = {0};
u8 D_800C88DC[1] = {0};
u8 D_800C88E0[1] = {0};
u8 D_800C88E4[1] = {0};
u8 D_800C88E8[1] = {0};
u8 D_800C88EC[1] = {0};
u8 D_800C88F0[1] = {0};
u8 D_800C88F4[1] = {0};
u8 D_800C88F8[1] = {0};
u8 D_800C88FC[1] = {0};
u8 D_800C8900[1] = {0};
u8 D_800C8904[1] = {0};
u8 D_800C8908[1] = {0};
u8 D_800C890C[1] = {0};
u8 D_800C8910[1] = {0};
u8 D_800C8914[1] = {0};
u8 D_800C8918[1] = {0};
u8 D_800C891C[1] = {0};
u8 D_800C8920[1] = {0};
u8 D_800C8924[1] = {0};
u8 D_800C8928[1] = {0};
u8 D_800C892C[1] = {0};
u8 D_800C8930[1] = {0};
u8 D_800C8934[1] = {0};
u8 D_800C8938[1] = {0};
u8 D_800C893C[1] = {0};
u8 D_800C8940[1] = {0};
u8 D_800C8948[1] = {0};
u8 D_800C8950[1] = {0};
u8 D_800C8954[1] = {0};
u8 D_800C8958[1] = {0};
u8 D_800C895C[1] = {0};
u8 D_800C8964[1] = {0};
u8 D_800C896C[1] = {0};
u8 D_800C8970[1] = {0};
u8 D_800C8974[1] = {0};
u8 D_800C8978[1] = {0};
u8 D_800C8980[1] = {0};
u8 D_800C8988[1] = {0};
u8 D_800C898C[1] = {0};
u8 D_800C8990[1] = {0};
u8 D_800C8994[1] = {0};
u8 D_800C899C[1] = {0};
u8 D_800C89A4[1] = {0};
u8 D_800C89A8[1] = {0};
u8 D_800C89AC[1] = {0};
u8 D_800C89B0[1] = {0};
u8 D_800C89B4[1] = {0};
u8 D_800C89B8[1] = {0};
u8 D_800C89BC[1] = {0};
u8 D_800C89C0[1] = {0};
u8 D_800C89C4[1] = {0};
u8 D_800C89C8[1] = {0};
u8 D_800C89CC[1] = {0};
u8 D_800C89D0[1] = {0};
u8 D_800C89D4[1] = {0};
u8 D_800C89D8[1] = {0};
u8 D_800C89DC[1] = {0};
u8 D_800C89E0[1] = {0};
u8 D_800C89E4[1] = {0};
u8 D_800EF79F = 0;
