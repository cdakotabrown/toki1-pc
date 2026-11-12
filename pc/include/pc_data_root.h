/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pc_data_root.h -- where the port keeps everything it reads and writes
 * (pc/hal/data_root.c).  docs/pc-port/PLAYER-GUIDE.md section 1 is the
 * player's description of these rules.
 *
 * The data root holds, in one folder:
 *     data/            the game data extracted from the player's disc
 *                      (assets.manifest.json, raw/, audio/, ...), with
 *       en/            the English files made from it (pc/hal/lang.c) and
 *       tokimeki-pc.log (+ .log.old)
 *     saves/           card1.mcr, quicksave/, extras/, seed/, seen_text.dat
 *     settings.ini     the options, and in its [state] section the window
 *                      place and the one-time hints
 *     screenshots/  textures/{dump,packs}/  music/{dump,packs}/  (made when
 *                      first used)
 * and, in a portable install, the player's disc goes in disk/ beside it.
 * Before 2026-11-07 the game data was assets/, the English files
 * assets-en/, the log in the root, and window.ini / hints.ini beside
 * settings.ini: pc_data_layout_migrate moves such an install over.
 *
 * Which folder (first match wins):
 *   1. TOKI_CONFIG_DIR=DIR                       everything in DIR
 *   2. portable: the executable's own folder, when
 *        TOKI_PORTABLE=1, or a file "portable.txt" beside the executable,
 *        or the build's default (PC_PORTABLE_DEFAULT: the Windows build and
 *        the Linux player package are portable; a source-checkout Linux
 *        build is not),
 *      unless TOKI_PORTABLE=0 or a file "user-data.txt" is beside it, and
 *      only when that folder is writable (else: 3, with `fallback` set so
 *      the setup screen can say so);
 *   3. per user: %APPDATA%\tokimeki-pc (Windows), ~/Library/Application
 *      Support/tokimeki-pc (macOS), or on Linux ~/.config/tokimeki-pc for
 *      settings.ini and ~/.local/share/tokimeki-pc for the rest
 *      (XDG_CONFIG_HOME / XDG_DATA_HOME honoured): the historical layout.
 *
 * The settings module (pc/hal/settings.c pc_settings_paths) takes its
 * config/data folders from here, so settings, saves, screenshots, texture
 * and music packs all follow the same rule.
 */
#ifndef PC_DATA_ROOT_H
#define PC_DATA_ROOT_H

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { PC_ROOT_USER, PC_ROOT_PORTABLE, PC_ROOT_ENV };

#define PC_ROOT_PATH_MAX 1024

typedef struct {
    int mode;                       /* PC_ROOT_* */
    int fallback;                   /* portable was wanted, the folder is read-only */
    char exe_dir[PC_ROOT_PATH_MAX];
    char root[PC_ROOT_PATH_MAX];    /* data root (assets/, saves/, logs, ...) */
    char why[128];                  /* what decided it, for the log */
} PcDataRoot;

/* Decide once (the first call); call early in main, before the settings
 * are read.  Never creates folders. */
const PcDataRoot *pc_data_root(void);
/* For pc/hal/settings.c: the folder settings.ini and the data
 * folders use when the root is portable or TOKI_CONFIG_DIR, else "". */
extern char pc_portable_root[PC_ROOT_PATH_MAX];

/* The game-data folder every asset reader uses (no trailing '/').  It is
 * "./pc/assets" (the historical working-directory layout every harness
 * uses) until main() sets the resolved one.
 * Readers call pc_asset_dir() / pc_asset_path(): the module is a weak
 * reference there, so the unit-test harnesses that compile one HAL file
 * alone keep "./pc/assets".  data_root.c defines PC_DATA_ROOT_IMPL, so its
 * own definitions stay strong on PE (docs/pc-port/WINDOWS-BUILD.md "WEAK"). */
#ifdef PC_DATA_ROOT_IMPL
#define PC_DR_WEAK
#else
#define PC_DR_WEAK __attribute__((weak))
#endif
const char *pc_asset_root(void) PC_DR_WEAK;
void pc_asset_root_set(const char *dir);
static __inline__ const char *pc_asset_dir(void) {
    return pc_asset_root ? pc_asset_root() : "./pc/assets";
}
/* root + "/" + rel into out; returns out. */
static __inline__ char *pc_asset_path(char *out, size_t n, const char *rel) {
    snprintf(out, n, "%s/%s", pc_asset_dir(), rel);
    return out;
}

/* The game-data folder of the data root (no trailing '/'): <root>/data,
 * or <root>/assets when an old install's folder could not be moved. */
#define PC_DATA_DIR "data"
/* The English files' folder inside it (pc/hal/lang.c). */
#define PC_DATA_EN "en"
const char *pc_data_dir(void);
/* 1 when pc_data_dir() is the old <root>/assets. */
int pc_data_layout_legacy(void);
/* (What else may sit in it: as_owned_entry, pc/hal/asset_setup.h.) */
/* The old layout (<root>/assets, <root>/assets-en) -> <root>/data and
 * <root>/data/en: one rename each when possible (the contents one by one,
 * the manifest last, when data/ already exists without game data).  When a
 * move fails the old folder stays in use (pc_data_dir, hal_lang_en_root)
 * and the next start tries again.  Logs with pc_logf; call once, right
 * after pc_data_root(), before anything opens a file there. */
void pc_data_layout_migrate(void);

/* "Player mode": the assets came from the data root (or the setup just
 * made them).  The saves then default to <root>/saves instead of the
 * working directory's "saves" (pc/hal/save_paths.c). */
void pc_data_root_set_player(int on);
int pc_data_root_player(void) PC_DR_WEAK;
/* 1 in a player's run (player mode and no automation variable): the
 * developer artifacts the HAL used to drop in the working folder
 * (debug.log, audio_dump.wav, game_state.json, vram_debug*.ppm) are not
 * written, so nothing lands outside the data root. */
int pc_player_quiet(void) PC_DR_WEAK;
static __inline__ int pc_dev_artifacts(void) { return !(pc_player_quiet && pc_player_quiet()); }

/* tokimeki-pc.log in pc_data_dir() (player mode, never in automated runs):
 * the previous run's log becomes tokimeki-pc.log.old.  With redirect, the
 * process's stdout/stderr go into it too (no console: the Windows build,
 * or Linux output that is not a terminal).  Returns 0 when open. */
int pc_log_open(const char *path, int redirect);
const char *pc_log_path(void);
/* A line in the log, and on stdout when stdout is not the log already. */
void pc_logf(const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
/* Called once per frame: stops a runaway log at 64 MB. */
void pc_log_frame(void) PC_DR_WEAK;

#ifdef __cplusplus
}
#endif
#endif
