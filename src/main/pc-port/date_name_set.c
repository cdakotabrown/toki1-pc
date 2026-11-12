/*
 * date_name_set -- PC override of src/main/matched/date_name_set.c
 * (main binary).
 *
 * Statement for statement the matched function; the divergence is the
 * table WIDTH.  date_name_set_tbl0 (0x800B9AE0) is 14 4-byte PS1 pointers
 * to the girls' name strings (0x800B9A64, 0x800B9A6C, ...).  The matched C
 * declares it `void *[]`, so entry i is read 8 bytes wide at stride 8: two
 * entries joined (girl 0: 0x800B9A6C800B9A64), and the PC strcpy
 * trampoline skipped the copy ("[BIOS] strcpy: 0x800b9a6c800b9a64 (1
 * bytes) is not mapped on the PC; skipped" when the date-day holiday
 * screen set up "today is a date with <name>"), leaving g_name stale.
 * Here the table is read as u32 and widened, as the console reads it.
 *
 * @DivergesFrom: matched date_name_set -- table read width only
 * (pc/tools/audit_ps1_wide_access.py WIDE-INDEX).
 */
#include <stdint.h>
#include "types.h"
/* strcpy -> the BIOS trampoline, as in the matched TU (pc-port objects do
 * not get the force-include) */
#include "sony_bios_alias.h"

extern void strcpy(void* a0, void* a1);
extern void get_p_name(void* a0, s32 a1);
extern void get_g_name(void* a0, s32 a1);

extern u8 p_name[];
extern u8 pp_name[];
extern u8 p_name2[];
extern u8 g_name[];
extern u8 g_name2[];

extern u8 p_sei;
extern u8 p_mei;

extern u32 date_name_set_tbl0[];    /* PS1 string pointers */

void date_name_set(s32 arg0) {
    s32 unused; /* original local at sp+0x2c; keeps tmp at sp+0x28 */
    u32 tmp;
    (void)&arg0;
    strcpy(p_name, &p_sei);
    strcpy(pp_name, &p_mei);
    tmp = (u8)arg0;
    if (tmp < 14U) {
        get_p_name(p_name2, (s32)tmp);
        strcpy(g_name, (void *)(uintptr_t)date_name_set_tbl0[*(u8*)&arg0]);
        get_g_name(g_name2, (s32)tmp);
    }
}
