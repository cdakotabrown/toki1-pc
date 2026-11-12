/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pc_setup.h -- first run inside the game window (pc/hal/setup_screen.c).
 *
 * main() calls, in order:
 *   pc_setup_args()      takes --disc/--audio-track/--check/--licenses/
 *                        --prepare-assets/--verify-assets out of argv;
 *   pc_setup_cli()       the console-only modes (no window): returns 1
 *                        with an exit status when one ran;
 *   pc_setup_choose()    which game-data folder to use: an explicit one
 *                        (argv[1], TOKI_ASSET_DIR), the historical
 *                        ./pc/assets, the launcher's <exe>/pc/assets, or
 *                        <data root>/assets when its manifest checks out
 *                        (sizes, inventory, executable SHA1: fast).  NULL:
 *                        the setup screen is needed;
 *   hal_init(NULL)       creates the window and runs pc_setup_run(), which
 *                        finds the disc (disk/ beside the executable first),
 *                        verifies and extracts it with a progress bar, and
 *                        returns the new folder -- the game then starts with
 *                        no restart.
 * Everything the setup checks and decides goes to tokimeki-pc.log in the
 * data root (pc/include/pc_data_root.h).
 */
#ifndef PC_SETUP_H
#define PC_SETUP_H

#include <SDL2/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *disc;           /* --disc FILE */
    const char *audio;          /* --audio-track FILE */
    int check;                  /* --check: full disc + game data report */
    int licenses;               /* --licenses */
    int cli;                    /* --prepare-assets / --verify-assets given */
} PcSetupOpts;

int pc_setup_args(int argc, char **argv, PcSetupOpts *o);
/* Returns 1 when a console mode ran (*status: the exit status). */
int pc_setup_cli(const PcSetupOpts *o, int argc, char **argv, int *status);
/* The folder to use, or NULL when the setup screen must make it. */
const char *pc_setup_choose(const char *explicit_dir, const PcSetupOpts *o);
/* The setup screen; returns the prepared folder, or NULL when the player
 * closed the window (the caller exits). */
const char *pc_setup_run(SDL_Window *win, SDL_Renderer *ren);
/* The settings menu's "GAME DATA" page: verify game data, about/licences. */
void pc_setup_menu_register(void);
/* The embedded licence texts (pc/hal/licenses.c). */
const char *pc_licenses(void);

#ifdef __cplusplus
}
#endif
#endif
