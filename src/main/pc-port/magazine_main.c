/*
 * magazine_main -- PC override of src/main/matched/magazine_main.c (0x80077EBC).
 *
 * Statement for statement the matched function; the divergence is the
 * table WIDTH.  magazine_main_tbl0 (0x800BD1F8) is a table of 4-byte PS1 message
 * pointers; the matched C declares it `u32 *magazine_main_tbl0[4]`, so entry i is read 8
 * bytes wide at stride 8 (words 2i and 2i+1 joined: a non-canonical
 * source).  Here each entry is read as u32 and widened (PS1_PTR), as the
 * console reads it.  Found by pc/tools/audit_ps1_wide_access.py (WIDE-INDEX
 * through a lea'd base); not entered by the scripted playthroughs yet.
 *
 * @DivergesFrom: matched magazine_main -- table read width only.
 */
#include <stdint.h>
/* string/BIOS calls -> the BIOS trampolines, as in the matched TU (pc-port
 * objects do not get the force-include) */
#include "sony_bios_alias.h"
#include "types.h"

extern u8 step_mode;
extern u8 date2_menu_select_tbl1;
extern u32 magazine_main_tbl0[];    /* PS1 message pointers */
#define PS1_PTR(t, i) ((void *)(uintptr_t)(t)[i])
extern u8 p_sei[128];
extern s32 magazine_base_show(void);
extern void xa_wait(void);
extern void sub_sub_step_up(void);
extern void k_sub_reset(void);
extern void k_sub_disp_start(s32);
extern void gnsx(u8 *);
extern s32 magazine_slot_get(void);
extern void sndisp(u32 *, s32, s32);

void magazine_main(void) {
    s32 mode;

    mode = magazine_base_show();
    mode = step_mode;

    switch (mode) {
    case 0:
        xa_wait();
        break;
    case 1:
        if (date2_menu_select_tbl1 != 0) {
            step_mode++;
            break;
        }
        sub_sub_step_up();
        break;
    case 2:
        k_sub_reset();
        k_sub_disp_start(0);
        gnsx(p_sei);
        sndisp(PS1_PTR(magazine_main_tbl0, magazine_slot_get()), 0, 31);
        step_mode++;
        break;
    case 3:
        xa_wait();
        break;
    case 4:
    default:
        sub_sub_step_up();
        break;
    }
}
