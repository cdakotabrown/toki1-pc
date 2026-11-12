/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pad.c -- Input HAL via SDL2: keyboard, game controller, and a PS1 mouse.
 *
 * Two output paths:
 *   1. PadRead() -- a packed PsyQ-format u32 for code that calls libpad
 *      directly (nothing in this game's matched path does).
 *   2. The libpad response buffers pad1_buf.. / pad2_buf.. (0x800F10D0 /
 *      0x800F1110) that the PS1 BIOS refreshes once per VBLANK after
 *      StartPAD.  The matched game reads ONLY these: pad_type_check
 *      (0x80042D48) decides from bytes 0/1 whether a mouse, a pad or
 *      nothing is on port 1; mouse_pad_set (0x80042F20) turns bytes 3/4/5
 *      into the cursor and the click bits; pad_get (0x80043280) packs the
 *      pad's bytes 2/3.  pad_update_buffers() is the VBLANK poll: pc/src/
 *      main.c calls it right before pad_main() once per game tick.
 *
 * The device decision, the mouse delta bookkeeping and the record bytes
 * are in pc/hal/pad_device.c (SDL-free, unit-tested).  This file is the
 * SDL glue: it collects host input, hands it to the model, and owns the
 * window-side policy for the OS cursor.
 *
 * Mouse modes ([input] mouse_mode / TOKI_MOUSE_MODE):
 *   absolute (default since 2026-11-01): the game's hand follows the OS
 *     pointer; each poll the HAL reports the counts that move the game's own
 *     cursor under it (see "the absolute pointer" below).  The OS cursor is
 *     hidden while the game is in mouse mode and the pointer is over the
 *     picture.
 *   relative: while the GAME is in mouse mode (pad_type == 0, the detector's
 *     own verdict) and the window has focus, SDL relative mode is on -- the
 *     OS cursor is hidden and confined, the game draws its own cursor from
 *     cursor_cw_x/y, and SDL's relative motion is the mouse's motion.
 * Focus loss releases the cursor immediately and drops held buttons;
 * pressing any mapped key/pad button hands port 1 back to the pad, which
 * also releases it.  TOKI_MOUSE_GRAB=0 keeps the OS cursor visible and free.
 *

 * Bindings and the mouse/controller options come from the player's settings
 * file (pc/hal/settings.c, [keyboard] / [controller] / [input]); in an
 * automated run the settings are the defaults.  Keyboard keys held with
 * Ctrl, Alt or GUI press no game button (those chords are PC hotkeys).
 *
 * Environment (read once, at the first poll; overrides the settings file):
 *   TOKI_MOUSE=0            never present a mouse (keyboard/pad only)
 *   TOKI_MOUSE_SENS=F       host pixels -> mouse counts (default 1.0)
 *   TOKI_MOUSE_SWAP_GAP=N   empty-port polls between devices (default 1)
 *   TOKI_INPUT_DEVICE=auto|mouse|pad|none   pin port 1 (default auto)
 *   TOKI_MOUSE_MODE=absolute|relative   see above (default absolute)
 *   TOKI_MOUSE_GRAB=0       do not use relative mode / hide the OS cursor
 *   TOKI_PORT2=0|1          [input] port2_controller (the second pad on port 2)
 *   TOKI_JOYSTICK_FALLBACK=0  ignore joysticks SDL has no mapping for
 *   TOKI_INPUT_SCRIPT=PATH  scripted host input for headless runs:
 *       one event per line, `<tick> <cmd> [args]`, ticks = game ticks:
 *       mouse DX DY | lbutton 0|1 | rbutton 0|1 | pad none|NAME[,NAME..]
 *       (cross circle square triangle up down left right start select
 *        l1 r1 l2 r2; held until the next `pad` line) | pointer FX FY
 *       (the absolute pointer at picture coordinates; overrides `mouse`
 *       from then on) | shot LABEL (PC_SHOT_DIR/LABEL.ppm) | dump | quit
 *       | key KEY [KEY..] (SDL key names, Ctrl+/Alt+/Shift+/GUI+ allowed:
 *       a press and release of each is queued as SDL events, in order, so
 *       they take the same path as typed keys -- PC hotkeys and the
 *       settings menu, whose pause loop takes them one per shown frame.
 *       The keyboard STATE the game polls is not changed: game buttons
 *       come from `pad`.) | keydown KEY [KEY..] / keyup KEY [KEY..] (one
 *       key each, by SDL name, or Ctrl / Alt / Shift / GUI for the left
 *       modifier key: a key press or release as a keyboard makes it -- the
 *       SDL event, with the modifiers held at that point of the script,
 *       and the keyboard STATE the game polls, which changes when the
 *       event is taken from the queue, so a pause loop that takes the
 *       events one by one sees the keys go down and up in order)
 *       | mbutton left|right 0|1 (a mouse button press / release as an SDL
 *       event: the path a real click takes, through the presenter; the
 *       `lbutton` / `rbutton` lines set the mouse model directly) | close
 *       (the window's close button: an SDL_QUIT event)
 *   PC_DEBUG=pad            log device changes and cursor grabs
 */
#include "hal.h"
#include "pad_device.h"
#include "pad_map.h"
#include "pc_debug.h"
#include "pc_settings.h"
#include "pc_present.h"
#include "profile.h"
#include <SDL2/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PSX libpad u16 button mapping (pressed -> bit set; the response bytes
 * are the active-LOW inverse).  Low byte = byte 3, high byte = byte 2. */
#define PAD_L2      0x0001  /* byte3 bit 0 */
#define PAD_R2      0x0002  /* byte3 bit 1 */
#define PAD_L1      0x0004  /* byte3 bit 2 */
#define PAD_R1      0x0008  /* byte3 bit 3 */
#define PAD_TRI     0x0010  /* byte3 bit 4 */
#define PAD_CIRCLE  0x0020  /* byte3 bit 5 */
#define PAD_CROSS   0x0040  /* byte3 bit 6 */
#define PAD_SQUARE  0x0080  /* byte3 bit 7 */
#define PAD_SELECT  0x0100  /* byte2 bit 0 */
#define PAD_START   0x0800  /* byte2 bit 3 */
#define PAD_UP      0x1000  /* byte2 bit 4 */
#define PAD_RIGHT   0x2000  /* byte2 bit 5 */
#define PAD_DOWN    0x4000  /* byte2 bit 6 */
#define PAD_LEFT    0x8000  /* byte2 bit 7 */

/* Game state the cursor policy and the script dump read (plain names). */
extern u8  pad_type;        /* 0x800EF6D4: 0 mouse, 1 pad, 0xFF none */
extern s16 cursor_cw_x;     /* 0x801271D6 */
extern s16 cursor_cw_y;     /* 0x801271DA */
extern u32 pad_data;        /* 0x800EF6E0 */
extern u32 pad_trig;        /* 0x800EF6E8 */
extern u8  menu_mode;       /* 0x800EF7F2 */
extern u8  now_step;        /* 0x800EF868 */

static void autoplay_init(void);
static void pad_config_init(void);
static void controllers_open(void);

void PadInit(s32 mode) { (void)mode; pad_config_init(); }
void PadInitDirect(u8 *a, u8 *b) { (void)a; (void)b; }

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

static int g_cfg_done;
static pad_device_config g_dev_cfg;
static int g_grab_enabled = 1;
static int g_mouse_mode = PC_MOUSE_ABSOLUTE;   /* [input] mouse_mode / TOKI_MOUSE_MODE */
static int g_controllers_enabled = 1;
static int g_port2_enabled;         /* [input] port2_controller / TOKI_PORT2 */
static int g_fallback_enabled = 1;  /* TOKI_JOYSTICK_FALLBACK */
static int g_stick_threshold = 16000, g_trigger_threshold = 8000;
static PcKeymap g_keymap, g_padmap;

/* Settings lookups for pc_keymap_build_* (SDL names -> codes). */
static int key_lookup(const char *name) {
    SDL_Scancode sc = SDL_GetScancodeFromName(name);
    return sc == SDL_SCANCODE_UNKNOWN ? -1 : (int)sc;
}
static int button_lookup(const char *name) {
    return (int)SDL_GameControllerGetButtonFromString(name);   /* INVALID = -1 */
}
static int axis_lookup(const char *name) {
    return (int)SDL_GameControllerGetAxisFromString(name);     /* INVALID = -1 */
}

/* Auto-input for automated testing (AUTOPLAY env var). */
static int g_autoplay = 0;
static int g_auto_frame = 0;

static void autoplay_init(void) {
    const char *env = getenv("AUTOPLAY");
    if (env && env[0]) g_autoplay = 1;
}

/* ---- scripted host input (TOKI_INPUT_SCRIPT) ---- */
enum { SC_MOTION, SC_LBUTTON, SC_RBUTTON, SC_PAD, SC_DUMP, SC_QUIT, SC_SHOT, SC_POINTER, SC_KEY,
       SC_KEYDOWN, SC_KEYUP, SC_MBUTTON, SC_CLOSE };
typedef struct { unsigned tick; int kind; double a, b; u32 mask; char label[128]; } script_ev;
static script_ev *g_script;
static int g_script_n, g_script_pos;
static u32 g_script_pad;

static u32 pad_name_mask(const char *name) {
    static const struct { const char *n; u32 m; } tbl[] = {
        {"cross", PAD_CROSS}, {"circle", PAD_CIRCLE}, {"square", PAD_SQUARE},
        {"triangle", PAD_TRI}, {"up", PAD_UP}, {"down", PAD_DOWN},
        {"left", PAD_LEFT}, {"right", PAD_RIGHT}, {"start", PAD_START},
        {"select", PAD_SELECT}, {"l1", PAD_L1}, {"r1", PAD_R1},
        {"l2", PAD_L2}, {"r2", PAD_R2}, {"none", 0},
    };
    size_t i;
    for (i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++)
        if (strcmp(tbl[i].n, name) == 0) return tbl[i].m;
    fprintf(stderr, "[PAD] input script: unknown pad button '%s'\n", name);
    return 0;
}

static void script_load(const char *path) {
    FILE *f = fopen(path, "r");
    char line[256];
    int cap = 0;
    if (!f) { fprintf(stderr, "[PAD] input script: cannot open %s\n", path); return; }
    while (fgets(line, sizeof(line), f)) {
        script_ev ev; char cmd[32], arg[128]; unsigned tick; int n;
        memset(&ev, 0, sizeof(ev));
        if (line[0] == '#' || line[0] == '\n') continue;
        n = sscanf(line, "%u %31s %127[^\n]", &tick, cmd, arg);
        if (n < 2) continue;
        ev.tick = tick;
        if (!strcmp(cmd, "mouse") && n == 3) { ev.kind = SC_MOTION; sscanf(arg, "%lf %lf", &ev.a, &ev.b); }
        else if (!strcmp(cmd, "lbutton") && n == 3) { ev.kind = SC_LBUTTON; ev.a = atof(arg); }
        else if (!strcmp(cmd, "rbutton") && n == 3) { ev.kind = SC_RBUTTON; ev.a = atof(arg); }
        else if (!strcmp(cmd, "pad") && n == 3) {
            char *tok, *save = NULL;
            ev.kind = SC_PAD;
            for (tok = strtok_r(arg, ", \t", &save); tok; tok = strtok_r(NULL, ", \t", &save))
                ev.mask |= pad_name_mask(tok);
        }
        else if (!strcmp(cmd, "pointer") && n == 3) { ev.kind = SC_POINTER; sscanf(arg, "%lf %lf", &ev.a, &ev.b); }
        else if (!strcmp(cmd, "shot") && n == 3) { ev.kind = SC_SHOT; sscanf(arg, "%47s", ev.label); }
        else if (!strcmp(cmd, "key") && n == 3) { ev.kind = SC_KEY; snprintf(ev.label, sizeof(ev.label), "%s", arg); }
        else if ((!strcmp(cmd, "keydown") || !strcmp(cmd, "keyup")) && n == 3) {
            ev.kind = cmd[3] == 'd' ? SC_KEYDOWN : SC_KEYUP;
            snprintf(ev.label, sizeof(ev.label), "%s", arg);
        }
        else if (!strcmp(cmd, "mbutton") && n == 3) {
            char which[16];
            int down = 0;
            if (sscanf(arg, "%15s %d", which, &down) != 2 ||
                (strcmp(which, "left") && strcmp(which, "right"))) {
                fprintf(stderr, "[PAD] input script: bad line: %s", line);
                continue;
            }
            ev.kind = SC_MBUTTON;
            ev.a = !strcmp(which, "right");
            ev.b = down != 0;
        }
        else if (!strcmp(cmd, "close")) ev.kind = SC_CLOSE;
        else if (!strcmp(cmd, "dump")) ev.kind = SC_DUMP;
        else if (!strcmp(cmd, "quit")) ev.kind = SC_QUIT;
        else { fprintf(stderr, "[PAD] input script: bad line: %s", line); continue; }
        if (g_script_n == cap) { cap = cap ? cap * 2 : 64; g_script = realloc(g_script, cap * sizeof(*g_script)); }
        g_script[g_script_n++] = ev;
    }
    fclose(f);
    fprintf(stderr, "[PAD] input script: %d events from %s\n", g_script_n, path);
}

static const char *dev_name(int d) {
    return d == PADDEV_MOUSE ? "mouse" : d == PADDEV_PAD ? "pad" : "none";
}

static void script_dump(unsigned tick) {
    extern u8 csr_speed;        /* 0x800EF6CF: mouse_pad_set's multiplier / 3 */
    extern u8 mouse_btn_mode;   /* 0x800EF6D5: button swap */
    extern u8 card_busy;        /* 0x800EE795: pad_type_check's disconnect arm */
    printf("[PADSCRIPT] tick=%u port1=%s pad_type=%u step=%u cursor=(%d,%d) "
           "pad_data=%08x pad_trig=%08x menu_mode=%u speed=%u swap=%u busy=%u mouse=%d%d\n",
           tick, dev_name(pad_device_presented()), pad_type, now_step,
           cursor_cw_x, cursor_cw_y, pad_data, pad_trig, menu_mode,
           csr_speed, mouse_btn_mode, card_busy, pad_device_mouse_held(0),
           pad_device_mouse_held(1));
    fflush(stdout);
}

static void pointer_script(double fx, double fy);

/* pc/hal/parity.c: PC_SHOT_DIR/<label>.ppm of the scanout (observation only) */
extern int parity_shot(const char *label) __attribute__((weak));

/* `key` lines: queue a press and a release of each named key. */
static void script_keys(const char *list) {
    char buf[128], *tok, *save = NULL;
    snprintf(buf, sizeof(buf), "%s", list);
    for (tok = strtok_r(buf, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
        unsigned mods;
        const char *name = pc_hotkey_split(tok, &mods);
        SDL_Scancode sc = SDL_GetScancodeFromName(name);
        SDL_Event e;
        Uint16 m = 0;
        if (sc == SDL_SCANCODE_UNKNOWN) {
            fprintf(stderr, "[PAD] input script: unknown key '%s'\n", tok);
            continue;
        }
        if (mods & PC_MOD_CTRL) m |= KMOD_LCTRL;
        if (mods & PC_MOD_ALT) m |= KMOD_LALT;
        if (mods & PC_MOD_SHIFT) m |= KMOD_LSHIFT;
        if (mods & PC_MOD_GUI) m |= KMOD_LGUI;
        memset(&e, 0, sizeof(e));
        e.type = SDL_KEYDOWN;
        e.key.state = SDL_PRESSED;
        e.key.keysym.scancode = sc;
        e.key.keysym.sym = SDL_GetKeyFromScancode(sc);
        e.key.keysym.mod = m;
        SDL_PushEvent(&e);
        e.type = SDL_KEYUP;
        e.key.state = SDL_RELEASED;
        SDL_PushEvent(&e);
    }
}

/* `keydown` / `keyup` lines: the keyboard a script holds.  g_skey_push is
 * the state as the script has pushed it (it gives each event its
 * modifiers); g_skey the state as the events have been TAKEN from the
 * queue (pad_script_key_taken), which is what the game's poll reads -- as
 * SDL's own keyboard state changes when an event is pumped.  Both stay
 * empty, and nothing reads them, unless a script uses these lines. */
#define SKEY_WINDOW 0x7E5C0001u      /* windowID marking a script's key event */
static Uint8 g_skey[SDL_NUM_SCANCODES], g_skey_push[SDL_NUM_SCANCODES];
static int g_skey_used;

static Uint16 skey_mods(const Uint8 *k) {
    Uint16 m = 0;
    if (k[SDL_SCANCODE_LCTRL]) m |= KMOD_LCTRL;
    if (k[SDL_SCANCODE_RCTRL]) m |= KMOD_RCTRL;
    if (k[SDL_SCANCODE_LALT]) m |= KMOD_LALT;
    if (k[SDL_SCANCODE_RALT]) m |= KMOD_RALT;
    if (k[SDL_SCANCODE_LSHIFT]) m |= KMOD_LSHIFT;
    if (k[SDL_SCANCODE_RSHIFT]) m |= KMOD_RSHIFT;
    if (k[SDL_SCANCODE_LGUI]) m |= KMOD_LGUI;
    if (k[SDL_SCANCODE_RGUI]) m |= KMOD_RGUI;
    return m;
}

static SDL_Scancode skey_code(const char *name) {
    if (!SDL_strcasecmp(name, "ctrl")) return SDL_SCANCODE_LCTRL;
    if (!SDL_strcasecmp(name, "alt")) return SDL_SCANCODE_LALT;
    if (!SDL_strcasecmp(name, "shift")) return SDL_SCANCODE_LSHIFT;
    if (!SDL_strcasecmp(name, "gui")) return SDL_SCANCODE_LGUI;
    return SDL_GetScancodeFromName(name);
}

static void script_key_edges(const char *list, int down) {
    char buf[128], *tok, *save = NULL;
    snprintf(buf, sizeof(buf), "%s", list);
    for (tok = strtok_r(buf, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
        SDL_Scancode sc = skey_code(tok);
        SDL_Event e;
        if (sc == SDL_SCANCODE_UNKNOWN) {
            fprintf(stderr, "[PAD] input script: unknown key '%s'\n", tok);
            continue;
        }
        g_skey_used = 1;
        g_skey_push[sc] = (Uint8)down;
        memset(&e, 0, sizeof(e));
        e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
        e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
        e.key.windowID = SKEY_WINDOW;
        e.key.keysym.scancode = sc;
        e.key.keysym.sym = SDL_GetKeyFromScancode(sc);
        e.key.keysym.mod = skey_mods(g_skey_push);
        SDL_PushEvent(&e);
    }
}

/* Whoever takes a key event from SDL's queue (pc/hal/present.c, for every
 * event) passes it here: a script's key changes the polled state now. */
void pad_script_key_taken(const SDL_Event *e) {
    if ((e->type != SDL_KEYDOWN && e->type != SDL_KEYUP) || e->key.windowID != SKEY_WINDOW) return;
    g_skey[e->key.keysym.scancode] = e->type == SDL_KEYDOWN;
}

/* The keyboard the game's poll reads: SDL's, plus a script's keys. */
static const Uint8 *keyboard_now(int *nkeys, int *chord) {
    static Uint8 merged[SDL_NUM_SCANCODES];
    const Uint8 *k = SDL_GetKeyboardState(nkeys);
    int i, n = *nkeys;
    *chord = (SDL_GetModState() & (KMOD_CTRL | KMOD_ALT | KMOD_GUI)) != 0;
    if (!g_skey_used) return k;
    if (n > SDL_NUM_SCANCODES) n = SDL_NUM_SCANCODES;
    memset(merged, 0, sizeof merged);
    for (i = 0; i < n; i++) merged[i] = k[i];
    for (i = 0; i < SDL_NUM_SCANCODES; i++) merged[i] |= g_skey[i];
    if (skey_mods(g_skey) & (KMOD_CTRL | KMOD_ALT | KMOD_GUI)) *chord = 1;
    *nkeys = SDL_NUM_SCANCODES;
    return merged;
}

/* `mbutton`: a click as SDL delivers it */
static void script_mbutton(int right, int down) {
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    e.button.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.button.button = right ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
    e.button.clicks = 1;
    SDL_PushEvent(&e);
}

/* `close`: the window's close button */
static void script_close(void) {
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = SDL_QUIT;
    SDL_PushEvent(&e);
}

/* Apply every scripted event due at `tick` (events are in file order). */
static void script_apply(unsigned tick) {
    while (g_script_pos < g_script_n && g_script[g_script_pos].tick <= tick) {
        script_ev *ev = &g_script[g_script_pos++];
        switch (ev->kind) {
        case SC_MOTION:  pad_device_mouse_motion(ev->a, ev->b); break;
        case SC_LBUTTON: pad_device_mouse_button(0, ev->a != 0); break;
        case SC_RBUTTON: pad_device_mouse_button(1, ev->a != 0); break;
        case SC_PAD:     g_script_pad = ev->mask; break;
        case SC_DUMP:    script_dump(tick); break;
        case SC_QUIT:    hal_request_quit(); break;
        case SC_POINTER: pointer_script(ev->a, ev->b); break;
        case SC_KEY:     script_keys(ev->label); break;
        case SC_KEYDOWN: script_key_edges(ev->label, 1); break;
        case SC_KEYUP:   script_key_edges(ev->label, 0); break;
        case SC_MBUTTON: script_mbutton(ev->a != 0, ev->b != 0); break;
        case SC_CLOSE:   script_close(); break;
        case SC_SHOT:
            if (!parity_shot || !parity_shot(ev->label))
                fprintf(stderr, "[PAD] input script: shot %s not written (PC_SHOT_DIR?)\n", ev->label);
            break;
        }
    }
}

static void pad_config_init(void) {
    const char *env;
    if (g_cfg_done) return;
    g_cfg_done = 1;

    {
        const PcSettings *s = pc_settings_get();
        static const int dev[] = { PADDEV_AUTO, PADDEV_MOUSE, PADDEV_PAD, PADDEV_NONE };
        g_dev_cfg.sensitivity = s->mouse_sensitivity;
        g_dev_cfg.swap_gap = s->mouse_swap_gap;
        g_dev_cfg.force = dev[s->input_device];
        g_dev_cfg.mouse_enabled = s->mouse_enabled;
        g_grab_enabled = s->mouse_grab;
        g_mouse_mode = s->mouse_mode;
        g_controllers_enabled = s->controller_enabled;
        g_stick_threshold = s->stick_threshold;
        g_trigger_threshold = s->trigger_threshold;
        g_port2_enabled = s->port2_controller;
        pc_keymap_build_keys(&g_keymap, s->key_bind, key_lookup);
        pc_keymap_build_pad(&g_padmap, s->pad_bind, button_lookup, axis_lookup);
    }

    if ((env = getenv("TOKI_MOUSE")) && (!strcmp(env, "0") || !strcmp(env, "off")))
        g_dev_cfg.mouse_enabled = 0;
    if ((env = getenv("TOKI_MOUSE_SENS")) && atof(env) > 0.0)
        g_dev_cfg.sensitivity = atof(env);
    if ((env = getenv("TOKI_MOUSE_SWAP_GAP")) && atoi(env) >= 0)
        g_dev_cfg.swap_gap = atoi(env);
    if ((env = getenv("TOKI_INPUT_DEVICE"))) {
        if (!strcmp(env, "mouse")) g_dev_cfg.force = PADDEV_MOUSE;
        else if (!strcmp(env, "pad")) g_dev_cfg.force = PADDEV_PAD;
        else if (!strcmp(env, "none")) g_dev_cfg.force = PADDEV_NONE;
    }
    if ((env = getenv("TOKI_MOUSE_GRAB")) && !strcmp(env, "0"))
        g_grab_enabled = 0;
    if ((env = getenv("TOKI_MOUSE_MODE"))) {
        if (!strcmp(env, "absolute")) g_mouse_mode = PC_MOUSE_ABSOLUTE;
        else if (!strcmp(env, "relative")) g_mouse_mode = PC_MOUSE_RELATIVE;
        else fprintf(stderr, "[PAD] TOKI_MOUSE_MODE=%s: expected absolute or relative\n", env);
    }
    if ((env = getenv("TOKI_PORT2")) && env[0])
        g_port2_enabled = strcmp(env, "0") != 0;
    if ((env = getenv("TOKI_JOYSTICK_FALLBACK")) && !strcmp(env, "0"))
        g_fallback_enabled = 0;
    if ((env = getenv("TOKI_INPUT_SCRIPT")) && env[0])
        script_load(env);

    pad_device_init(&g_dev_cfg);
    autoplay_init();
    controllers_open();
    PC_DEBUG_PAD("config: mouse=%d mode=%s sens=%.3f swap_gap=%d device=%s grab=%d",
                 g_dev_cfg.mouse_enabled,
                 g_mouse_mode == PC_MOUSE_ABSOLUTE ? "absolute" : "relative",
                 g_dev_cfg.sensitivity, g_dev_cfg.swap_gap,
                 g_dev_cfg.force == PADDEV_AUTO ? "auto" : dev_name(g_dev_cfg.force),
                 g_grab_enabled);
}

/* ------------------------------------------------------------------ */
/* Game controllers                                                    */
/* ------------------------------------------------------------------ */

/* Every SDL game controller is opened (up to MAX_CONTROLLERS), in the
 * order they arrive.  A joystick SDL has no mapping for (a generic USB /
 * DirectInput pad, an arcade stick, a clone) gets the default mapping of
 * pc/hal/pad_map.c first, so it becomes a game controller like the rest:
 * events, the menus, [controller] bindings and the legend all work on it,
 * and the line is printed so the player can correct it in
 * gamecontrollerdb.txt.  TOKI_JOYSTICK_FALLBACK=0 turns that off.
 *
 * Ports: with [input] port2_controller off (the default) every controller
 * presses port 1's buttons, as the keyboard does -- the duplicate pads that
 * Steam Input or DS4Windows create cannot leave the game deaf.  With it on,
 * the earliest-connected controller is player 1 (port 1, with the
 * keyboard) and the next one player 2 (port 2); their LEDs say which. */
#define MAX_CONTROLLERS 8
typedef struct {
    SDL_GameController *gc;
    SDL_JoystickID id;
    unsigned seq;               /* arrival order: lower = earlier */
    int fallback;               /* on pad_map.c's default mapping */
    u32 axis_held;              /* PS1 bits its stick/trigger binds hold (hysteresis) */
    unsigned axes_centred;      /* stick axes seen near the centre since connected */
    int menu_dir[2];            /* left stick x / y as menu presses: -1, 0, +1 */
} PadCtl;
static PadCtl g_ctl[MAX_CONTROLLERS];
static unsigned g_ctl_seq;
static int g_player_dirty = 1;      /* re-assign the player LEDs */
static u32 g_port2_buttons;         /* this tick's port-2 buttons */

/* Hints, before SDL_Init (pc/hal/system.c).  Set at the DEFAULT priority:
 * the same variable in the environment wins (SDL_GetHint), so a player can
 * still change any of them.
 *
 *   SDL_GAMECONTROLLER_USE_BUTTON_LABELS=0: face buttons by POSITION on
 *     every pad.  SDL2's default reports Nintendo pads (Switch Pro, Joy-Con,
 *     GameCube) by LABEL, which would put circle -- confirm -- on the bottom
 *     button there; by position it is the right one, where the Nintendo A
 *     (confirm) is, and "a" is the bottom button on every pad as
 *     [controller] documents.
 *   SDL_JOYSTICK_HIDAPI_COMBINE_JOY_CONS=1: two Joy-Cons are one pad
 *     (SDL's default, stated).
 *   Left at SDL 2.32's defaults, all on: the HIDAPI drivers (DualShock 4,
 *   DualSense, Switch Pro / Joy-Con, GameCube adapters, Xbox, Stadia, Luna,
 *   Shield, Steam Deck, and the 8BitDo pads that present one of these) and,
 *   on Windows, XInput, DirectInput, RawInput and Windows.Gaming.Input.
 *   SDL_JOYSTICK_HIDAPI_STEAM (a Steam Controller without Steam running)
 *   is SDL's opt-in and is left to the player (docs/pc-port/PLAYER-GUIDE.md).
 *   SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS stays 0: presses made while the
 *   window has no focus are not taken, as for the keyboard. */
void pad_sdl_hints(void) {
    SDL_SetHintWithPriority(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0", SDL_HINT_DEFAULT);
    SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS, "1", SDL_HINT_DEFAULT);
}

static PadCtl *ctl_by_id(SDL_JoystickID id) {
    int i;
    for (i = 0; i < MAX_CONTROLLERS; i++)
        if (g_ctl[i].gc && g_ctl[i].id == id) return &g_ctl[i];
    return NULL;
}

/* The `max` earliest-connected controllers, earliest first: [0] is player
 * 1, [1] player 2.  Returns how many there are (at most `max`). */
static int ctl_players(PadCtl **out, int max) {
    int i, j, n = 0;
    for (i = 0; i < MAX_CONTROLLERS; i++) {
        PadCtl *c = &g_ctl[i];
        if (!c->gc) continue;
        /* insertion into the sorted prefix, dropping what falls off */
        for (j = n; j > 0 && out[j - 1]->seq > c->seq; j--)
            if (j < max) out[j] = out[j - 1];
        if (j < max) out[j] = c;
        if (n < max) n++;
    }
    return n;
}

static int controller_players(void) {
    PadCtl *p[2];
    return ctl_players(p, 2);
}

/* The PS1 port a controller presses: 2 for player 2 with port2_controller. */
static int ctl_port(const PadCtl *c) {
    PadCtl *p[2];
    if (!g_port2_enabled) return 1;
    return ctl_players(p, 2) == 2 && p[1] == c ? 2 : 1;
}

static void players_update(void) {
    int i;
    if (!g_player_dirty) return;
    g_player_dirty = 0;
    for (i = 0; i < MAX_CONTROLLERS; i++)
        if (g_ctl[i].gc) SDL_GameControllerSetPlayerIndex(g_ctl[i].gc, ctl_port(&g_ctl[i]) - 1);
}

/* The GUIDs given the default mapping this run (a device plugged in again
 * finds its mapping in place, and is still on the default one). */
#define MAX_FALLBACK_GUIDS 16
static char g_fallback_guid[MAX_FALLBACK_GUIDS][40];
static int g_fallback_n;

static int fallback_known(int idx) {
    char guid[40];
    int i;
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(idx), guid, sizeof guid);
    for (i = 0; i < g_fallback_n; i++)
        if (!strcmp(g_fallback_guid[i], guid)) return 1;
    return 0;
}

/* An unmapped joystick: give SDL the default mapping for it.  1 when the
 * device is a game controller now. */
static int fallback_map(int idx) {
    SDL_Joystick *j;
    char guid[64], line[1024];
    const char *name;
    int nb, na, nh, n;
    if (!g_fallback_enabled) return 0;
    if (SDL_JoystickGetDeviceType(idx) == SDL_JOYSTICK_TYPE_THROTTLE) return 0;
    j = SDL_JoystickOpen(idx);
    if (!j) return 0;
    nb = SDL_JoystickNumButtons(j);
    na = SDL_JoystickNumAxes(j);
    nh = SDL_JoystickNumHats(j);
    name = SDL_JoystickName(j);
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(j), guid, sizeof guid);
    n = pad_fallback_mapping(line, sizeof line, guid, name, nb, na, nh);
    /* SDL takes a gamecontrollerdb.txt line only with its platform field */
    if (n > 0 && snprintf(line + n, sizeof line - n, "platform:%s,", SDL_GetPlatform()) >=
                     (int)(sizeof line - n))
        n = -1;
    if (n > 0 && SDL_GameControllerAddMapping(line) >= 0) {
        if (g_fallback_n < MAX_FALLBACK_GUIDS)
            snprintf(g_fallback_guid[g_fallback_n++], sizeof g_fallback_guid[0], "%s", guid);
        printf("[PAD] no controller mapping for \"%s\" (%d buttons, %d axes, %d hats): "
               "using the default one; to change it, put this line, edited, in "
               "gamecontrollerdb.txt:\n[PAD]   %s\n",
               name ? name : "(unnamed)", nb, na, nh, line);
        fflush(stdout);
    }
    SDL_JoystickClose(j);
    return SDL_IsGameController(idx);
}

static void controller_open_index(int idx) {
    int i, fallback = 0;
    SDL_JoystickID id;
    if (!g_controllers_enabled || !SDL_WasInit(SDL_INIT_GAMECONTROLLER) || idx < 0 ||
        idx >= SDL_NumJoysticks())
        return;
    id = SDL_JoystickGetDeviceInstanceID(idx);
    if (id < 0 || ctl_by_id(id)) return;
    for (i = 0; i < MAX_CONTROLLERS && g_ctl[i].gc; i++) {}
    if (i == MAX_CONTROLLERS) return;
    if (!SDL_IsGameController(idx)) {
        if (!fallback_map(idx)) return;
        fallback = 1;
    } else {
        fallback = fallback_known(idx);
    }
    {
        PadCtl *c = &g_ctl[i];
        const char *name;
        char *m;
        memset(c, 0, sizeof *c);
        c->gc = SDL_GameControllerOpen(idx);
        if (!c->gc) return;
        c->id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c->gc));
        c->seq = ++g_ctl_seq;
        c->fallback = fallback;
        g_player_dirty = 1;
        players_update();
        name = SDL_GameControllerName(c->gc);
        m = SDL_GameControllerMapping(c->gc);
        printf("[PAD] controller connected: %s (%s%s)\n", name ? name : "(unnamed)",
               fallback ? "default mapping" : "mapped",
               g_port2_enabled ? (ctl_port(c) == 2 ? ", port 2" : ", port 1") : "");
        PC_DEBUG_PAD("controller %d type %d mapping %s", (int)c->id,
                     (int)SDL_GameControllerGetType(c->gc), m ? m : "-");
        if (m) SDL_free(m);
        fflush(stdout);
    }
}

static void controllers_rescan(void) {
    int n = SDL_NumJoysticks(), i;
    for (i = 0; i < n; i++) controller_open_index(i);
}

/* gamecontrollerdb.txt (SDL_GameControllerDB format): beside the program,
 * then in the settings folder (loaded last, so its lines win).  SDL itself
 * reads SDL_GAMECONTROLLERCONFIG (mapping lines) and
 * SDL_GAMECONTROLLERCONFIG_FILE (a file) from the environment at start. */
static void controller_db_load(const char *dir) {
    char path[PC_SETTINGS_PATH_MAX + 32];
    size_t len;
    FILE *f;
    int n;
    if (!dir || !(len = strlen(dir))) return;
    snprintf(path, sizeof(path), "%s%sgamecontrollerdb.txt", dir,
             dir[len - 1] == '/' || dir[len - 1] == '\\' ? "" : "/");
    if ((f = fopen(path, "r")) == NULL) return;
    fclose(f);
    n = SDL_GameControllerAddMappingsFromFile(path);
    if (n < 0)
        printf("[PAD] %s: %s\n", path, SDL_GetError());
    else
        printf("[PAD] %d controller mappings from %s\n", n, path);
    fflush(stdout);
}

static void controllers_open(void) {
    const PcSettings *s = pc_settings_get();
    if (!g_controllers_enabled || !SDL_WasInit(SDL_INIT_GAMECONTROLLER)) return;
    if (!s->harness) {
        char *base = SDL_GetBasePath();
        size_t bl = base ? strlen(base) : 0;
        /* the base path ends in a separator; the portable config dir does not */
        if (base && !(bl > 1 && !strncmp(base, s->config_dir, bl - 1) &&
                      !s->config_dir[bl - 1]))
            controller_db_load(base);
        if (base) SDL_free(base);
        controller_db_load(s->config_dir);
    }
    controllers_rescan();
}

static void controller_close_instance(SDL_JoystickID id) {
    PadCtl *c = ctl_by_id(id);
    const char *name;
    if (!c) return;
    name = SDL_GameControllerName(c->gc);
    printf("[PAD] controller disconnected: %s\n", name ? name : "(unnamed)");
    fflush(stdout);
    SDL_GameControllerClose(c->gc);
    memset(c, 0, sizeof *c);
    g_player_dirty = 1;
    players_update();
    controllers_rescan();           /* one that found no free slot before */
}

/* One controller's PS1 buttons.  A stick or trigger bind presses past its
 * threshold and lets go under three quarters of it, so a stick resting at
 * the threshold does not flicker the d-pad.  A stick axis counts only once
 * it has been seen near its centre: an axis that rests at one end (a
 * wheel's pedal or a throttle on a joystick given the default mapping)
 * does not hold a direction down forever. */
static u32 ctl_buttons(PadCtl *c) {
    u32 b = 0, axes = 0;
    int k;
    for (k = 0; k < g_padmap.n; k++) {
        const PcBind *pb = &g_padmap.b[k];
        if (pb->axis_sign == 0) {
            if (SDL_GameControllerGetButton(c->gc, (SDL_GameControllerButton)pb->code))
                b |= pb->mask;
        } else {
            int v = SDL_GameControllerGetAxis(c->gc, (SDL_GameControllerAxis)pb->code);
            int trig = pb->code == SDL_CONTROLLER_AXIS_TRIGGERLEFT ||
                       pb->code == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
            int thr = trig ? g_trigger_threshold : g_stick_threshold;
            if (!trig && pb->code >= 0 && pb->code < 16 && !(c->axes_centred & (1u << pb->code))) {
                if (v > thr / 2 || v < -thr / 2) continue;
                c->axes_centred |= 1u << pb->code;
            }
            if (c->axis_held & pb->mask) thr = thr * 3 / 4;
            if (pb->axis_sign > 0 ? v > thr : v < -thr) axes |= pb->mask;
        }
    }
    c->axis_held = axes;
    return b | axes;
}

/* The buttons of every controller on `port` (1 or 2). */
static u32 controller_buttons(int port) {
    u32 b = 0;
    int i;
    players_update();
    for (i = 0; i < MAX_CONTROLLERS; i++)
        if (g_ctl[i].gc && ctl_port(&g_ctl[i]) == port) b |= ctl_buttons(&g_ctl[i]);
    return b;
}

/* The settings menu and the backlog move with the d-pad; the left stick
 * moves them too (pc/hal/present.c): a push past the stick threshold is one
 * d-pad press -- an SDL_CONTROLLER_BUTTON_DPAD_* returned once, -1
 * otherwise -- and the stick must come back under three quarters of it
 * before the next. */
int pad_stick_menu_button(const SDL_Event *e) {
    PadCtl *c;
    int axis, v, thr, dir, *st;
    if (e->type != SDL_CONTROLLERAXISMOTION) return -1;
    axis = e->caxis.axis;
    if (axis != SDL_CONTROLLER_AXIS_LEFTX && axis != SDL_CONTROLLER_AXIS_LEFTY) return -1;
    pad_config_init();
    if (!(c = ctl_by_id(e->caxis.which))) return -1;
    st = &c->menu_dir[axis == SDL_CONTROLLER_AXIS_LEFTY];
    v = e->caxis.value;
    thr = g_stick_threshold;
    if (*st > 0 ? v > thr * 3 / 4 : *st < 0 ? v < -thr * 3 / 4 : 0) return -1;   /* still held */
    dir = v > thr ? 1 : v < -thr ? -1 : 0;
    if (dir == *st) return -1;
    *st = dir;
    if (!dir) return -1;
    if (axis == SDL_CONTROLLER_AXIS_LEFTX)
        return dir > 0 ? SDL_CONTROLLER_BUTTON_DPAD_RIGHT : SDL_CONTROLLER_BUTTON_DPAD_LEFT;
    return dir > 0 ? SDL_CONTROLLER_BUTTON_DPAD_DOWN : SDL_CONTROLLER_BUTTON_DPAD_UP;
}

/* The family whose button names the PC menus show: player 1's pad
 * (PAD_FAMILY_XBOX with none connected); its name into `name`. */
int pad_controller_family(char *name, size_t n) {
    PadCtl *p[1];
    const char *nm;
    pad_config_init();
    if (name && n) name[0] = 0;
    if (ctl_players(p, 1) < 1) return PAD_FAMILY_XBOX;
    nm = SDL_GameControllerName(p[0]->gc);
    if (name && n) snprintf(name, n, "%s", nm ? nm : "");
    if (p[0]->fallback) return PAD_FAMILY_GENERIC;
    switch (SDL_GameControllerGetType(p[0]->gc)) {
    case SDL_CONTROLLER_TYPE_PS3:
    case SDL_CONTROLLER_TYPE_PS4:
    case SDL_CONTROLLER_TYPE_PS5:
        return PAD_FAMILY_PLAYSTATION;
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        return PAD_FAMILY_NINTENDO;
    default:
        return PAD_FAMILY_XBOX;
    }
}

/* ------------------------------------------------------------------ */
/* Button sampling (keyboard + controller + autoplay + script)         */
/* ------------------------------------------------------------------ */

/* Single source of truth for the current tick's button bitmask, cached so
 * PadRead() and pad_update_buffers() agree within one tick. */
static u32 g_pad_cached = 0;
static int g_pad_frame_seen = -1;
static int g_pad_frame_now = 0;

static u32 pad_compute_buttons(void);

static u32 pad_buttons_now(void) {
    if (g_pad_frame_seen != g_pad_frame_now) {
        g_pad_cached = pad_compute_buttons();
        g_pad_frame_seen = g_pad_frame_now;
    }
    return g_pad_cached;
}

u32 PadRead(s32 port) {
    u32 buttons;
    (void)port;
    pad_config_init();
    buttons = pad_buttons_now();
    /* PsyQ format: upper byte = 0x41 (digital pad), buttons active-LOW */
    return 0x41000000 | ((~buttons) & 0xFFFF);
}

static u32 pad_compute_player_buttons(void);

/* Dialogue auto-advance (pc/hal/enhance.c, TOKI_TEXT_AUTO) adds a confirm
 * press on top of whatever the player holds: circle on a pad, the left
 * button while the game reads a mouse (pad_type 0).  Off unless asked for
 * (text_assist_confirm returns 0 and nothing here runs). */
extern int text_assist_confirm(u32 player) __attribute__((weak));
/* The intro skip (pc/hal/enhance.c, [enhanced] skip_intro) taps START on
 * the boot movie and the title animation; off unless asked for. */
extern u32 intro_assist_buttons(u32 player) __attribute__((weak));
/* Quick text (pc/hal/quick_text.c, [enhanced] quick_text): the player's
 * confirm press during a page's printing is held back (the page completes
 * at the frame's end), and one during its voice line is sent as cross,
 * the game's own cut.  Off unless asked for: the buttons pass unchanged. */
extern u32 quick_text_buttons(u32 player) __attribute__((weak));
static int g_assist_mouse;    /* the assist holds the mouse's left button */
static u32 pad_compute_buttons(void) {
    u32 buttons = pad_compute_player_buttons();
    int confirm;
    if (quick_text_buttons) buttons = quick_text_buttons(buttons);
    if (intro_assist_buttons) buttons |= intro_assist_buttons(buttons);
    confirm = text_assist_confirm ? text_assist_confirm(buttons) : 0;
    int mouse = confirm && pad_type == 0;
    if (confirm && !mouse) buttons |= PAD_CIRCLE;
    if (mouse != g_assist_mouse) {
        pad_device_mouse_button(0, mouse);
        g_assist_mouse = mouse;
    }
    return buttons;
}

/* Fast-forward input (pc/hal/system.c, the enhanced profile): ` held, or a
 * controller's right-stick click.  Not a PS1 button. */
int pad_fast_forward_held(void) {
    const u8 *keys = SDL_GetKeyboardState(NULL);
    int i;
    if (keys && keys[SDL_SCANCODE_GRAVE]) return 1;
    for (i = 0; i < MAX_CONTROLLERS; i++)
        if (g_ctl[i].gc &&
            SDL_GameControllerGetButton(g_ctl[i].gc, SDL_CONTROLLER_BUTTON_RIGHTSTICK))
            return 1;
    return 0;
}

extern int pc_menu_is_open(void) __attribute__((weak));   /* pc/hal/menu.c */
static u32 pad_compute_player_buttons(void) {
    u32 buttons = 0;
    const u8 *keys;

    /* Scripted input (PC_INPUT, pc/hal/trace.c -- the PC side of
     * pc/tools/trace_compare.py) replaces keyboard, controller, AUTOPLAY and
     * PAD_SCRIPT as the pad source while a trace script is loaded; everything
     * downstream (the matched pad chain) is unchanged. */
    {
        extern int trace_input_active(void);
        extern u32 trace_input_mask_pc(void);
        if (trace_input_active()) {
            g_port2_buttons = 0;
            return trace_input_mask_pc();
        }
    }

    if (g_autoplay) {
        g_auto_frame++;
        /* Stay quiet through boot so the game's own timeouts drive scene
         * 0 -> 18 -> 48 -> 50 -> 64 (title); at the title tap CROSS every
         * ~10 s to exercise input without flooding the chain. */
        if (now_step == 64) {
            if ((g_auto_frame % 600) >= 595) buttons |= PAD_CROSS;
        }
    }

    {
        int nkeys = 0;
        u32 held, down, ctl, p2, all;
        /* Ctrl / Alt / GUI chords are PC hotkeys: only bindings that are
         * themselves modifier keys still count while one is held. */
        int chord;
        keys = keyboard_now(&nkeys, &chord);
        ctl = controller_buttons(1);
        p2 = g_port2_enabled ? controller_buttons(2) : 0;
        down = pc_keymap_keys(&g_keymap, keys, nkeys, -1) | ctl;    /* chord or not */
        held = chord ? pc_keymap_keys(&g_keymap, keys, nkeys, SDL_SCANCODE_LCTRL) | ctl : down;
        /* While the settings menu is open (the game is normally held in
         * the presenter's pause loop then, but a HAL wait that pumps events
         * can still poll), and after it closes, the keys and buttons that
         * drive it (Escape is cross, Enter circle) are down: they reach the
         * game only once everything has been released (pad_device.c).
         * "Released" counts every bound key, also one a held Ctrl hides:
         * Ctrl+Space ending the pause, Ctrl let go first, must not end the
         * hold-off while Space is still down. */
        if (pc_menu_is_open && pc_menu_is_open()) pad_hold_off_arm();
        /* port 2's buttons wait for the same release (player 2 may have
         * closed the menu) */
        all = down | p2;
        if (pad_hold_off(all) == all) {
            buttons |= held;
            g_port2_buttons = p2;
        } else {
            g_port2_buttons = 0;
        }
    }

    buttons |= g_script_pad;
    return buttons;
}

/* ------------------------------------------------------------------ */
/* SDL events: mouse, focus, controller hot-plug                       */
/* ------------------------------------------------------------------ */

static int g_focus = 1;     /* window has keyboard focus */
static int g_grabbed = 0;   /* relative mouse mode engaged */

/* ---- the absolute pointer ([input] mouse_mode = absolute, the default) ----
 *
 * The hand follows the OS pointer: each poll, the pointer's window position
 * goes through the presenter's layout (present_window_to_frame: scale mode,
 * letter/pillarbox, high-DPI) to a point of the 320x240 picture; the cursor
 * coordinate that puts the hand's fingertip there is the target, and the
 * mouse reports the counts that move the game's own cursor accumulator
 * (mouse_pad_st.x/y, read only) onto it through mouse_pad_set's
 * `count * speed / 3` (pad_steer_counts).  The game still clamps, moves
 * and hit-tests its cursor itself; nothing writes game memory.
 *
 * Why it is the default: SDL's relative mode on X11 reads XInput2 raw
 * motion, and for a pointer device whose valuators are ABSOLUTE it turns
 * positions into motion as `previous - current` (SDL_x11xinput2.c) -- the
 * wrong sign, in device units (0..65535 across the screen).  Under WSLg
 * the pointer arrives over RDP as absolute positions and XWayland's
 * `xwayland-pointer` device has absolute valuators, so relative mode moved
 * the hand opposite to the mouse, many times too fast.  Remote desktops,
 * VMs and tablets have the same kind of device.  The absolute path reads
 * only the pointer position, which every backend reports correctly.
 *
 * Fingertip: with the cursor at (0,0) the hand sprite's fingertip is at
 * picture pixel (HAND_TIP_X0, HAND_TIP_Y0), measured from the scanout
 * (docs/pc-port/PC-PORT-MOUSE.md, "Absolute mode"); mouse_pad_set clamps
 * the cursor to x -150..140, y -100..100. */
#define HAND_TIP_X0 162
#define HAND_TIP_Y0 120
#define CURSOR_MIN_X (-150)
#define CURSOR_MAX_X 140
#define CURSOR_MIN_Y (-100)
#define CURSOR_MAX_Y 100

extern s16 mouse_x;      /* mouse_pad_st.x: mouse_pad_set's accumulator */
extern s16 mouse_y;      /* mouse_pad_st.y */
extern u8  csr_speed;       /* mouse_pad_st.speed */
extern SDL_Renderer *hal_get_renderer(void);

static int g_ptr_seen;              /* a host pointer position has been seen */
static int g_ptr_last_x, g_ptr_last_y;   /* its last window position */
static int g_ptr_script;            /* TOKI_INPUT_SCRIPT `pointer` drives it */
static double g_ptr_fx, g_ptr_fy;   /* the script's picture position */
static pad_steer_axis g_steer_x, g_steer_y;
static int g_steering;              /* steer state valid (game in mouse mode) */
static int g_cursor_hidden;         /* absolute mode hid the OS cursor */
static int g_held;                  /* the presenter holds the game (pad_hold_changed) */
static int g_ptr_inside;            /* the pointer is over the picture */

static void pointer_script(double fx, double fy) {
    g_ptr_script = 1;
    g_ptr_fx = fx;
    g_ptr_fy = fy;
    pad_device_mouse_activity();
}

/* The pointer's place on the 320x240 picture; 0 when unknown (no pointer
 * motion seen yet, the window has no focus or the pointer is elsewhere). */
static int pointer_frame(double *fx, double *fy) {
    SDL_Renderer *ren;
    SDL_Window *win;
    int x, y, ww, wh, ow, oh;
    if (g_ptr_script) {
        *fx = g_ptr_fx;
        *fy = g_ptr_fy;
        return 1;
    }
    if (!g_ptr_seen || !g_focus) return 0;
    ren = hal_get_renderer();
    win = ren ? SDL_RenderGetWindow(ren) : NULL;
    if (!win || SDL_GetMouseFocus() != win) return 0;
    SDL_GetMouseState(&x, &y);
    SDL_GetWindowSize(win, &ww, &wh);
    if (SDL_GetRendererOutputSize(ren, &ow, &oh) != 0) { ow = ww; oh = wh; }
    g_ptr_inside = present_window_to_frame(x + 0.5, y + 0.5, ww, wh, ow, oh,
                                           pc_settings_get()->scale_mode, fx, fy);
    return 1;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Before each poll: the counts that bring the game's cursor under the
 * pointer.  Only while the game itself reads a mouse (port 1 presents one
 * and pad_type_check said 0); otherwise the steering state is dropped. */
static void absolute_steer(void) {
    double fx, fy;
    int tx, ty;
    if (g_mouse_mode != PC_MOUSE_ABSOLUTE || pad_device_presented() != PADDEV_MOUSE ||
        pad_type != 0 || !pointer_frame(&fx, &fy)) {
        g_steering = 0;
        return;
    }
    if (!g_steering) {
        pad_steer_reset(&g_steer_x);
        pad_steer_reset(&g_steer_y);
        g_steering = 1;
    }
    tx = clampi((int)floor(fx) - HAND_TIP_X0, CURSOR_MIN_X, CURSOR_MAX_X);
    ty = clampi((int)floor(fy) - HAND_TIP_Y0, CURSOR_MIN_Y, CURSOR_MAX_Y);
    pad_device_mouse_counts(pad_steer_counts(&g_steer_x, mouse_x, tx, csr_speed),
                            pad_steer_counts(&g_steer_y, mouse_y, ty, csr_speed));
}

/* Absolute mode's OS cursor: hidden while the game draws its hand (mouse
 * mode, focus) and the pointer is over the picture; shown over the bars. */
static void absolute_cursor_update(void) {
    int want = g_mouse_mode == PC_MOUSE_ABSOLUTE && g_grab_enabled && g_focus && !g_held &&
               pad_type == 0 && g_ptr_seen && !g_ptr_script && g_ptr_inside;
    if (want == g_cursor_hidden) return;
    g_cursor_hidden = want;
    SDL_ShowCursor(want ? SDL_DISABLE : SDL_ENABLE);
    PC_DEBUG_PAD("OS cursor %s (absolute, pad_type=%u%s)", want ? "hidden" : "shown", pad_type,
                 g_held ? ", game held" : "");
}

void pad_handle_event(const SDL_Event *e) {
    pad_config_init();
    switch (e->type) {
    case SDL_MOUSEMOTION:
        if (e->motion.which == SDL_TOUCH_MOUSEID) break;
        if (g_mouse_mode == PC_MOUSE_ABSOLUTE) {
            /* the position is read at the poll (pointer_frame); a change of
             * position is mouse activity for the hot-switch policy */
            int x, y;
            SDL_GetMouseState(&x, &y);
            if (g_ptr_seen && (x != g_ptr_last_x || y != g_ptr_last_y))
                pad_device_mouse_activity();
            g_ptr_seen = 1;
            g_ptr_script = 0;
            g_ptr_last_x = x;
            g_ptr_last_y = y;
            break;
        }
        pad_device_mouse_motion((double)e->motion.xrel, (double)e->motion.yrel);
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        if (e->button.which == SDL_TOUCH_MOUSEID) break;
        if (e->button.button == SDL_BUTTON_LEFT)
            pad_device_mouse_button(0, e->type == SDL_MOUSEBUTTONDOWN);
        else if (e->button.button == SDL_BUTTON_RIGHT)
            pad_device_mouse_button(1, e->type == SDL_MOUSEBUTTONDOWN);
        break;
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            g_focus = 0;
            pad_device_release();
            if (g_grabbed) {
                SDL_SetRelativeMouseMode(SDL_FALSE);
                SDL_ShowCursor(SDL_ENABLE);
                g_grabbed = 0;
                PC_DEBUG_PAD("focus lost: cursor released");
            }
            absolute_cursor_update();
        } else if (e->window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
            g_focus = 1;
        }
        break;
    /* hot-plug: a game controller sends both events, a joystick SDL has no
     * mapping for only the JOY ones (it gets the default mapping) */
    case SDL_CONTROLLERDEVICEADDED:
        controller_open_index(e->cdevice.which);
        break;
    case SDL_JOYDEVICEADDED:
        controller_open_index(e->jdevice.which);
        break;
    case SDL_CONTROLLERDEVICEREMOVED:
        controller_close_instance(e->cdevice.which);
        break;
    case SDL_JOYDEVICEREMOVED:
        controller_close_instance(e->jdevice.which);
        break;
    default:
        break;
    }
}

/* Relative mode: engage SDL relative mode exactly while the game itself is
 * in mouse mode.  Absolute mode never uses it. */
static void pad_grab_update(void) {
    int want = g_mouse_mode == PC_MOUSE_RELATIVE && g_grab_enabled && g_focus && !g_held &&
               pad_type == 0;
    absolute_cursor_update();
    if (want == g_grabbed) return;
    g_grabbed = want;
    SDL_SetRelativeMouseMode(want ? SDL_TRUE : SDL_FALSE);
    SDL_ShowCursor(want ? SDL_DISABLE : SDL_ENABLE);
    PC_DEBUG_PAD("cursor %s (pad_type=%u%s)", want ? "grabbed" : "released", pad_type,
                 g_held ? ", game held" : "");
}

/* ------------------------------------------------------------------ */
/* The VBLANK poll                                                     */
/* ------------------------------------------------------------------ */

/* DIVERGENCE (enhanced profile, TOKI_MENU_MODE_KEEP; off in parity).
 * pad_type_check (0x80042D48) sets menu_mode = 0 (the free hand) when it
 * finds a mouse on port 1 and never puts the player's mode back: on the
 * console the mouse is plugged in on purpose.  Here a bumped mouse presents
 * one, and the pad then steered the hand for the rest of the game instead
 * of cycling through the options (menu_cursor_move, mode 1, the boot
 * default).  So: remember menu_mode when the mouse takes port 1 from the
 * pad, and write it back when the pad returns, unless the game or the
 * player changed it meanwhile (SELECT, the Options screen, a loaded save:
 * then it is no longer the 0 the mouse set). */
static void menu_mode_keep(int now) {
    extern u8 menu_mode;        /* 0x800EF7F2 & 3: 0 hand, 1 cycle, 2 hold, 3 nearest */
    static int last = PADDEV_PAD, kept = -1;
    if (now != PADDEV_MOUSE && now != PADDEV_PAD) return;      /* the empty-port gap */
    if (now == last) return;
    if (hal_feature_on("TOKI_MENU_MODE_KEEP")) {
        if (now == PADDEV_MOUSE) {
            kept = menu_mode;
        } else if (kept >= 0 && (menu_mode & 3) == 0) {
            menu_mode = (u8)kept;
            PC_DEBUG_PAD("menu mode %d back with the pad", kept & 3);
        }
    }
    last = now;
}

void pad_update_buffers(void) {
    int before, after;
    unsigned tick;

    pad_config_init();
    g_pad_frame_now++;                       /* new tick: resample buttons */
    tick = pad_device_ticks() + 1;
    script_apply(tick);
    pad_device_pad_buttons(pad_buttons_now());
    pad_device_port2(g_port2_enabled && controller_players() >= 2, g_port2_buttons);
    absolute_steer();

    before = pad_device_presented();
    pad_device_tick();
    after = pad_device_presented();
    if (after != before) {
        PC_DEBUG_PAD("tick %u: port 1 %s -> %s", tick, dev_name(before), dev_name(after));
        menu_mode_keep(after);
    }

    pad_grab_update();
}

/* ---- the settings menu (pc/hal/present.c) ---- */

void pad_suppress_until_release(void) {
    pad_hold_off_arm();
}

/* The presenter holds the game (pause, focus pause, the settings menu, the
 * backlog: pc/hal/present.c pc_present_pause_loop) or lets it go.  Held,
 * the game reads no input and the pointer is the player's: relative mode
 * off and the OS cursor shown, as on focus loss.  Let go, the poll's own
 * rule decides again (what the next poll would do). */
void pad_hold_changed(int held) {
    pad_config_init();
    g_held = held != 0;
    pad_grab_update();
}

/* The debug toolkit's `keys` (pc/hal/dbg_cmds.c): queue key presses as a
 * script's `key` line does. */
void pad_queue_keys(const char *list) {
    pad_config_init();
    script_keys(list);
}

/* [input] mouse_mode changed while playing. */
void pad_set_mouse_mode(int mode) {
    pad_config_init();
    g_mouse_mode = mode == PC_MOUSE_RELATIVE ? PC_MOUSE_RELATIVE : PC_MOUSE_ABSOLUTE;
    g_steering = 0;
    pad_grab_update();
}

/* The PS1 buttons the player's bindings give a key / a controller button:
 * the menu navigates with them as well as with its fixed keys. */
unsigned pad_menu_key_mask(int scancode) {
    int i;
    unsigned m = 0;
    pad_config_init();
    for (i = 0; i < g_keymap.n; i++)
        if (g_keymap.b[i].code == scancode) m |= g_keymap.b[i].mask;
    return m;
}

unsigned pad_menu_button_mask(int button) {
    int i;
    unsigned m = 0;
    pad_config_init();
    for (i = 0; i < g_padmap.n; i++)
        if (g_padmap.b[i].axis_sign == 0 && g_padmap.b[i].code == button) m |= g_padmap.b[i].mask;
    return m;
}

/* The controls legend (pc/hal/controls_view.c): the keymaps this file
 * polls, as resolved from the settings, and the controller state. */
void pad_live_bindings(const PcKeymap **keys, const PcKeymap **pad, int *controllers_on,
                       int *connected) {
    int i, n = 0;
    pad_config_init();
    for (i = 0; i < MAX_CONTROLLERS; i++)
        if (g_ctl[i].gc) n++;
    if (keys) *keys = &g_keymap;
    if (pad) *pad = &g_padmap;
    if (controllers_on) *controllers_on = g_controllers_enabled;
    if (connected) *connected = n;
}
