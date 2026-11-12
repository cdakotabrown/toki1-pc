/* SPDX-License-Identifier: GPL-3.0-or-later */
/* missing_globals_manual.c -- hand-corrected PS1 globals for the PC port.
 *
 * pc/tools/gen_missing_globals.py derives most unresolved D_xxxxxxxx globals
 * mechanically (type from the extern declarations, initial bytes from the
 * retail image).  It cannot do anything sensible for globals whose retail
 * value is a POINTER INTO PS1 HARDWARE OR RAM, because those addresses mean
 * nothing in a native process -- it emits NULL and notes the address.  The
 * corrections live here so a regeneration of the generated file cannot lose
 * them.
 *
 * Keep this file and pc/src/missing_globals_gen.c disjoint: a symbol defined
 * here must be deleted from the generated file.
 */
#include "types.h"

/* libapi_rcnt_regs_ptr -- retail value 0x1F801100, the PS1 root-counter (timer)
 * register block: three counters, 16 bytes apart.  SetRCnt /
 * GetRCnt / ResetRCnt index it as `libapi_rcnt_regs_ptr[idx * 8]` for
 * idx 0..2, i.e. s16 stride 8 == the 16-byte hardware stride, so they are
 * reading and clearing counter values.
 *
 * The PC port does not emulate the root counters (timing comes from the SDL2
 * frame loop), so back the pointer with a zeroed 3 x 16-byte block.  Reads
 * return 0 and writes are absorbed, which is what those three functions
 * expect from a stopped counter -- and, unlike a NULL pointer, does not
 * segfault on the very first sound call.  Previously this global was absent
 * from the build entirely.
 *
 * TODO: if root-counter values ever matter, back this with a real HAL timer
 * block instead of dead memory.
 */
static s16 psx_root_counters[3 * 8];
s16 *libapi_rcnt_regs_ptr = psx_root_counters;

/* D_801399C0 -- KANGEI's kangei_sub_dispatch jump table (22 fn pointers,
 * 88 bytes).  The decompiled dispatcher copies the table and calls
 * `table.ptrs[now_sub_sub_step](128)`; the generated stubs defined this as
 * `s32 D_801399C0;`, so every KANGEI scene called a NULL pointer
 * (scene_matrix 95/96, 2026-10-17).
 *
 * Retail values read from pc/assets/raw/CDROM/EXEDIR/KANGEI.EXN at
 * offset 0x79C0 (= 0x801399C0 - 0x80132000).  The EXN ships these pre-baked
 * as absolute PS1 addresses (no loader relocation), so each entry maps
 * 1:1 to a PC symbol: 0x8013xxxx through the generated overlay_symtab,
 * 0x800xxxxx through config/level3_functions.json.  This is the
 * "pointer into PS1 RAM" class described at the top of this file -- the
 * long-term fix is the 2 MB RAM model with load-time relocation.
 */
extern void KANGEI_kangei_sub_init();
extern void KANGEI_kangei_girl_device_load();
extern void KANGEI_kangei_girl_device_wait();
extern void KANGEI_kangei_data_load();
extern void KANGEI_kangei_bukatu_serifu_set();
extern void KANGEI_kangei_bustup_read();
extern void KANGEI_device_cmd_send200_step();
extern void KANGEI_kangei_zyotai_face_set();
extern void KANGEI_kangei_exit();
extern void cd_wait_timeout();
extern void dec_bg_load_check();
extern void indexed_toggle_step();
extern void zyotai_sprite_set_chk();
extern void wipe_main();
extern void basyo_string_step();
extern void fade_up_main();
extern void serifu_run();
extern void fade_down_main();
extern void wipe_main2();
extern void game_reset();
void *D_801399C0[22] = {
    (void *)KANGEI_kangei_sub_init,        /* [ 0] 0x80132514 */
    (void *)KANGEI_kangei_girl_device_load,/* [ 1] 0x801323A8 */
    (void *)KANGEI_kangei_girl_device_wait,/* [ 2] 0x80132430 */
    (void *)KANGEI_kangei_data_load,       /* [ 3] 0x801324D8 */
    (void *)cd_wait_timeout,               /* [ 4] 0x8007C814 */
    (void *)KANGEI_kangei_bukatu_serifu_set,/* [ 5] 0x801327DC */
    (void *)cd_wait_timeout,               /* [ 6] 0x8007C814 */
    (void *)dec_bg_load_check,             /* [ 7] 0x8007EE3C */
    (void *)indexed_toggle_step,           /* [ 8] 0x8007EE14 */
    (void *)KANGEI_kangei_bustup_read,     /* [ 9] 0x80132C2C */
    (void *)cd_wait_timeout,               /* [10] 0x8007C814 */
    (void *)zyotai_sprite_set_chk,         /* [11] 0x8008367C */
    (void *)wipe_main,                     /* [12] 0x8007C934 */
    (void *)basyo_string_step,             /* [13] 0x8007EDAC */
    (void *)KANGEI_device_cmd_send200_step,/* [14] 0x801324B0 */
    (void *)KANGEI_kangei_zyotai_face_set, /* [15] 0x80132354 */
    (void *)fade_up_main,                  /* [16] 0x8007E4D4 */
    (void *)serifu_run,                    /* [17] 0x80085C78 */
    (void *)fade_down_main,                /* [18] 0x8007E6E4 */
    (void *)wipe_main2,                    /* [19] 0x8007D9BC */
    (void *)KANGEI_kangei_exit,            /* [20] 0x80132290 */
    (void *)game_reset,                    /* [21] 0x80046840 */
};

/* D_80134AF0 -- OMIMAI's omimai_main0 jump table (25 fn pointers, 100 bytes).
 * The same table-copy dispatch as KANGEI's D_801399C0 above (the stubs
 * defined it `s32 D_80134AF0;` = zero, so every OMIMAI scene called NULL --
 * scene_matrix 97/98).  Retail values from pc/assets/raw/CDROM/EXEDIR/
 * OMIMAI.EXN at offset 0x2AF0 (= 0x80134AF0 - 0x80132000), pre-baked
 * absolute PS1 addresses; translated via overlay_symtab (0x8013xxxx) and
 * the registry (0x800xxxxx).  Same fix template as D_801399C0.
 */
extern void OMIMAI_omimai_text_init0();
extern void OMIMAI_omimai_mother_init1();
extern void OMIMAI_omimai_girl_load();
extern void OMIMAI_omimai_load_wait();
extern void OMIMAI_omimai_serifu_load();
extern void OMIMAI_omimai_bg_event_step();
extern void OMIMAI_omimai_scene_init();
extern void OMIMAI_device_cmd_send200_step();
extern void OMIMAI_omimai_birthday_step();
extern void OMIMAI_omimai_schedule_return();
extern void girl_message_load();
extern void fade_up_main();
extern void fade_down_main();
extern void wipe_main2();
void *D_80134AF0[25] = {
    (void *)OMIMAI_omimai_text_init0,        /* [ 0] 0x80132630 */
    (void *)OMIMAI_omimai_mother_init1,      /* [ 1] 0x801327BC */
    (void *)OMIMAI_omimai_girl_load,         /* [ 2] 0x801324BC */
    (void *)OMIMAI_omimai_load_wait,         /* [ 3] 0x80132544 */
    (void *)OMIMAI_omimai_serifu_load,       /* [ 4] 0x801325EC */
    (void *)cd_wait_timeout,                 /* [ 5] 0x8007C814 */
    (void *)serifu_run,                      /* [ 6] 0x80085C78 */
    (void *)OMIMAI_omimai_bg_event_step,     /* [ 7] 0x80132788 */
    (void *)cd_wait_timeout,                 /* [ 8] 0x8007C814 */
    (void *)dec_bg_load_check,               /* [ 9] 0x8007EE3C */
    (void *)indexed_toggle_step,             /* [10] 0x8007EE14 */
    (void *)OMIMAI_omimai_scene_init,        /* [11] 0x80132670 */
    (void *)girl_message_load,               /* [12] 0x80084358 */
    (void *)cd_wait_timeout,                 /* [13] 0x8007C814 */
    (void *)zyotai_sprite_set_chk,           /* [14] 0x8008367C */
    (void *)wipe_main,                       /* [15] 0x8007C934 */
    (void *)basyo_string_step,               /* [16] 0x8007EDAC */
    (void *)OMIMAI_device_cmd_send200_step,  /* [17] 0x801325C4 */
    (void *)fade_up_main,                    /* [18] 0x8007E4D4 */
    (void *)serifu_run,                      /* [19] 0x80085C78 */
    (void *)fade_down_main,                  /* [20] 0x8007E6E4 */
    (void *)OMIMAI_omimai_birthday_step,     /* [21] 0x80132398 */
    (void *)wipe_main2,                      /* [22] 0x8007D9BC */
    (void *)OMIMAI_omimai_schedule_return,   /* [23] 0x801323F8 */
    (void *)game_reset,                      /* [24] 0x80046840 */
};
/* OLH menu label strings + the D_80139E60 pointer table (scene 58's crash).
 * Content from pc/assets/raw/CDROM/EXEDIR/OLH.EXN at 0x7660..0x7E5F (Shift-JIS
 * labels: system settings / memory card / file select / ...). The stubs defined
 * the table 's32 D_80139E60;' -- a 4-byte scalar for a 32-byte pointer table --
 * so olh_init's walk read garbage past it, and the label strings had no storage
 * at all. Initialized-data class; the 2 MB RAM model absorbs this wholesale:
 * every one of these names resolves to its PS1 address (pc/ld/psx_globals.syms,
 * an absolute symbol in both the Linux and the Windows link), where OLH.EXN's
 * own bytes are when the overlay is loaded.  These host definitions are
 * therefore never read, and they no longer copy the EXN's label bytes (the
 * binary carries no retail data; pc/tools/audit_retail_bytes.py). */
u8 D_80139660[14];
u8 D_80139760[16];
u8 D_80139860[18];
u8 D_80139960[14];
u8 D_80139A60[10];
u8 D_80139B60[12];
u8 D_80139C60[10];
u8 D_80139D60[14];

void *D_80139E60[8] = {
    D_80139660,
    D_80139760,
    D_80139860,
    D_80139960,
    D_80139A60,
    D_80139B60,
    D_80139C60,
    D_80139D60,
};
