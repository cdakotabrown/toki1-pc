/* SPDX-License-Identifier: GPL-3.0-or-later */
/* settings.c -- the player's settings file (see pc/include/pc_settings.h).
 *
 * SDL-free and free of game state, so pc/tests/test_settings.c links it
 * directly.  One table (g_keys) is the single source of truth for every key:
 * its section, type, range, default and the comment written into a fresh
 * settings file.  Defaults are applied by parsing the table's default
 * strings through the same setter the file uses.
 *
 * Format: a small INI/TOML subset.
 *     # comment            ; comment
 *     profile = "enhanced"
 *     [display]
 *     scale = aspect       (quotes optional; booleans true/false/on/off/1/0)
 */
#include "pc_settings.h"

#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <direct.h>
#define pc_mkdir(p) _mkdir(p)
#else
#define pc_mkdir(p) mkdir((p), 0755)
#endif

const char *const pc_button_names[PC_BTN_COUNT] = {
    "up", "down", "left", "right", "circle", "cross", "square", "triangle",
    "l1", "r1", "l2", "r2", "start", "select",
};
const unsigned pc_button_masks[PC_BTN_COUNT] = {
    PC_PAD_UP, PC_PAD_DOWN, PC_PAD_LEFT, PC_PAD_RIGHT,
    PC_PAD_CIRCLE, PC_PAD_CROSS, PC_PAD_SQUARE, PC_PAD_TRIANGLE,
    PC_PAD_L1, PC_PAD_R1, PC_PAD_L2, PC_PAD_R2, PC_PAD_START, PC_PAD_SELECT,
};
const char *const pc_hotkey_names[PC_HK_COUNT] = {
    "fullscreen", "screenshot", "pause", "filter", "scale", "scanlines",
    "vsync", "volume_up", "volume_down", "quit", "menu",
    "quicksave", "quickload", "quick_slot_next", "quick_slot_prev",
    "backlog", "voice_replay",
    "saves", "status",
    "rewind",
    "controls",
};

/* ------------------------------------------------------------------ */
/* The key table                                                       */
/* ------------------------------------------------------------------ */

enum { T_BOOL, T_INT, T_FLOAT, T_STR, T_ENUM };

typedef struct {
    const char *section;        /* "" = top level */
    const char *key;
    int type;
    size_t off;
    size_t size;                /* T_STR capacity */
    double lo, hi;              /* T_INT / T_FLOAT range */
    const char *const *names;   /* T_ENUM, NULL-terminated */
    const char *def;
    const char *comment;
} KeyDef;

static const char *const scale_names[] = { "aspect", "integer", "stretch", NULL };
static const char *const filter_names[] = { "nearest", "linear", NULL };
/* order = PC_CRT_* (pc/include/pc_crt.h) */
static const char *const crt_names[] = { "off", "lite", "aperture", "shadow", NULL };
static const char *const layout_names[] = { "positional", "confirm_south", NULL };
static const char *const device_names[] = { "auto", "mouse", "pad", "none", NULL };
static const char *const mouse_mode_names[] = { "absolute", "relative", NULL };
static const char *const autosave_every_names[] = { "month", "week", NULL };
static const char *const language_names[] = { "ja", "en", NULL };
static const char *const interp_names[] = { "gaussian", "cubic", "sinc", NULL };
static const char *const rate_names[] = { "44100", "48000", NULL };
static const char *const reverb_names[] = { "original", "hq", NULL };

#define F(m) offsetof(PcSettings, m)
#define S(m) F(m), sizeof(((PcSettings *)0)->m)
#define KB(i, d) { "keyboard", NULL, T_STR, F(key_bind[i]), PC_BIND_MAX, 0, 0, NULL, d, NULL }
#define PB(i, d) { "controller", NULL, T_STR, F(pad_bind[i]), PC_BIND_MAX, 0, 0, NULL, d, NULL }
#define HK(i, d) { "hotkeys", NULL, T_STR, F(hotkey[i]), PC_BIND_MAX, 0, 0, NULL, d, NULL }

/* Binding rows carry key == NULL; their names come from pc_button_names /
 * pc_hotkey_names (fixed up by key_name()). */
static const KeyDef g_keys[] = {
    { "", "profile", T_STR, S(profile), 0, 0, NULL, "enhanced",
      "Game profile: \"enhanced\" (instant loads, steady frame rate and the PC\n"
      "play features; the default) or \"parity\" (console-faithful timing).\n"
      "Automated runs ignore this file and always use parity." },

    { "display", "fullscreen", T_BOOL, F(fullscreen), 0, 0, 0, NULL, "false",
      "Start in (desktop) fullscreen. Alt+Enter / F11 toggle it while playing." },
    { "display", "window_width", T_INT, F(window_width), 0, 0, 16384, NULL, "0",
      "Window size in pixels on first launch; 0 = the largest whole multiple of\n"
      "320x240 that fits the desktop." },
    { "display", "window_height", T_INT, F(window_height), 0, 0, 16384, NULL, "0", NULL },
    { "display", "remember_window", T_BOOL, F(remember_window), 0, 0, 0, NULL, "true",
      "Reopen the window at the size and place it had when the game was closed\n"
      "(kept in the [state] section at the end of this file)." },
    { "display", "scale", T_ENUM, F(scale_mode), 0, 0, 0, scale_names, "aspect",
      "aspect = 4:3, as large as fits, letter/pillarboxed; integer = largest\n"
      "whole multiple of 320x240 (sharpest, more border); stretch = fill window." },
    { "display", "filter", T_ENUM, F(filter), 0, 0, 0, filter_names, "nearest",
      "Scaling filter: nearest (crisp pixels) or linear (smooth)." },
    { "display", "scanlines", T_INT, F(scanlines), 0, 0, 100, NULL, "0",
      "Scanline overlay strength in percent (0 = off). Drawn by the presenter\n"
      "only; the game's picture and screenshots are unaffected." },
    { "display", "crt", T_ENUM, F(crt), 0, 0, 0, crt_names, "off",
      "CRT filter: off, lite (soft scanlines), aperture (scanlines + RGB stripe\n"
      "mask + glow) or shadow (scanlines + slot mask + more glow). Drawn by the\n"
      "presenter at 2x and up; replaces filter and scanlines while on.\n"
      "Screenshots stay the plain picture. TOKI_CRT overrides it." },
    { "display", "crt_curvature", T_BOOL, F(crt_curvature), 0, 0, 0, NULL, "false",
      "With a CRT filter on: bend the picture like a tube (rounded edges).\n"
      "TOKI_CRT_CURVATURE overrides it." },
    { "display", "vsync", T_BOOL, F(vsync), 0, 0, 0, NULL, "false",
      "Wait for the monitor's refresh when presenting (reduces tearing)." },

    { "graphics", "resolution_scale", T_INT, F(resolution_scale), 0, 1, 4, NULL, "1",
      "Internal resolution for 3D (enhanced profile only): 3D polygons are\n"
      "drawn at this many times 320x240 (2 = 640x480, 4 = 1280x960); 2D\n"
      "pictures, sprites and text stay pixel-for-pixel as on the console.\n"
      "1 = off. Parity ignores it (GPU_HIRES)." },
    { "graphics", "subpixel", T_BOOL, F(subpixel), 0, 0, 0, NULL, "true",
      "Enhanced profile, with resolution_scale 2..4: place 3D vertices at the\n"
      "geometry engine's sub-pixel positions in the high-resolution picture,\n"
      "so 3D models move smoothly instead of snapping to the console's pixel\n"
      "grid. The game's own picture is unchanged. TOKI_SUBPIXEL=0 overrides." },
    { "graphics", "texture_dump", T_BOOL, F(texture_dump), 0, 0, 0, NULL, "false",
      "Enhanced profile: write every texture the game uses as a PNG into\n"
      "<data dir>/textures/dump/SLPS-00065/ (with index.csv), for upscaling." },
    { "graphics", "texture_pack", T_STR, S(texture_pack), 0, 0, NULL, "",
      "Enhanced profile: a texture replacement pack -- a folder name under\n"
      "<data dir>/textures/packs/, or a path. PNGs named as the dump names\n"
      "them, any size (2x, 4x, ...). \"\" = none." },

    { "audio", "master", T_INT, F(vol_master), 0, 0, 100, NULL, "100",
      "Volumes in percent, applied in the PC mixer (100 = as the game sets it).\n"
      "master scales everything." },
    { "audio", "music", T_INT, F(vol_music), 0, 0, 100, NULL, "100",
      "music and sfx: the SPU's 24 voices play both the sequenced music and the\n"
      "sound effects, and the mixer cannot yet tell them apart, so the two are\n"
      "multiplied together onto that one bus (while a music pack plays a song,\n"
      "music is the pack's volume and sfx the SPU's)." },
    { "audio", "sfx", T_INT, F(vol_sfx), 0, 0, 100, NULL, "100", NULL },
    { "audio", "voice", T_INT, F(vol_voice), 0, 0, 100, NULL, "100",
      "voice: audio streamed from the disc (character voices, streamed music\n"
      "and movie sound)." },
    { "audio", "music_pack", T_STR, S(music_pack), 0, 0, NULL, "",
      "Enhanced profile: the folder in <data dir>/music/packs/ whose files\n"
      "replace the game's sequenced songs (<track id>.ogg / .flac / .wav;\n"
      "PLAYER-GUIDE.md \"Music packs\"). Empty = the original music.\n"
      "TOKI_MUSIC_PACK overrides it." },
    { "audio", "music_dump", T_BOOL, F(music_dump), 0, 0, 0, NULL, "false",
      "Write every song the game plays, as the console's sound chip renders it,\n"
      "to <data dir>/music/dump/<track id>.wav (to identify tracks).\n"
      "TOKI_MUSIC_DUMP=1 does the same." },
    { "audio", "soundfont", T_STR, S(soundfont), 0, 0, NULL, "",
      "Enhanced profile: a SoundFont (.sf2) in <data dir>/music/soundfonts/ (or a\n"
      "path) that plays the game's sequenced music in place of the console's\n"
      "instruments (PLAYER-GUIDE.md \"SoundFont mode\"; <name>.map next to it\n"
      "maps the game's instruments). Empty = the original. TOKI_SOUNDFONT\n"
      "overrides it." },
    { "audio", "interpolation", T_ENUM, F(interpolation), 0, 0, 0, interp_names, "gaussian",
      "Enhanced profile: how the sound chip interpolates its samples. gaussian\n"
      "= the console (soft); cubic and sinc keep more treble (less muffled).\n"
      "TOKI_SPU_INTERP overrides it." },
    { "audio", "output_rate", T_ENUM, F(output_rate), 0, 0, 0, rate_names, "44100",
      "Enhanced profile: the audio device's rate. 44100 = the console's; 48000\n"
      "resamples the mix with a band-limited filter for devices that run at\n"
      "48 kHz, instead of the system's resampler. Takes effect on the next launch\n"
      "(TOKI_AUDIO_RATE)." },
    { "audio", "reverb", T_ENUM, F(reverb_quality), 0, 0, 0, reverb_names, "original",
      "Enhanced profile: original = the console's 16-bit reverb; hq = the same\n"
      "reverb computed in floating point (smoother tails). TOKI_SPU_REVERB." },

    { "input", "mouse", T_BOOL, F(mouse_enabled), 0, 0, 0, NULL, "true",
      "Present a PS1 mouse on port 1 when the mouse is used (TOKI_MOUSE)." },
    { "input", "mouse_mode", T_ENUM, F(mouse_mode), 0, 0, 0, mouse_mode_names, "absolute",
      "absolute: the game's hand cursor goes where the OS pointer is in the\n"
      "window (works under WSLg, remote desktop and VMs). relative: the hand\n"
      "moves by the mouse's motion, the OS cursor is hidden and confined, as a\n"
      "PS1 mouse on a desk (TOKI_MOUSE_MODE)." },
    { "input", "mouse_sensitivity", T_FLOAT, F(mouse_sensitivity), 0, 0.01, 100, NULL, "1.0",
      "Relative mode: host pixels per PS1 mouse count (TOKI_MOUSE_SENS)." },
    { "input", "mouse_grab", T_BOOL, F(mouse_grab), 0, 0, 0, NULL, "true",
      "Hide the OS cursor while the game is in mouse mode (over the picture in\n"
      "absolute mode; hidden and confined in relative mode)." },
    { "input", "mouse_swap_gap", T_INT, F(mouse_swap_gap), 0, 0, 60, NULL, "1",
      "Empty-port polls between devices on a pad/mouse hot-switch." },
    { "input", "device", T_ENUM, F(input_device), 0, 0, 0, device_names, "auto",
      "Port 1: auto (last used), mouse, pad or none (TOKI_INPUT_DEVICE)." },
    { "input", "controller", T_BOOL, F(controller_enabled), 0, 0, 0, NULL, "true",
      "Use SDL game controllers (hot-plug supported)." },
    { "input", "controller_layout", T_ENUM, F(controller_layout), 0, 0, 0, layout_names,
      "positional",
      "positional: face buttons by position, as on a PS1 pad (south = cross,\n"
      "east = circle). The game confirms with circle, so on an Xbox pad or\n"
      "Steam Deck that is B. confirm_south: south (A / cross) confirms and east\n"
      "(B / circle) cancels. Explicit [controller] cross/circle lines win." },
    { "input", "stick_threshold", T_INT, F(stick_threshold), 0, 1000, 32767, NULL, "16000",
      "Left-stick deflection (of 32767) that counts as a d-pad press." },
    { "input", "trigger_threshold", T_INT, F(trigger_threshold), 0, 1000, 32767, NULL, "8000",
      "Analog trigger travel (of 32767) that counts as L2/R2." },
    { "input", "port2_controller", T_BOOL, F(port2_controller), 0, 0, 0, NULL, "false",
      "true: the second controller connected is a pad on PS1 port 2 (the\n"
      "game's controller-2 extras); false: every controller is port 1." },
    { "input", "menu_button", T_STR, S(menu_button), 0, 0, NULL, "guide, touchpad",
      "Controller button(s) that open the settings menu (SDL GameController\n"
      "names, comma-separated; none = keyboard only; touchpad is the\n"
      "PlayStation touchpad click). The keyboard key is\n"
      "[hotkeys] menu." },
    { "input", "backlog_button", T_STR, S(backlog_button), 0, 0, NULL, "leftstick",
      "Controller button(s) that open the dialogue backlog (enhanced profile;\n"
      "none = keyboard and mouse wheel only). The keyboard key is [hotkeys] backlog." },

    /* [keyboard]: SDL scancode names (https://wiki.libsdl.org/SDL2/SDL_Scancode) */
    KB(PC_BTN_UP, "Up"), KB(PC_BTN_DOWN, "Down"),
    KB(PC_BTN_LEFT, "Left"), KB(PC_BTN_RIGHT, "Right"),
    KB(PC_BTN_CIRCLE, "Return, Space, Keypad Enter"),
    KB(PC_BTN_CROSS, "Backspace, Escape"),
    KB(PC_BTN_SQUARE, "Q"), KB(PC_BTN_TRIANGLE, "E"),
    KB(PC_BTN_L1, "A"), KB(PC_BTN_R1, "D"), KB(PC_BTN_L2, "Z"), KB(PC_BTN_R2, "C"),
    KB(PC_BTN_START, "F1, S"), KB(PC_BTN_SELECT, "Tab"),

    /* [controller]: SDL GameController names; +axis / -axis for directions */
    PB(PC_BTN_UP, "dpup, -lefty"), PB(PC_BTN_DOWN, "dpdown, +lefty"),
    PB(PC_BTN_LEFT, "dpleft, -leftx"), PB(PC_BTN_RIGHT, "dpright, +leftx"),
    PB(PC_BTN_CIRCLE, "b"), PB(PC_BTN_CROSS, "a"),
    PB(PC_BTN_SQUARE, "x"), PB(PC_BTN_TRIANGLE, "y"),
    PB(PC_BTN_L1, "leftshoulder"), PB(PC_BTN_R1, "rightshoulder"),
    PB(PC_BTN_L2, "+lefttrigger"), PB(PC_BTN_R2, "+righttrigger"),
    PB(PC_BTN_START, "start"), PB(PC_BTN_SELECT, "back"),

    HK(PC_HK_FULLSCREEN, "F11, Alt+Return"),
    HK(PC_HK_SCREENSHOT, "PrintScreen, Ctrl+P"),
    HK(PC_HK_PAUSE, "Pause, Ctrl+Space"),
    HK(PC_HK_FILTER, "Ctrl+F"),
    HK(PC_HK_SCALE, "Ctrl+M"),
    HK(PC_HK_SCANLINES, "Ctrl+L"),
    HK(PC_HK_VSYNC, "Ctrl+V"),
    HK(PC_HK_VOLUME_UP, "Ctrl+="),
    HK(PC_HK_VOLUME_DOWN, "Ctrl+-"),
    HK(PC_HK_QUIT, "Ctrl+Q"),
    HK(PC_HK_MENU, "Ctrl+O"),
    HK(PC_HK_QUICKSAVE, "F5"),
    HK(PC_HK_QUICKLOAD, "F9"),
    HK(PC_HK_QSLOT_NEXT, "]"),
    HK(PC_HK_QSLOT_PREV, "["),
    HK(PC_HK_BACKLOG, "Ctrl+B"),
    HK(PC_HK_VOICE_REPLAY, "Ctrl+R"),
    HK(PC_HK_SAVES, "Ctrl+S"),
    HK(PC_HK_STATUS, "Ctrl+I"),
    HK(PC_HK_REWIND, "Delete, Ctrl+Z"),
    HK(PC_HK_CONTROLS, "Ctrl+H"),

    { "system", "pause_on_focus_loss", T_BOOL, F(pause_on_focus_loss), 0, 0, 0, NULL, "false",
      "Pause the game (and its sound) while the window is in the background." },
    { "system", "toasts", T_BOOL, F(toasts), 0, 0, 0, NULL, "false",
      "Show short on-screen notes: a hotkey changed a setting, a quicksave\n"
      "finished, a new gallery picture, ending or achievement.  Off by default;\n"
      "the first-run controls hint and error notes are always shown." },
    { "system", "screenshot_dir", T_STR, S(screenshot_dir), 0, 0, NULL, "",
      "Screenshot folder; empty = the 'screenshots' folder in the data directory." },
    { "system", "save_dir", T_STR, S(save_dir), 0, 0, NULL, "",
      "Folder for the memory card (card1.mcr) and quicksaves; empty = the\n"
      "launcher's default (the 'saves' folder in the data directory), or\n"
      "'saves' next to the game when it is started directly. TOKI_SAVE_DIR wins." },
    { "system", "screenshot_scale", T_INT, F(screenshot_scale), 0, 1, 8, NULL, "2",
      "Screenshots are the game's 320x240 picture enlarged by this whole factor." },

    { "saves", "autosave", T_BOOL, F(autosave), 0, 0, 0, NULL, "true",
      "Save the game automatically at the planning screen (the screen where\n"
      "you choose the week's activities; how often: autosave_every), into the rotating slots\n"
      "auto1..autoN of the saves folder. Both profiles; never in automated runs." },
    { "saves", "autosave_every", T_ENUM, F(autosave_every), 0, 0, 0, autosave_every_names, "month",
      "month: autosave at the first planning screen of each month (the week\n"
      "that crosses into the new month). week: at every week's planning screen." },
    { "saves", "autosave_slots", T_INT, F(autosave_slots), 0, 1, 9, NULL, "3",
      "How many autosaves to keep (1-9); the oldest is overwritten." },
    { "saves", "autosave_ending", T_BOOL, F(autosave_ending), 0, 0, 0, NULL, "true",
      "Also save when the ending (graduation day, the confession) starts, into\n"
      "the 'ending' save, so it can be watched again." },
    { "saves", "resume", T_BOOL, F(resume), 0, 0, 0, NULL, "true",
      "When you quit during a game, save where you are and offer to continue\n"
      "from there at the next launch (only with the same build of the game)." },

    { "enhanced", "fast_forward_speed", T_INT, F(ff_speed), 0, 0, 64, NULL, "4",
      "Enhanced profile: game frames run per shown frame while the fast-forward\n"
      "key is held (0 = as fast as the computer can). TOKI_FF_SPEED overrides it." },
    { "enhanced", "backlog", T_BOOL, F(backlog), 0, 0, 0, NULL, "true",
      "Enhanced profile: keep the dialogue pages of this session for the backlog\n"
      "(Ctrl+B, the mouse wheel up, the left-stick click) and replay their voice\n"
      "lines (Enter in the backlog, Ctrl+R for the last one)." },
    { "enhanced", "status", T_BOOL, F(status), 0, 0, 0, NULL, "true",
      "Enhanced profile: the read-only status page (Ctrl+I): the girls you have\n"
      "met with their affection, bomb and ending score, your parameters, and the\n"
      "calendar ahead, as the game itself holds them." },
    { "enhanced", "status_hud", T_BOOL, F(status_hud), 0, 0, 0, NULL, "false",
      "Enhanced profile: a one-line note over the game with your stress and the\n"
      "girl whose bomb is highest." },
    { "enhanced", "rewind", T_BOOL, F(rewind), 0, 0, 0, NULL, "true",
      "Enhanced profile: keep the last moments of play in memory; hold the\n"
      "rewind key ([hotkeys] rewind, Delete or Ctrl+Z) to step back through\n"
      "them, release to play on from there. About 100-200 MB of memory." },
    { "enhanced", "rewind_seconds", T_INT, F(rewind_seconds), 0, 5, 300, NULL, "30",
      "How far back rewinding reaches, in seconds of play (5-300)." },
    { "enhanced", "rewind_interval_frames", T_INT, F(rewind_interval), 0, 1, 60, NULL, "10",
      "Game frames between two rewind points (1-60; 60 frames = 1 second).\n"
      "Smaller steps back more finely and uses more memory." },
    { "enhanced", "achievements", T_BOOL, F(achievements), 0, 0, 0, NULL, "false",
      "Enhanced profile: track achievements and show a note when one unlocks\n"
      "(Extras > Achievements). Off unless you turn it on; the endings list\n"
      "and the gallery do not depend on it." },
    { "enhanced", "skip_intro", T_BOOL, F(skip_intro), 0, 0, 0, NULL, "false",
      "Enhanced profile: press START for you through the boot movie and the\n"
      "title animation, straight to the title menu (the game's own skip)." },
    { "enhanced", "skip_seen", T_BOOL, F(skip_seen), 0, 0, 0, NULL, "false",
      "Enhanced profile: while the fast-forward key is held, also confirm\n"
      "dialogue pages you have read before (in any session); it stops at\n"
      "new text. The read pages are kept in seen_text.dat in the saves folder." },
    { "enhanced", "quick_text", T_BOOL, F(quick_text), 0, 0, 0, NULL, "true",
      "Enhanced profile: confirm while a dialogue page is still printing shows the\n"
      "whole page at once; confirm again while its voice line plays cuts the voice\n"
      "and goes on (the game's own cancel-button cut). TOKI_QUICK_TEXT overrides it." },

    { "game", "language", T_ENUM, F(language), 0, 0, 0, language_names, "en",
      "Game text language: en (the project's English translation, the default;\n"
      "built on first use from your own Japanese files into the data/en folder;\n"
      "docs/pc-port/TRANSLATION-ON-PC.md) or ja (the original\n"
      "Japanese). Takes effect on the next launch. TOKI_LANG=ja|en overrides it.\n"
      "Automated runs ignore this file and play in Japanese." },
};
#define NKEYS ((int)(sizeof(g_keys) / sizeof(g_keys[0])))

static const char *key_name(const KeyDef *k) {
    if (k->key) return k->key;
    if (!strcmp(k->section, "hotkeys"))
        return pc_hotkey_names[(k->off - F(hotkey)) / PC_BIND_MAX];
    if (!strcmp(k->section, "keyboard"))
        return pc_button_names[(k->off - F(key_bind)) / PC_BIND_MAX];
    return pc_button_names[(k->off - F(pad_bind)) / PC_BIND_MAX];
}

static const char *section_comment(const char *section) {
    if (!strcmp(section, "keyboard"))
        return "PS1 button = comma-separated SDL key names (see PLAYER-GUIDE.md).\n"
               "The game confirms with circle and cancels with cross.\n"
               "Keys held together with Ctrl, Alt or the Windows/Command key never\n"
               "press game buttons, so the hotkeys below cannot leak into the game.";
    if (!strcmp(section, "controller"))
        return "PS1 button = comma-separated SDL GameController names: a b x y back\n"
               "start guide leftstick rightstick leftshoulder rightshoulder dpup\n"
               "dpdown dpleft dpright, or an axis with a sign: +leftx -lefty\n"
               "+rightx -righty +lefttrigger +righttrigger.";
    if (!strcmp(section, "hotkeys"))
        return "PC-side keys (never seen by the game). Modifiers: Ctrl, Alt, Shift, GUI.\n"
               "menu opens the in-game settings menu (docs/pc-port/PLAYER-GUIDE.md).\n"
               "quicksave / quickload save and load the selected quicksave slot (1-9;\n"
               "slot 0 is the automatic undo of the last quickload); quick_slot_next /\n"
               "quick_slot_prev select a slot and show the slot list; saves opens the\n"
               "save browser (every save with its picture, date and stats).\n"
               "backlog shows the dialogue backlog and voice_replay replays the last\n"
               "voice line (enhanced profile); status opens the read-only status page\n"
               "(girls, bombs, parameters, calendar; enhanced profile); rewind is held\n"
               "to step back in time (enhanced profile, [enhanced] rewind); controls\n"
               "shows which key and controller button each PS1 button is.";
    if (!strcmp(section, "saves"))
        return "PC save states (docs/pc-port/PLAYER-GUIDE.md, \"Saves\"). The game's own\n"
               "memory-card saves are separate and always work.";
    if (!strcmp(section, "enhanced"))
        return "PC-side play features of the enhanced profile (docs/pc-port/ENHANCED-MODE.md).";
    if (!strcmp(section, "game"))
        return "Game options that apply in both profiles.";
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Value parsing                                                       */
/* ------------------------------------------------------------------ */

static int ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int parse_bool(const char *v, int *out) {
    if (ieq(v, "true") || ieq(v, "yes") || ieq(v, "on") || !strcmp(v, "1")) { *out = 1; return 0; }
    if (ieq(v, "false") || ieq(v, "no") || ieq(v, "off") || !strcmp(v, "0")) { *out = 0; return 0; }
    return -1;
}

static const KeyDef *find_key(const char *section, const char *key) {
    int i;
    for (i = 0; i < NKEYS; i++)
        if (ieq(g_keys[i].section, section) && ieq(key_name(&g_keys[i]), key))
            return &g_keys[i];
    return NULL;
}

/* 0 = stored; -1 = bad value (the field is unchanged). */
static int store(PcSettings *s, const KeyDef *k, const char *v) {
    char *base = (char *)s + k->off;
    char *end;
    switch (k->type) {
    case T_BOOL: {
        int b;
        if (parse_bool(v, &b)) return -1;
        *(int *)base = b;
        return 0;
    }
    case T_INT: {
        long n;
        errno = 0;
        n = strtol(v, &end, 10);
        if (errno || end == v || *end || n < (long)k->lo || n > (long)k->hi) return -1;
        *(int *)base = (int)n;
        return 0;
    }
    case T_FLOAT: {
        double d;
        errno = 0;
        d = strtod(v, &end);
        if (errno || end == v || *end || !(d >= k->lo && d <= k->hi)) return -1;
        *(double *)base = d;
        return 0;
    }
    case T_STR:
        if (strlen(v) >= k->size) return -1;
        strcpy(base, v);
        return 0;
    case T_ENUM: {
        int i;
        for (i = 0; k->names[i]; i++)
            if (ieq(k->names[i], v)) { *(int *)base = i; return 0; }
        return -1;
    }
    }
    return -1;
}

static void format_value(const PcSettings *s, const KeyDef *k, char *out, size_t n) {
    const char *base = (const char *)s + k->off;
    switch (k->type) {
    case T_BOOL:  snprintf(out, n, "%s", *(const int *)base ? "true" : "false"); break;
    case T_INT:   snprintf(out, n, "%d", *(const int *)base); break;
    case T_FLOAT: snprintf(out, n, "%g", *(const double *)base); break;
    case T_STR:   snprintf(out, n, "\"%s\"", base); break;
    case T_ENUM:  snprintf(out, n, "%s", k->names[*(const int *)base]); break;
    }
}

static char *trim(char *p) {
    char *e;
    while (*p && isspace((unsigned char)*p)) p++;
    e = p + strlen(p);
    while (e > p && isspace((unsigned char)e[-1])) *--e = 0;
    return p;
}

/* Strip quotes from a value, or an unquoted trailing # / ; comment. */
static char *unquote(char *v) {
    size_t n = strlen(v);
    if (n >= 2 && (v[0] == '"' || v[0] == '\'')) {
        char *close = strchr(v + 1, v[0]);
        if (close) { *close = 0; return v + 1; }
    }
    {
        char *c = v;
        while ((c = strpbrk(c, "#;")) != NULL) {
            if (c == v || isspace((unsigned char)c[-1])) { *c = 0; break; }
            c++;
        }
    }
    return trim(v);
}

/* Track which binding rows were given explicitly (the layout rule). */
static unsigned g_explicit_pad;

static int set_in_section(PcSettings *s, const char *section, const char *key,
                          const char *value, const char *origin, int line) {
    const KeyDef *k;
    /* [state]: the program's own values (window place, one-time hints),
     * read by pc_settings_state_get, never settings */
    if (ieq(section, PC_SETTINGS_STATE_SECTION)) return 0;
    k = find_key(section, key);
    if (!k) {
        fprintf(stderr, "[SETTINGS] %s:%d: unknown key %s%s%s (ignored)\n", origin, line,
                section, *section ? "." : "", key);
        return 1;
    }
    if (store(s, k, value)) {
        fprintf(stderr, "[SETTINGS] %s:%d: bad value '%s' for %s%s%s (kept default)\n",
                origin, line, value, section, *section ? "." : "", key);
        return 1;
    }
    if (!strcmp(k->section, "controller"))
        g_explicit_pad |= 1u << ((k->off - F(pad_bind)) / PC_BIND_MAX);
    return 0;
}

/* controller_layout only changes the cross/circle DEFAULTS. */
static void apply_layout(PcSettings *s) {
    if (s->controller_layout != PC_LAYOUT_CONFIRM_SOUTH) return;
    if (!(g_explicit_pad & (1u << PC_BTN_CIRCLE))) strcpy(s->pad_bind[PC_BTN_CIRCLE], "a");
    if (!(g_explicit_pad & (1u << PC_BTN_CROSS))) strcpy(s->pad_bind[PC_BTN_CROSS], "b");
}

void pc_settings_defaults(PcSettings *s) {
    int i;
    memset(s, 0, sizeof(*s));
    for (i = 0; i < NKEYS; i++)
        if (store(s, &g_keys[i], g_keys[i].def))
            fprintf(stderr, "[SETTINGS] internal: bad default for %s\n", key_name(&g_keys[i]));
    g_explicit_pad = 0;
}

int pc_settings_parse(PcSettings *s, const char *text, const char *origin) {
    char section[32] = "";
    char line[1024];
    int lineno = 0, warn = 0;
    const char *p = text;
    while (*p) {
        size_t n = strcspn(p, "\n");
        char *l, *eq;
        lineno++;
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, p, n);
        line[n] = 0;
        p += strcspn(p, "\n");
        if (*p) p++;
        l = trim(line);
        if (!*l || *l == '#' || *l == ';') continue;
        if (*l == '[') {
            char *close = strchr(l, ']');
            if (!close || close - l - 1 >= (long)sizeof(section)) {
                fprintf(stderr, "[SETTINGS] %s:%d: bad section header\n", origin, lineno);
                warn++;
                section[0] = 0;
                continue;
            }
            *close = 0;
            snprintf(section, sizeof(section), "%s", trim(l + 1));
            continue;
        }
        eq = strchr(l, '=');
        if (!eq) {
            fprintf(stderr, "[SETTINGS] %s:%d: expected key = value\n", origin, lineno);
            warn++;
            continue;
        }
        *eq = 0;
        warn += set_in_section(s, section, trim(l), unquote(trim(eq + 1)), origin, lineno);
    }
    apply_layout(s);
    s->warnings += warn;
    return warn;
}

int pc_settings_set(PcSettings *s, const char *dotted, const char *value) {
    char section[64], key[64];
    const char *dot = strchr(dotted, '.');
    char v[PC_SETTINGS_PATH_MAX];
    int r;
    if (dot) {
        if ((size_t)(dot - dotted) >= sizeof(section)) return -1;
        memcpy(section, dotted, (size_t)(dot - dotted));
        section[dot - dotted] = 0;
        snprintf(key, sizeof(key), "%s", dot + 1);
    } else {
        section[0] = 0;
        snprintf(key, sizeof(key), "%s", dotted);
    }
    snprintf(v, sizeof(v), "%s", value);
    r = set_in_section(s, section, trim(key), unquote(trim(v)), "TOKI_SET", 0);
    apply_layout(s);
    return r ? -1 : 0;
}

int pc_settings_apply_overrides(PcSettings *s, const char *list) {
    char item[PC_SETTINGS_PATH_MAX + 64];
    int warn = 0;
    while (list && *list) {
        size_t n = strcspn(list, ";\n");
        char *eq, *it;
        if (n >= sizeof(item)) n = sizeof(item) - 1;
        memcpy(item, list, n);
        item[n] = 0;
        list += strcspn(list, ";\n");
        if (*list) list++;
        it = trim(item);
        if (!*it) continue;
        eq = strchr(it, '=');
        if (!eq) {
            fprintf(stderr, "[SETTINGS] TOKI_SET: expected section.key=value, got '%s'\n", it);
            warn++;
            continue;
        }
        *eq = 0;
        if (pc_settings_set(s, trim(it), eq + 1)) warn++;
    }
    s->warnings += warn;
    return warn;
}

static void write_comment(FILE *f, const char *c) {
    while (c && *c) {
        size_t n = strcspn(c, "\n");
        fprintf(f, "# %.*s\n", (int)n, c);
        c += n;
        if (*c) c++;
    }
}

int pc_settings_write(const PcSettings *s, const char *path) {
    FILE *f = fopen(path, "w");
    const char *section = "";
    int i;
    if (!f) return -1;
    fprintf(f, "# Heartthrob Memorial PC Enhanced -- player settings.\n"
               "# Edit while the game is closed, or use the in-game menu (Ctrl+O);\n"
               "# delete this file to restore defaults.\n"
               "# Reference: docs/pc-port/PLAYER-GUIDE.md\n\n");
    for (i = 0; i < NKEYS; i++) {
        const KeyDef *k = &g_keys[i];
        char v[PC_SETTINGS_PATH_MAX + 4];
        if (strcmp(k->section, section)) {
            section = k->section;
            fprintf(f, "\n[%s]\n", section);
            write_comment(f, section_comment(section));
        }
        if (k->comment) write_comment(f, k->comment);
        format_value(s, k, v, sizeof(v));
        fprintf(f, "%s = %s\n", key_name(k), v);
    }
    return fclose(f) ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* Writing single keys back (the settings menu)                        */
/* ------------------------------------------------------------------ */

static const KeyDef *find_dotted(const char *dotted, char *section, size_t sn,
                                 const char **key) {
    const char *dot = strchr(dotted, '.');
    if (dot) {
        if ((size_t)(dot - dotted) >= sn) return NULL;
        memcpy(section, dotted, (size_t)(dot - dotted));
        section[dot - dotted] = 0;
        *key = dot + 1;
    } else {
        section[0] = 0;
        *key = dotted;
    }
    return find_key(section, *key);
}

int pc_settings_format(const PcSettings *s, const char *dotted, char *out, unsigned n) {
    char section[64];
    const char *key;
    const KeyDef *k = find_dotted(dotted, section, sizeof(section), &key);
    if (!k || !n) return -1;
    format_value(s, k, out, n);
    return 0;
}

/* A growable string. */
typedef struct { char *p; size_t n, cap; int oom; } Buf;
static void buf_put(Buf *b, const char *s, size_t n) {
    if (b->oom) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = (b->n + n + 1) * 2;
        char *q = (char *)realloc(b->p, cap);
        if (!q) { b->oom = 1; return; }
        b->p = q;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void buf_str(Buf *b, const char *s) { buf_put(b, s, strlen(s)); }

/* Where the trailing comment of a value (v..end) starts, by the parser's
 * rule (unquote): a # or ; after whitespace, or the first non-blank after
 * a closing quote.  NULL: none. */
static const char *value_comment(const char *v, const char *end) {
    const char *c;
    while (v < end && isspace((unsigned char)*v)) v++;
    if (v < end && (*v == '"' || *v == '\'')) {
        const char *close = (const char *)memchr(v + 1, *v, (size_t)(end - v - 1));
        if (close) {
            for (c = close + 1; c < end && isspace((unsigned char)*c); c++) { }
            return c < end ? c : NULL;
        }
    }
    for (c = v; c < end; c++)
        if ((*c == '#' || *c == ';') && c > v && isspace((unsigned char)c[-1])) return c;
    return NULL;
}

/* The section a header line (l, n bytes) names, into `out`; 0: no header. */
static int header_of(const char *l, size_t n, char *out, size_t on) {
    const char *e = l + n, *close;
    while (l < e && isspace((unsigned char)*l)) l++;
    if (l >= e || *l != '[') return 0;
    close = (const char *)memchr(l, ']', (size_t)(e - l));
    if (!close) return 0;
    l++;
    while (l < close && isspace((unsigned char)*l)) l++;
    while (close > l && isspace((unsigned char)close[-1])) close--;
    if ((size_t)(close - l) >= on) return 0;
    memcpy(out, l, (size_t)(close - l));
    out[close - l] = 0;
    return 1;
}

/* The key and '=' of a `key = value` line; 0 for anything else. */
static int key_of(const char *l, size_t n, const char **ks, size_t *kn, const char **eqp) {
    const char *e = l + n, *eq, *k;
    while (l < e && isspace((unsigned char)*l)) l++;
    if (l >= e || *l == '#' || *l == ';' || *l == '[') return 0;
    eq = (const char *)memchr(l, '=', (size_t)(e - l));
    if (!eq) return 0;
    k = eq;
    while (k > l && isspace((unsigned char)k[-1])) k--;
    *ks = l;
    *kn = (size_t)(k - l);
    *eqp = eq;
    return 1;
}

static int ieqn(const char *a, size_t an, const char *b) {
    size_t i;
    if (strlen(b) != an) return 0;
    for (i = 0; i < an; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    return 1;
}

#define NOWHERE ((size_t)-1)

char *pc_settings_edit_text(const char *text, const char *section, const char *key,
                            const char *value) {
    Buf out = { NULL, 0, 0, 0 };
    Buf line = { NULL, 0, 0, 0 };
    const char *p = text ? text : "";
    int top = section[0] == 0;
    int in_target = top;            /* lines before any header are top level */
    int seen_section = top, found = 0, any_header = 0;
    size_t insert_at = NOWHERE;     /* after the target section's last key line */
    buf_put(&out, "", 0);
    while (*p) {
        size_t n = strcspn(p, "\n");
        size_t nl = p[n] == '\n';
        const char *ks, *eq;
        size_t kn;
        char hdr[64];
        if (header_of(p, n, hdr, sizeof(hdr))) {
            if (top && !any_header && insert_at == NOWHERE)
                insert_at = out.n;  /* no top-level key yet: before the first header */
            any_header = 1;
            in_target = !top && ieq(hdr, section);
            if (in_target) {
                seen_section = 1;
                insert_at = out.n + n + nl;
            }
        } else if (in_target && key_of(p, n, &ks, &kn, &eq)) {
            if (ieqn(ks, kn, key)) {
                const char *cm = value_comment(eq + 1, p + n);
                buf_put(&out, p, (size_t)(eq - p));
                buf_str(&out, "= ");
                buf_str(&out, value);
                if (cm) {
                    buf_str(&out, "  ");
                    buf_put(&out, cm, (size_t)(p + n - cm));
                }
                if (nl) buf_str(&out, "\n");
                found = 1;
                p += n + nl;
                insert_at = out.n;
                continue;
            }
            insert_at = out.n + n + nl;
        }
        buf_put(&out, p, n + nl);
        p += n + nl;
    }
    if (!found) {
        if (!seen_section) {
            if (out.n && out.p[out.n - 1] != '\n') buf_str(&out, "\n");
            if (out.n) buf_str(&out, "\n");
            buf_str(&out, "[");
            buf_str(&out, section);
            buf_str(&out, "]\n");
            insert_at = out.n;
        } else if (insert_at == NOWHERE || insert_at >= out.n) {
            if (out.n && out.p[out.n - 1] != '\n') buf_str(&out, "\n");
            insert_at = out.n;
        }
        buf_str(&line, key);
        buf_str(&line, " = ");
        buf_str(&line, value);
        buf_str(&line, "\n");
        if (!line.oom && !out.oom) {
            size_t tail = out.n - insert_at;
            buf_put(&out, line.p, line.n);          /* grow, then shift into place */
            if (!out.oom) {
                memmove(out.p + insert_at + line.n, out.p + insert_at, tail);
                memcpy(out.p + insert_at, line.p, line.n);
            }
        }
        if (line.oom) out.oom = 1;
        free(line.p);
    }
    if (out.oom) { free(out.p); return NULL; }
    return out.p;
}

static char *read_file(const char *path);

/* `text` into `path` through path.tmp and a rename (the old file stays
 * whole when the write fails); 0 on success. */
static int write_text_atomic(const char *path, const char *text) {
    char tmp[PC_SETTINGS_PATH_MAX + 8];
    FILE *f;
    size_t len = strlen(text);
    int bad = 0;
    if (pc_settings_mkdirs(path, 0)) return -1;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f) return -1;
    if (fwrite(text, 1, len, f) != len) bad = 1;
    if (fclose(f)) bad = 1;
    if (bad || rename(tmp, path)) {
        remove(tmp);
        return -1;
    }
    return 0;
}

int pc_settings_save_keys(const PcSettings *s, const char *path,
                          const char *const *keys, int n) {
    char *text;
    int i, err = 0;
    if (s->harness || !path || !*path) return -1;
    text = read_file(path);
    if (!text) {
        text = (char *)calloc(1, 1);
        if (!text) return -1;
    }
    for (i = 0; i < n && text; i++) {
        char section[64], v[PC_SETTINGS_PATH_MAX + 4], *next;
        const char *key;
        const KeyDef *k = find_dotted(keys[i], section, sizeof(section), &key);
        if (!k) {
            fprintf(stderr, "[SETTINGS] save: unknown key %s (skipped)\n", keys[i]);
            err = 1;
            continue;
        }
        format_value(s, k, v, sizeof(v));
        next = pc_settings_edit_text(text, k->section, key_name(k), v);
        free(text);
        text = next;
    }
    if (!text) return -1;
    if (write_text_atomic(path, text)) err = 2;
    free(text);
    return err ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* Environment, paths, load                                            */
/* ------------------------------------------------------------------ */

const char *pc_settings_harness_reason(pc_getenv_fn env) {
    static const char *const vars[] = {
        "PC_TRACE", "PC_TRACE_FRAMES", "PC_INPUT", "PC_TURBO", "PC_PARITY_DIR",
        "PC_PARITY_FRAMES", "PC_SHOT_DIR", "PC_DBG_CMDS", "TOKI_INPUT_SCRIPT",
        "AUTOPLAY", "SCENE_NAV", NULL,
    };
    const char *v;
    int i;
    for (i = 0; vars[i]; i++)
        if ((v = env(vars[i])) && *v) return vars[i];
    /* TOKI_TEST_AS_PLAYER=1 (pc/tools/fresh_install.py): a headless run
     * that must otherwise behave as a player's (settings written, saves
     * and logs in the data root) */
    if ((v = env("SDL_VIDEODRIVER")) && (!strcmp(v, "dummy") || !strcmp(v, "offscreen")) &&
        !((v = env("TOKI_TEST_AS_PLAYER")) && !strcmp(v, "1")))
        return "SDL_VIDEODRIVER";
    if ((v = env("TOKI_SETTINGS")) && (ieq(v, "off") || ieq(v, "none") || !strcmp(v, "0")))
        return "TOKI_SETTINGS";
    return NULL;
}

static void join(char *out, size_t n, const char *a, const char *b) {
    snprintf(out, n, "%s/%s", a, b);
}

/* Set by pc/hal/data_root.c (pc/include/pc_data_root.h): the portable
 * folder (or TOKI_CONFIG_DIR) that holds settings and data alike; "" for
 * the per-user layout.  Defined here so the settings unit test links
 * without the data-root module. */
char pc_portable_root[1024];

void pc_settings_paths(PcSettings *s, pc_getenv_fn env) {
    const char *v, *home = env("HOME");
    char base[PC_SETTINGS_PATH_MAX];
    if (pc_portable_root[0]) {
        snprintf(s->config_dir, sizeof(s->config_dir), "%s", pc_portable_root);
        snprintf(s->data_dir, sizeof(s->data_dir), "%s", pc_portable_root);
    } else if ((v = env("TOKI_CONFIG_DIR")) && *v) {
        snprintf(s->config_dir, sizeof(s->config_dir), "%s", v);
        snprintf(s->data_dir, sizeof(s->data_dir), "%s", v);
    } else {
#if defined(_WIN32)
        v = env("APPDATA");
        join(s->config_dir, sizeof(s->config_dir), v ? v : ".", "tokimeki-pc");
        snprintf(s->data_dir, sizeof(s->data_dir), "%s", s->config_dir);
#elif defined(__APPLE__)
        snprintf(base, sizeof(base), "%s/Library/Application Support", home ? home : ".");
        join(s->config_dir, sizeof(s->config_dir), base, "tokimeki-pc");
        snprintf(s->data_dir, sizeof(s->data_dir), "%s", s->config_dir);
#else
        if ((v = env("XDG_CONFIG_HOME")) && *v == '/') snprintf(base, sizeof(base), "%s", v);
        else snprintf(base, sizeof(base), "%s/.config", home ? home : ".");
        join(s->config_dir, sizeof(s->config_dir), base, "tokimeki-pc");
        if ((v = env("XDG_DATA_HOME")) && *v == '/') snprintf(base, sizeof(base), "%s", v);
        else snprintf(base, sizeof(base), "%s/.local/share", home ? home : ".");
        join(s->data_dir, sizeof(s->data_dir), base, "tokimeki-pc");
#endif
    }
    if ((v = env("TOKI_SETTINGS")) && *v)
        snprintf(s->file_path, sizeof(s->file_path), "%s", v);
    else
        join(s->file_path, sizeof(s->file_path), s->config_dir, "settings.ini");
}

/* mkdir -p for the parent directory of `path` (or `path` itself, dir=1). */
int pc_settings_mkdirs(const char *path, int dir) {
    char tmp[PC_SETTINGS_PATH_MAX];
    char *p;
    snprintf(tmp, sizeof(tmp), "%s", path);
    if (!dir) {
        p = strrchr(tmp, '/');
#if defined(_WIN32)
        {
            /* a Windows path: either separator (C:\Users\...\saves) */
            char *b = strrchr(tmp, '\\');
            if (b && (!p || b > p)) p = b;
        }
#endif
        if (!p) return 0;
        *p = 0;
    }
    for (p = tmp + 1; *p; p++) {
#if defined(_WIN32)
        if ((*p == '/' || *p == '\\') && p[-1] != ':') {
#else
        if (*p == '/') {
#endif
            char c = *p;
            *p = 0;
            if (pc_mkdir(tmp) && errno != EEXIST) return -1;
            *p = c;
        }
    }
    if (*tmp && pc_mkdir(tmp) && errno != EEXIST) return -1;
    return 0;
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || n > 1024 * 1024 || fseek(f, 0, SEEK_SET)) {
        fclose(f);
        return NULL;
    }
    buf = (char *)malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    if (buf) buf[n] = 0;
    fclose(f);
    return buf;
}

void pc_settings_load(PcSettings *s, pc_getenv_fn env, int create_missing) {
    const char *why = pc_settings_harness_reason(env);
    char *text;
    pc_settings_defaults(s);
    pc_settings_paths(s, env);
    if (why) {
        s->harness = 1;
        snprintf(s->harness_reason, sizeof(s->harness_reason), "%s", why);
        s->file_path[0] = 0;
        /* the player default is enhanced (2026-11-04); automated runs stay
         * on parity, the battery's oracle (docs/pc-port/ENHANCED-MODE.md) */
        snprintf(s->profile, sizeof(s->profile), "%s", "parity");
        /* the player default is English (2026-11-05); automated runs keep
         * the original Japanese, which every trace and test was taken in
         * (TOKI_LANG=en still selects English for a run that wants it) */
        s->language = 0;
        /* toasts are off by default (2026-11-07); a test about them asks
         * (TOKI_SET and the file are ignored here) */
        {
            const char *t = env("TOKI_TOASTS");
            if (t && !strcmp(t, "1")) s->toasts = 1;
        }
        return;
    }
    text = read_file(s->file_path);
    if (text) {
        pc_settings_parse(s, text, s->file_path);
        free(text);
    } else if (create_missing) {
        if (pc_settings_mkdirs(s->file_path, 0) == 0 && pc_settings_write(s, s->file_path) == 0)
            s->created = 1;
        else
            fprintf(stderr, "[SETTINGS] cannot create %s; using defaults\n", s->file_path);
    }
    {
        const char *set = env("TOKI_SET");
        if (set && *set) pc_settings_apply_overrides(s, set);
    }
}

static const char *real_getenv(const char *name) { return getenv(name); }

static PcSettings g_settings;
static int g_loaded;

PcSettings *pc_settings_mut(void) {
    if (!g_loaded) {
        g_loaded = 1;
        pc_settings_load(&g_settings, real_getenv, 1);
        if (g_settings.harness)
            printf("[SETTINGS] automated run (%s): user settings ignored, defaults in use\n",
                   g_settings.harness_reason);
        else
            printf("[SETTINGS] %s %s (profile %s)\n",
                   g_settings.created ? "created default settings:" : "loaded",
                   g_settings.file_path, g_settings.profile);
        fflush(stdout);
    }
    return &g_settings;
}

const PcSettings *pc_settings_get(void) { return pc_settings_mut(); }

/* ------------------------------------------------------------------ */
/* Bindings                                                            */
/* ------------------------------------------------------------------ */

int pc_bind_tokens(const char *list, char tokens[][64], int max) {
    int n = 0;
    while (list && *list && n < max) {
        size_t len = strcspn(list, ",");
        char tmp[64];
        char *t;
        if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
        memcpy(tmp, list, len);
        tmp[len] = 0;
        t = trim(tmp);
        if (*t) snprintf(tokens[n++], 64, "%s", t);
        list += strcspn(list, ",");
        if (*list) list++;
    }
    return n;
}

const char *pc_hotkey_split(const char *token, unsigned *mods) {
    static const struct { const char *name; unsigned bit; } mod[] = {
        {"ctrl", PC_MOD_CTRL}, {"control", PC_MOD_CTRL}, {"alt", PC_MOD_ALT},
        {"option", PC_MOD_ALT}, {"shift", PC_MOD_SHIFT}, {"gui", PC_MOD_GUI},
        {"super", PC_MOD_GUI}, {"win", PC_MOD_GUI}, {"cmd", PC_MOD_GUI},
    };
    const char *p = token;
    *mods = 0;
    for (;;) {
        const char *plus = strchr(p, '+');
        size_t i, n;
        int hit = 0;
        if (!plus || plus == p || !plus[1]) break;
        n = (size_t)(plus - p);
        for (i = 0; i < sizeof(mod) / sizeof(mod[0]); i++) {
            if (strlen(mod[i].name) == n) {
                size_t j;
                for (j = 0; j < n && tolower((unsigned char)p[j]) == mod[i].name[j]; j++) { }
                if (j == n) { *mods |= mod[i].bit; hit = 1; break; }
            }
        }
        if (!hit) break;
        p = plus + 1;
    }
    return p;
}

static void keymap_add(PcKeymap *km, int code, int sign, unsigned mask) {
    if (km->n < PC_KEYMAP_MAX) {
        km->b[km->n].code = code;
        km->b[km->n].axis_sign = sign;
        km->b[km->n].mask = mask;
        km->n++;
    }
}

void pc_keymap_build_keys(PcKeymap *km, const char binds[][PC_BIND_MAX],
                          int (*lookup)(const char *name)) {
    int i, j, n, code;
    char tok[16][64];
    memset(km, 0, sizeof(*km));
    for (i = 0; i < PC_BTN_COUNT; i++) {
        n = pc_bind_tokens(binds[i], tok, 16);
        for (j = 0; j < n; j++) {
            if (ieq(tok[j], "none")) continue;
            code = lookup(tok[j]);
            if (code < 0) {
                fprintf(stderr, "[SETTINGS] keyboard.%s: unknown key '%s'\n",
                        pc_button_names[i], tok[j]);
                km->unresolved++;
                continue;
            }
            keymap_add(km, code, 0, pc_button_masks[i]);
        }
    }
}

void pc_keymap_build_pad(PcKeymap *km, const char binds[][PC_BIND_MAX],
                         int (*button_lookup)(const char *name),
                         int (*axis_lookup)(const char *name)) {
    int i, j, n, code;
    char tok[16][64];
    memset(km, 0, sizeof(*km));
    for (i = 0; i < PC_BTN_COUNT; i++) {
        n = pc_bind_tokens(binds[i], tok, 16);
        for (j = 0; j < n; j++) {
            const char *t = tok[j];
            int sign = 0;
            if (ieq(t, "none")) continue;
            if (*t == '+' || *t == '-') { sign = *t == '+' ? 1 : -1; t++; }
            code = sign ? axis_lookup(t) : button_lookup(t);
            if (code < 0) {
                fprintf(stderr, "[SETTINGS] controller.%s: unknown %s '%s'\n",
                        pc_button_names[i], sign ? "axis" : "button", tok[j]);
                km->unresolved++;
                continue;
            }
            keymap_add(km, code, sign, pc_button_masks[i]);
        }
    }
}

unsigned pc_keymap_keys(const PcKeymap *km, const unsigned char *state,
                        int nstate, int only_codes_ge) {
    unsigned m = 0;
    int i;
    for (i = 0; i < km->n; i++) {
        int c = km->b[i].code;
        if (c < 0 || c >= nstate || !state[c]) continue;
        if (only_codes_ge >= 0 && c < only_codes_ge) continue;
        m |= km->b[i].mask;
    }
    return m;
}

/* ------------------------------------------------------------------ */
/* Window geometry                                                     */
/* ------------------------------------------------------------------ */

/* The legacy window.ini (before 2026-11-07): x/y/width/height/maximized
 * under [window].  Read once, by pc_settings_fold_legacy. */
int pc_window_state_load_legacy(const char *path, PcWindowState *ws) {
    char *text = read_file(path);
    char *line, *save = NULL;
    memset(ws, 0, sizeof(*ws));
    if (!text) return -1;
    for (line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char key[32];
        int v;
        if (sscanf(line, " %31[a-z_] = %d", key, &v) != 2) continue;
        if (!strcmp(key, "x")) ws->x = v;
        else if (!strcmp(key, "y")) ws->y = v;
        else if (!strcmp(key, "width")) ws->w = v;
        else if (!strcmp(key, "height")) ws->h = v;
        else if (!strcmp(key, "maximized")) ws->maximized = v != 0;
    }
    free(text);
    ws->valid = ws->w >= 160 && ws->h >= 120 && ws->w <= 16384 && ws->h <= 16384;
    return ws->valid ? 0 : -1;
}

static const char *const g_window_keys[5] = {
    "window_x", "window_y", "window_width", "window_height", "window_maximized",
};

int pc_window_state_load(const char *path, PcWindowState *ws) {
    int v[5], i;
    char buf[32];
    memset(ws, 0, sizeof(*ws));
    for (i = 0; i < 5; i++) {
        char *end;
        if (pc_settings_state_get(path, g_window_keys[i], buf, sizeof(buf))) return -1;
        v[i] = (int)strtol(buf, &end, 10);
        if (end == buf) return -1;
    }
    ws->x = v[0];
    ws->y = v[1];
    ws->w = v[2];
    ws->h = v[3];
    ws->maximized = v[4] != 0;
    ws->valid = ws->w >= 160 && ws->h >= 120 && ws->w <= 16384 && ws->h <= 16384;
    return ws->valid ? 0 : -1;
}

int pc_window_state_save(const char *path, const PcWindowState *ws) {
    char vals[5][16];
    const char *vp[5];
    int i;
    snprintf(vals[0], sizeof vals[0], "%d", ws->x);
    snprintf(vals[1], sizeof vals[1], "%d", ws->y);
    snprintf(vals[2], sizeof vals[2], "%d", ws->w);
    snprintf(vals[3], sizeof vals[3], "%d", ws->h);
    snprintf(vals[4], sizeof vals[4], "%d", ws->maximized ? 1 : 0);
    for (i = 0; i < 5; i++) vp[i] = vals[i];
    return pc_settings_state_set(path, g_window_keys, vp, 5);
}

/* ------------------------------------------------------------------ */
/* [state]: values the program keeps for itself                        */
/* ------------------------------------------------------------------ */

int pc_settings_state_get(const char *path, const char *key, char *out, size_t n) {
    char *text, *p;
    int in_state = 0, found = 0;
    if (!path || !*path || !n) return -1;
    text = read_file(path);
    if (!text) return -1;
    for (p = text; *p && !found;) {
        size_t len = strcspn(p, "\n");
        const char *ks, *eq;
        size_t kn;
        char hdr[64];
        if (header_of(p, len, hdr, sizeof(hdr))) {
            in_state = ieq(hdr, PC_SETTINGS_STATE_SECTION);
        } else if (in_state && key_of(p, len, &ks, &kn, &eq) && ieqn(ks, kn, key)) {
            char v[PC_SETTINGS_PATH_MAX];
            size_t vn = (size_t)(p + len - (eq + 1));
            if (vn >= sizeof(v)) vn = sizeof(v) - 1;
            memcpy(v, eq + 1, vn);
            v[vn] = 0;
            snprintf(out, n, "%s", unquote(trim(v)));
            found = 1;
        }
        p += len;
        if (*p) p++;
    }
    free(text);
    return found ? 0 : -1;
}

#define STATE_COMMENT \
    "# Kept by the game itself (window place and size, one-time hints); safe\n" \
    "# to delete. The settings above are never changed by it.\n"

int pc_settings_state_set(const char *path, const char *const *keys,
                          const char *const *values, int n) {
    char *text, *next;
    const char *p;
    int i, rc, have = 0;
    if (!path || !*path) return -1;
    text = read_file(path);
    if (!text && !(text = (char *)calloc(1, 1))) return -1;
    /* a [state] section that does not exist yet starts with its note */
    for (p = text; *p && !have;) {
        size_t len = strcspn(p, "\n");
        char hdr[64];
        if (header_of(p, len, hdr, sizeof(hdr)) && ieq(hdr, PC_SETTINGS_STATE_SECTION)) have = 1;
        p += len;
        if (*p) p++;
    }
    if (!have) {
        Buf b = { NULL, 0, 0, 0 };
        size_t tn = strlen(text);
        buf_put(&b, text, tn);
        if (tn && text[tn - 1] != '\n') buf_str(&b, "\n");
        if (tn) buf_str(&b, "\n");
        buf_str(&b, STATE_COMMENT "[" PC_SETTINGS_STATE_SECTION "]\n");
        free(text);
        if (b.oom) { free(b.p); return -1; }
        text = b.p;
    }
    for (i = 0; i < n && text; i++) {
        next = pc_settings_edit_text(text, PC_SETTINGS_STATE_SECTION, keys[i], values[i]);
        free(text);
        text = next;
    }
    if (!text) return -1;
    rc = write_text_atomic(path, text);
    free(text);
    return rc;
}

int pc_settings_fold_legacy(const char *settings_path, const char *config_dir) {
    char wpath[PC_SETTINGS_PATH_MAX + 16], hpath[PC_SETTINGS_PATH_MAX + 16];
    PcWindowState ws, have;
    FILE *f;
    int folded = 0;
    if (!settings_path || !*settings_path || !config_dir || !*config_dir) return 0;
    join(wpath, sizeof(wpath), config_dir, "window.ini");
    join(hpath, sizeof(hpath), config_dir, "hints.ini");
    if ((f = fopen(wpath, "rb")) != NULL) {
        int ok = 1;
        fclose(f);
        /* an empty window.ini carries nothing to keep; a remembered window
         * already in [state] is newer than the old file */
        if (pc_window_state_load_legacy(wpath, &ws) == 0 &&
            pc_window_state_load(settings_path, &have) != 0)
            ok = pc_window_state_save(settings_path, &ws) == 0;
        if (!ok) fprintf(stderr, "[SETTINGS] cannot fold %s into %s; kept\n", wpath, settings_path);
        else if (remove(wpath) == 0) folded++;
    }
    if ((f = fopen(hpath, "rb")) != NULL) {
        char line[256];
        int shown = 0, ok = 1;
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "controls_hint", 13)) shown = 1;
        fclose(f);
        if (shown) {
            const char *k = "controls_hint", *v = "shown";
            ok = pc_settings_state_set(settings_path, &k, &v, 1) == 0;
        }
        if (!ok) fprintf(stderr, "[SETTINGS] cannot fold %s into %s; kept\n", hpath, settings_path);
        else if (remove(hpath) == 0) folded++;
    }
    return folded;
}
