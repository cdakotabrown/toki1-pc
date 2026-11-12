/*
 * serifu_run_end -- PC override of src/main/matched/serifu_run_end.c
 * (0x80085C00, head absorbed).
 *
 * Statement for statement the matched function; the divergence is the
 * pointer WIDTH.  now_s / now_s_in are 4-byte PS1 cells at 0x800D0634 /
 * 0x800D0638, followed by ss_s (0x800D063C).  The matched C declares them
 * `u8 *`, 8 bytes on LP64: the now_s store wrote now_s_in's word and the
 * now_s_in store wrote ss_s's (each re-stored afterwards in the build's
 * store order, so nothing is lost today), and every reader -- serifu_run,
 * serifu_run_step and the pc-port serifu_main -- reads them as 32-bit
 * values.  Here they are
 * u32 and hold the PS1 address of now_serifu / now_serifu_in, the value
 * serifu_run_step stores.
 *
 * @DivergesFrom: matched serifu_run_end -- pointer store width only
 * (pc/tools/audit_ps1_wide_access.py).
 */
#include <stdint.h>
#include "types.h"

extern u8   now_serifu[];
extern u8   now_serifu_in[];
extern s32  ss_serifu;
extern s32  ss_serifu_kind;
extern s32  sss_sound;
extern u32  now_s;        /* PS1 pointer */
extern u32  now_s_in;     /* PS1 pointer */
extern s32  ss_s;
extern s32  ss_s_kind;
extern s32  sss_so;

extern void serifu_main(s32 a, s32 b, s32 c);

void serifu_run_end(void)
{
    now_s = (u32)(uintptr_t)now_serifu;
    now_s_in = (u32)(uintptr_t)now_serifu_in;
    ss_s = ss_serifu;
    ss_s_kind = ss_serifu_kind;
    sss_so = sss_sound;
    serifu_main(255, 1, 0);
}
