/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pc_settings.h -- the player's settings file (pc/hal/settings.c).
 *
 * Host-side polish only: display, input bindings, mixer volumes and
 * quality-of-life switches (and where saves live, pc/hal/save_paths.c).  Nothing here reaches the matched PS1 code; the
 * consumers are the presenter (pc/hal/present.c), the input HAL
 * (pc/hal/pad.c) and the PC mixer (pc/hal/spu.c).
 *
 * Sources, lowest to highest precedence:
 *   1. built-in defaults (the table in settings.c; they reproduce the port's
 *      behaviour before the settings file existed, except the keyboard map,
 *      see docs/pc-port/PLAYER-GUIDE.md);
 *   2. the settings file: $TOKI_SETTINGS, else <config dir>/settings.ini,
 *      written with the defaults and comments on the first interactive run;
 *   3. TOKI_SET="section.key=value;section.key=value" (pc/start_game.py
 *      --set / --fullscreen / --windowed produce it);
 *   4. the older per-feature variables (TOKI_MOUSE*, TOKI_INPUT_DEVICE),
 *      applied by their owners after the settings.
 *
 * Harness rule: when the environment says an automated run is in progress
 * (PC_TRACE, PC_TRACE_FRAMES, PC_INPUT, PC_TURBO, PC_PARITY_DIR, PC_SHOT_DIR,
 * PC_DBG_CMDS, TOKI_INPUT_SCRIPT, AUTOPLAY, SCENE_NAV, SDL_VIDEODRIVER=dummy/
 * offscreen) or TOKI_SETTINGS=off, sources 2 and 3 are ignored and nothing is
 * written: the scene matrix, playthroughs and parity tools always see the
 * defaults, whatever the player configured.
 */
#ifndef PC_SETTINGS_H
#define PC_SETTINGS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PS1 digital-pad button masks, as pc/hal/pad.c packs them (pressed = 1). */
#define PC_PAD_L2       0x0001u
#define PC_PAD_R2       0x0002u
#define PC_PAD_L1       0x0004u
#define PC_PAD_R1       0x0008u
#define PC_PAD_TRIANGLE 0x0010u
#define PC_PAD_CIRCLE   0x0020u
#define PC_PAD_CROSS    0x0040u
#define PC_PAD_SQUARE   0x0080u
#define PC_PAD_SELECT   0x0100u
#define PC_PAD_START    0x0800u
#define PC_PAD_UP       0x1000u
#define PC_PAD_RIGHT    0x2000u
#define PC_PAD_DOWN     0x4000u
#define PC_PAD_LEFT     0x8000u

enum {
    PC_BTN_UP, PC_BTN_DOWN, PC_BTN_LEFT, PC_BTN_RIGHT,
    PC_BTN_CIRCLE, PC_BTN_CROSS, PC_BTN_SQUARE, PC_BTN_TRIANGLE,
    PC_BTN_L1, PC_BTN_R1, PC_BTN_L2, PC_BTN_R2,
    PC_BTN_START, PC_BTN_SELECT,
    PC_BTN_COUNT
};
extern const char *const pc_button_names[PC_BTN_COUNT];
extern const unsigned pc_button_masks[PC_BTN_COUNT];

enum {
    PC_HK_FULLSCREEN, PC_HK_SCREENSHOT, PC_HK_PAUSE, PC_HK_FILTER,
    PC_HK_SCALE, PC_HK_SCANLINES, PC_HK_VSYNC, PC_HK_VOLUME_UP,
    PC_HK_VOLUME_DOWN, PC_HK_QUIT, PC_HK_MENU,
    /* the player's quicksave slots (pc/hal/quicksave.c) */
    PC_HK_QUICKSAVE, PC_HK_QUICKLOAD, PC_HK_QSLOT_NEXT, PC_HK_QSLOT_PREV,
    /* the enhanced dialogue backlog (pc/hal/backlog.c) */
    PC_HK_BACKLOG, PC_HK_VOICE_REPLAY,
    /* the save browser page of the settings menu (pc/hal/save_browser.c) */
    PC_HK_SAVES,
    /* the enhanced status-at-a-glance page (pc/hal/status_view.c) */
    PC_HK_STATUS,
    /* hold to rewind (enhanced; pc/hal/rewind.c) */
    PC_HK_REWIND,
    /* the controls legend page (pc/hal/controls_view.c) */
    PC_HK_CONTROLS,
    PC_HK_COUNT
};
extern const char *const pc_hotkey_names[PC_HK_COUNT];

enum { PC_SCALE_ASPECT, PC_SCALE_INTEGER, PC_SCALE_STRETCH };
enum { PC_FILTER_NEAREST, PC_FILTER_LINEAR };
enum { PC_LAYOUT_POSITIONAL, PC_LAYOUT_CONFIRM_SOUTH };
enum { PC_DEVICE_AUTO, PC_DEVICE_MOUSE, PC_DEVICE_PAD, PC_DEVICE_NONE };
enum { PC_MOUSE_ABSOLUTE, PC_MOUSE_RELATIVE };
enum { PC_AUTOSAVE_MONTH, PC_AUTOSAVE_WEEK };

#define PC_SETTINGS_PATH_MAX 1024
#define PC_BIND_MAX 128

/* [audio] interpolation / output_rate / reverb (pc/hal/spu_quality.c) */
enum { PC_INTERP_GAUSSIAN, PC_INTERP_CUBIC, PC_INTERP_SINC };
enum { PC_RATE_44100, PC_RATE_48000 };
enum { PC_REVERB_ORIGINAL, PC_REVERB_HQ };

typedef struct PcSettings {
    /* top level */
    char profile[32];           /* "parity" | "enhanced" (pc/hal/profile.*) */
    /* [display] */
    int fullscreen;
    int window_width, window_height;   /* 0 = automatic */
    int remember_window;
    int scale_mode;             /* PC_SCALE_* */
    int filter;                 /* PC_FILTER_* */
    int scanlines;              /* 0..100 % darkening of every other row */
    int crt;                    /* PC_CRT_* filter preset (pc/include/pc_crt.h) */
    int crt_curvature;          /* bend the CRT picture like a tube */
    int vsync;
    /* [graphics] */
    int resolution_scale;       /* 1..4: polygons at N x (enhanced; pc/hal/gpu_hires.h) */
    int subpixel;               /* sub-pixel 3D vertices in the N x picture (pc/hal/gpu_pgxp.h) */
    int texture_dump;           /* write every texture the game uses as PNG (pc/hal/gpu_texrep.h) */
    char texture_pack[256];     /* replacement pack: a name under <data dir>/textures/packs, or a path */
    /* [audio] 0..100 % */
    int vol_master, vol_music, vol_sfx, vol_voice;
    char music_pack[64];        /* music pack folder name under <data dir>/music/packs, "" = none */
    int music_dump;             /* write each song the game plays to <data dir>/music/dump */
    char soundfont[256];        /* SF2 under <data dir>/music/soundfonts, or a path; "" = none (pc/hal/soundfont.c) */
    int interpolation;          /* PC_INTERP_*: the SPU's sample interpolation (enhanced; pc/hal/spu_quality.c) */
    int output_rate;            /* PC_RATE_*: the audio device's rate (enhanced) */
    int reverb_quality;         /* PC_REVERB_*: the SPU reverb's precision (enhanced) */
    /* [input] */
    int mouse_enabled;
    double mouse_sensitivity;
    int mouse_mode;             /* PC_MOUSE_*: the hand follows the OS pointer / relative motion */
    int mouse_grab;
    int mouse_swap_gap;
    int input_device;           /* PC_DEVICE_* */
    int controller_enabled;
    int controller_layout;      /* PC_LAYOUT_* */
    int stick_threshold;        /* 0..32767 */
    int trigger_threshold;      /* 0..32767 */
    int port2_controller;       /* the second controller is a pad on PS1 port 2 */
    char menu_button[PC_BIND_MAX];   /* controller buttons that open the settings menu */
    char backlog_button[PC_BIND_MAX];   /* ... that open the dialogue backlog (enhanced) */
    /* [keyboard] / [controller]: comma-separated SDL names per PS1 button */
    char key_bind[PC_BTN_COUNT][PC_BIND_MAX];
    char pad_bind[PC_BTN_COUNT][PC_BIND_MAX];
    /* [hotkeys] */
    char hotkey[PC_HK_COUNT][PC_BIND_MAX];
    /* [system] */
    int pause_on_focus_loss;
    int toasts;
    int screenshot_scale;       /* 1..8 */
    char screenshot_dir[PC_SETTINGS_PATH_MAX];   /* "" = <data dir>/screenshots */
    char save_dir[PC_SETTINGS_PATH_MAX];         /* "" = TOKI_SAVE_DIR / cwd "saves" (pc/hal/save_paths.c) */
    /* [saves] (pc/hal/save_auto.c; never acted on in automated runs) */
    int autosave;               /* a rotating autosave at each week's planner */
    int autosave_slots;         /* 1..9 rotating autosaves */
    int autosave_every;         /* PC_AUTOSAVE_*: first planner of each month / every week */
    int autosave_ending;        /* one more when the ending starts */
    int resume;                 /* a resume state on quit, offered at launch */
    /* [enhanced] */
    int ff_speed;               /* fast-forward: game frames per shown frame, 0 = unpaced */
    int backlog;                /* the dialogue backlog and voice replay (pc/hal/backlog.c) */
    int status;                 /* the status-at-a-glance page (pc/hal/status_view.c) */
    int status_hud;             /* its one-line HUD over the game (off by default) */
    int rewind;                 /* keep a rewind buffer; hold [hotkeys] rewind (pc/hal/rewind.c) */
    int rewind_seconds;         /* how far back it reaches */
    int rewind_interval;        /* game frames between two snapshots */
    int achievements;           /* extras: track and announce achievements (opt-in; pc/hal/extras.c) */
    int skip_intro;             /* press START through the boot movie and title intro (pc/hal/enhance.c) */
    int skip_seen;              /* fast-forward also confirms dialogue pages read before (pc/hal/seen_text.c) */
    int quick_text;             /* confirm finishes a printing page, then cuts its voice (pc/hal/quick_text.c) */
    /* [game] */
    int language;               /* 0 = ja, 1 = en (pc/hal/lang.c) */

    /* not settings: where they came from */
    int harness;                /* automated run: defaults only */
    char harness_reason[64];
    char file_path[PC_SETTINGS_PATH_MAX];        /* "" when none was read */
    char config_dir[PC_SETTINGS_PATH_MAX];
    char data_dir[PC_SETTINGS_PATH_MAX];
    int created;                /* the file was written this run */
    int warnings;               /* unknown keys / bad values seen */
} PcSettings;

typedef const char *(*pc_getenv_fn)(const char *name);

/* The process-wide settings, loaded on first use from the real environment. */
const PcSettings *pc_settings_get(void);
/* Runtime changes.  Hotkey changes are session only; the settings menu
 * (pc/hal/menu.c) writes the keys it changed back with
 * pc_settings_save_keys when it closes (never in a harness run). */
PcSettings *pc_settings_mut(void);

/* Pure pieces, unit-tested in pc/tests/test_settings.c. */
void pc_settings_defaults(PcSettings *s);
/* Apply INI text.  Returns the number of warnings. `origin` names it in
 * messages.  Unknown keys and bad values warn and keep the old value. */
int  pc_settings_parse(PcSettings *s, const char *text, const char *origin);
/* Apply "section.key=value" items separated by ';' or newlines (TOKI_SET). */
int  pc_settings_apply_overrides(PcSettings *s, const char *list);
/* One key ("display.scale" or "profile"); 0 on success. */
int  pc_settings_set(PcSettings *s, const char *dotted_key, const char *value);
/* Write a commented settings file holding the values in `s`. 0 on success. */
int  pc_settings_write(const PcSettings *s, const char *path);
/* One key's value as the file spells it ("aspect", "true", "\"parity\"").
 * 0 on success, -1 for an unknown key. */
int  pc_settings_format(const PcSettings *s, const char *dotted_key, char *out, unsigned n);
/* `text` (a settings file) with `section`.`key` set to `value`: every
 * existing assignment of the key is rewritten in place (indentation and a
 * trailing comment kept); a missing key is added at the end of its
 * section; a missing section is appended (the top level "" goes before the
 * first section).  Every other line -- comments, unknown keys, blank
 * lines -- is kept byte for byte.  Returns malloc'd text (NULL: no memory). */
char *pc_settings_edit_text(const char *text, const char *section, const char *key,
                            const char *value);
/* Write the values of the `n` dotted keys from `s` into the file at `path`
 * with pc_settings_edit_text (a missing file is started empty), through a
 * temporary file renamed over it.  Refuses (-1, nothing written) when `s`
 * is a harness run or has no file path.  0 on success. */
int  pc_settings_save_keys(const PcSettings *s, const char *path,
                           const char *const *keys, int n);
/* The automation variable that makes this a harness run, or NULL. */
const char *pc_settings_harness_reason(pc_getenv_fn env);
/* Fill config_dir / data_dir / file_path from the environment. */
void pc_settings_paths(PcSettings *s, pc_getenv_fn env);
/* mkdir -p of path's parent (dir=0) or of path itself (dir=1). */
/* mkdir -p loops: does `p` (not the first character) end a folder name?
 * Windows takes both separators and never splits a drive root ("C:\\");
 * POSIX only '/'. */
#ifdef _WIN32
#define PC_MKDIR_SPLIT(p) ((*(p) == '/' || *(p) == '\\') && (p)[-1] != ':')
#else
#define PC_MKDIR_SPLIT(p) (*(p) == '/')
#endif
int  pc_settings_mkdirs(const char *path, int dir);
/* Full load as pc_settings_get does it, with an injectable environment. */
void pc_settings_load(PcSettings *s, pc_getenv_fn env, int create_missing);

/* Binding strings: "Return, Space" -> tokens; 0..max tokens returned. */
int  pc_bind_tokens(const char *list, char tokens[][64], int max);
/* "Ctrl+Alt+Return" -> mods (PC_MOD_*) and the key name it points into. */
#define PC_MOD_CTRL  1u
#define PC_MOD_ALT   2u
#define PC_MOD_SHIFT 4u
#define PC_MOD_GUI   8u
const char *pc_hotkey_split(const char *token, unsigned *mods);

/* A resolved keyboard / controller map (codes from the caller's lookup). */
#define PC_KEYMAP_MAX 96
typedef struct { int code; int axis_sign; unsigned mask; } PcBind;
typedef struct { PcBind b[PC_KEYMAP_MAX]; int n; int unresolved; } PcKeymap;
/* lookup(name) returns a code >= 0, or < 0 when the name is unknown. */
void pc_keymap_build_keys(PcKeymap *km, const char binds[][PC_BIND_MAX],
                          int (*lookup)(const char *name));
/* Controller tokens: a button name ("a", "dpup", "leftshoulder") or a signed
 * axis ("+lefttrigger", "-leftx"); axes get axis_sign +1/-1. */
void pc_keymap_build_pad(PcKeymap *km, const char binds[][PC_BIND_MAX],
                         int (*button_lookup)(const char *name),
                         int (*axis_lookup)(const char *name));
/* Buttons held according to a key-state array (state[code] != 0).  With
 * `only_codes_ge` >= 0, bindings whose code is below it are ignored (the
 * modifier-held rule: Ctrl/Alt chords never press game buttons). */
unsigned pc_keymap_keys(const PcKeymap *km, const unsigned char *state,
                        int nstate, int only_codes_ge);

/* [state]: a section of settings.ini the program owns (window place and
 * size, one-time hints; 2026-11-07, before: window.ini and hints.ini).  The
 * parser skips it; the menu's key writer never touches it; writes go
 * through pc_settings_edit_text and a temporary file, so the player's
 * edits and comments elsewhere in the file are kept byte for byte. */
#define PC_SETTINGS_STATE_SECTION "state"
/* `key`'s value from [state] of the file at `path`; 0 when found. */
int  pc_settings_state_get(const char *path, const char *key, char *out, size_t n);
/* Set `n` keys of [state] (the section is appended, with a note, when
 * missing).  0 on success; the file is unchanged on failure. */
int  pc_settings_state_set(const char *path, const char *const *keys,
                           const char *const *values, int n);
/* Fold <config_dir>/window.ini and hints.ini (the old layout) into
 * [state] of `settings_path`, then delete them (an empty one is just
 * deleted).  A file whose fold fails is kept.  Returns the number deleted. */
int  pc_settings_fold_legacy(const char *settings_path, const char *config_dir);

/* Persisted window geometry: window_x / window_y / window_width /
 * window_height / window_maximized in [state] of the settings file at
 * `path`. */
typedef struct { int valid, x, y, w, h, maximized; } PcWindowState;
int  pc_window_state_load(const char *path, PcWindowState *ws);
int  pc_window_state_save(const char *path, const PcWindowState *ws);
/* The old window.ini format (x / y / width / height / maximized). */
int  pc_window_state_load_legacy(const char *path, PcWindowState *ws);

#ifdef __cplusplus
}
#endif
#endif /* PC_SETTINGS_H */
