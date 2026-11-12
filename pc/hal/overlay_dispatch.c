/* SPDX-License-Identifier: GPL-3.0-or-later */
/* overlay_dispatch.c — Maps scene IDs to EXN overlay names and functions
 *
 * On PS1, the game loads one EXN overlay at a time based on the current
 * gameplay context. On PC, all overlay code is compiled statically.
 * This table maps scene IDs to the correct overlay entry function.
 *
 * SOURCE OF TRUTH (2026-10-17): the retail loader switch in
 * src/main/matched/overlay_load_init.c (the `switch (charId + 0U)` setting
 * newA2 = LBA, byte-verified matched C).  Scene ids not in any arm load
 * TACO (the retail `default` arm) -- including 18 and 64.  EVENT.EXN and
 * GYOZI.EXN have NO arm and are never loaded (stale builds on the disc);
 * do not add rows for them.
 * An earlier version of this table was written from guessed scene
 * semantics and disagreed with retail almost everywhere; if you touch it,
 * re-derive from the loader switch, not from descriptions.
 */
#include "hal.h"
#include "overlay_entries.h"
#include <stdio.h>

extern u8 now_step;
extern void overlay_stub_handler(void);
extern void debug_log(const char *fmt, ...);

/* Scene-to-overlay mapping */
typedef struct {
    u8 scene_min;
    u8 scene_max;
    const char *overlay_name;
    const char *description;
    void (*handler)(void);
} OverlayMapping;

static const OverlayMapping g_overlay_map[] = {
    /* retail: NAME_ENT (LBA 0x95EA) -- 17, 19, 20, 21 (explicitly NOT 18) */
    { 17,  17, "NAME_ENT", "Name entry screen",             overlay_NAME_ENT_entry},
    { 19,  21, "NAME_ENT", "Name entry screen",             overlay_NAME_ENT_entry},

    /* retail: OPTION (0x976A) */
    { 32,  34, "OPTION",   "Options menu",                  overlay_OPTION_entry},

    /* retail: ETC (0x93AA) */
    { 53,  55, "ETC",      "Misc scenes",                   overlay_ETC_entry},
    {145, 145, "ETC",      "Misc scenes",                   overlay_ETC_entry},
    {194, 196, "ETC",      "Misc scenes",                   overlay_ETC_entry},

    /* retail: OLH (0x96AA) -- memcard/minigame menus (olh_* names) */
    { 58,  58, "OLH",      "Memcard/minigame menus",        overlay_OLH_entry},

    /* retail: TEL (0x9A0A) */
    { 67,  67, "TEL",      "Phone call scenes",             overlay_TEL_entry},
    { 69,  69, "TEL",      "Phone call scenes",             overlay_TEL_entry},

    /* retail: DATE (0x922A) */
    { 80,  80, "DATE",     "Date scenes",                   overlay_DATE_entry},
    { 94,  94, "DATE",     "Date scenes",                   overlay_DATE_entry},

    /* retail: SHUGAKU (0x98EA) */
    { 81,  81, "SHUGAKU",  "School-trip scenes",            overlay_SHUGAKU_entry},

    /* retail: SHOUGATU (0x988A) */
    { 82,  83, "SHOUGATU", "New Year scenes",               overlay_SHOUGATU_entry},
    { 85,  85, "SHOUGATU", "New Year scenes",               overlay_SHOUGATU_entry},
    { 91,  91, "SHOUGATU", "New Year scenes",               overlay_SHOUGATU_entry},

    /* retail: VALEN (0x9ACA) */
    { 84,  84, "VALEN",    "Valentine scenes",              overlay_VALEN_entry},

    /* retail: GEKO (0x946A) */
    { 86,  87, "GEKO",     "GEKO.EXN scenes",               overlay_GEKO_entry},
    { 89,  90, "GEKO",     "GEKO.EXN scenes",               overlay_GEKO_entry},
    { 93,  93, "GEKO",     "GEKO.EXN scenes",               overlay_GEKO_entry},

    /* retail: DATE2 (0x928A) */
    { 88,  88, "DATE2",    "Date scenes (second set)",      overlay_DATE2_entry},
    { 92,  92, "DATE2",    "Date scenes (second set)",      overlay_DATE2_entry},

    /* retail: KANGEI (0x952A) */
    { 95,  96, "KANGEI",   "Welcome-party scenes",          overlay_KANGEI_entry},

    /* retail: OMIMAI (0x970A) */
    { 97,  98, "OMIMAI",   "Hospital-visit scenes",         overlay_OMIMAI_entry},

    /* retail: ENDING (0x92EA) */
    { 99,  99, "ENDING",   "Ending scenes",                 overlay_ENDING_entry},

    /* retail: BUNKASAI (0x916A) / BUNKAKEN (0x910A) / TAIIKU (0x99AA) /
     *        RENSYU (0x97CA) / MASTER (0x958A) */
    {112, 112, "BUNKASAI", "Culture-festival scenes",       overlay_BUNKASAI_entry},
    {113, 113, "BUNKAKEN", "Culture-presentation scenes",   overlay_BUNKAKEN_entry},
    {114, 114, "TAIIKU",   "Sports-festival scenes",        overlay_TAIIKU_entry},
    {115, 115, "RENSYU",   "Practice scenes",               overlay_RENSYU_entry},
    {116, 118, "MASTER",   "MASTER.EXN scenes",             overlay_MASTER_entry},

    /* retail: BUNKA_SD (0x91CA) / RPG_BAT (0x982A) / EN_NICHI (0x934A) */
    {128, 128, "BUNKA_SD", "Culture-festival SD scenes",    overlay_BUNKA_SD_entry},
    {129, 129, "RPG_BAT",  "RPG battle minigame",           overlay_RPG_BAT_entry},
    {130, 130, "EN_NICHI", "EN_NICHI.EXN scenes",           overlay_EN_NICHI_entry},

    /* retail: TT (0x9A6A) */
    {208, 208, "TT",       "TT.EXN minigame",               overlay_TT_entry},

    /* every other scene id (incl. 3, 18, 64, 100-111, 119-127, 131-144,
     * 146-193, 197-207, 209+): retail default arm -> TACO (0x994A).
     * EVENT/GYOZI deliberately absent -- never loaded. */
    {  0,   0, NULL,       NULL,                            NULL}  /* sentinel */
};

/* Find the overlay for a given scene */
const char *overlay_get_name(u8 scene_id) {
    const OverlayMapping *m;
    for (m = g_overlay_map; m->overlay_name; m++) {
        if (scene_id >= m->scene_min && scene_id <= m->scene_max) {
            return m->overlay_name;
        }
    }
    return "UNKNOWN";
}

const char *overlay_get_description(u8 scene_id) {
    const OverlayMapping *m;
    for (m = g_overlay_map; m->overlay_name; m++) {
        if (scene_id >= m->scene_min && scene_id <= m->scene_max) {
            return m->description;
        }
    }
    return "Unknown scene";
}

/* Dispatch to the correct overlay handler.
 * Unlisted scene ids go to TACO, mirroring the retail loader's default arm. */
void overlay_dispatch(void) {
    const OverlayMapping *m;
    extern int hal_overlay_load_exn(const char *name);
    for (m = g_overlay_map; m->overlay_name; m++) {
        if (now_step >= m->scene_min && now_step <= m->scene_max) {
            if (m->handler) {
                /* hal_overlay_load_exn(m->overlay_name) -- the PS1 loader
                 * flow; wiring it makes the window data live, which the
                 * globals-model pilot found exposes scene-state paths that
                 * need their setup first (PC-PORT-TODO section 4). */
                static u8 last_ovl_scene = 255;
                if (now_step != last_ovl_scene) {
                    debug_log("[Overlay] Scene %d -> %s (%s)",
                              now_step, m->overlay_name, m->description);
                    last_ovl_scene = now_step;
                }
                m->handler();
                return;
            }
            break;
        }
    }
    /* retail default arm: TACO.EXN */
    {
        static u8 last_def_scene = 255;
        if (now_step != last_def_scene) {
            debug_log("[Overlay] Scene %d -> TACO (retail default arm)", now_step);
            last_def_scene = now_step;
        }
    }
    overlay_TACO_entry();
}
