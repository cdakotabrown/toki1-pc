/* SPDX-License-Identifier: GPL-3.0-or-later */
/* save_paths.c -- where the player's saves live (memory card, seeds,
 * quicksave slots).
 *
 * Resolution, first match wins:
 *   1. TOKI_SAVE_DIR            the launcher (pc/start_game.py) sets it to
 *                               <data dir>/saves, or to the player's
 *                               [system] save_dir; a developer may set it
 *   2. [system] save_dir        the settings file (pc/hal/settings.c); never
 *                               read in automated runs (harness rule)
 *   3. <data root>/saves        player mode (pc/include/pc_data_root.h: the
 *                               game data came from the data root, as in a
 *                               portable install), never in automated runs
 *   4. "saves"                  relative to the working directory: what the
 *                               port always did, and what every harness,
 *                               playthrough and the battery still get
 *
 * With 4 the paths are byte-for-byte the historical ones ("saves",
 * "saves/seed", "saves/card1.mcr"), so automated runs are unchanged.
 *
 * The strings are cached in this file's statics.  They describe the host,
 * not the machine, so pc/hal/dbg_state.c keeps this whole file out of a
 * save state's restore (KEEP_FILES): loading a state made by another session
 * never moves the player's memory card.
 */
#include "pc_settings.h"
#include "pc_data_root.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* a path separator: Windows takes both */
#ifdef _WIN32
#define SAVE_SEP(c) ((c) == '/' || (c) == '\\')
#else
#define SAVE_SEP(c) ((c) == '/')
#endif

static char g_dir[PC_SETTINGS_PATH_MAX];
static char g_card[PC_SETTINGS_PATH_MAX + 16];
static char g_seed[PC_SETTINGS_PATH_MAX + 16];
static int g_done;

static void resolve(void) {
    const char *e = getenv("TOKI_SAVE_DIR");
    const char *src = "default";
    size_t n;
    if (g_done) return;
    g_done = 1;
    if (e && *e) {
        snprintf(g_dir, sizeof g_dir, "%s", e);
        src = "TOKI_SAVE_DIR";
    } else {
        const PcSettings *s = pc_settings_get();
        if (!s->harness && s->save_dir[0]) {
            const char *home = getenv("HOME");
#ifdef _WIN32
            /* Windows has no HOME: "~" is the user's profile folder, and
             * "~\\x" is spelled with either separator there */
            if (!home || !*home) home = getenv("USERPROFILE");
#endif
            if (s->save_dir[0] == '~' && SAVE_SEP(s->save_dir[1]) &&
                home && *home)
                snprintf(g_dir, sizeof g_dir, "%s%s", home, s->save_dir + 1);
            else
                snprintf(g_dir, sizeof g_dir, "%s", s->save_dir);
            src = "settings system.save_dir";
        } else if (!s->harness && pc_data_root_player && pc_data_root_player()) {
            snprintf(g_dir, sizeof g_dir, "%s/saves", s->data_dir);
            src = "data root";
        } else {
            strcpy(g_dir, "saves");
        }
    }
    n = strlen(g_dir);
    while (n > 1 && SAVE_SEP(g_dir[n - 1])) g_dir[--n] = 0;
    snprintf(g_card, sizeof g_card, "%s/card1.mcr", g_dir);
    snprintf(g_seed, sizeof g_seed, "%s/seed", g_dir);
    if (strcmp(src, "default")) {
        /* a configured folder may be nested and not exist yet */
        if (pc_settings_mkdirs(g_seed, 1))
            fprintf(stderr, "[SAVES] cannot create %s\n", g_seed);
        printf("[SAVES] save directory %s (%s)\n", g_dir, src);
        fflush(stdout);
    }
}

const char *pc_save_dir(void) { resolve(); return g_dir; }
const char *pc_save_card_path(void) { resolve(); return g_card; }
const char *pc_save_seed_dir(void) { resolve(); return g_seed; }
