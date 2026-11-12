/* SPDX-License-Identifier: GPL-3.0-or-later */
/* present.c -- the PC presenter (see pc/include/pc_present.h).
 *
 * Owns the host side of the window: its first size and place (and
 * remembering them), fullscreen, the scale mode and filter used to put the
 * 320x240 scanout on screen, the scanline, toast and slot-list overlays, the
 * PC hotkeys (including the quicksave keys, pc/hal/quicksave.c),
 * screenshots, the host pause (hotkey or focus loss) and the in-game
 * settings menu (model: pc/hal/menu.c), drawn here over the finished
 * frame while the pause loop holds the game.
 *
 * The game's picture is never altered: gpu_display() still packs the same
 * scanout from VRAM and copies it with SDL_RenderCopy into the renderer's
 * 320x240 logical space; this file only chooses that space's placement and
 * draws on top of the finished frame.  Screenshots are taken from the
 * scanout itself, so they never contain scanlines or toasts.
 *
 * Every entry point is reached from system.c / gpu.c through a weak
 * reference: harness binaries that compile those files alone (the
 * presentation harness, the GPU tests) behave exactly as before.  In an
 * automated run (pc_settings: harness) the window keeps its historical
 * 640x480 and every setting is at its default, which presents exactly as
 * the port did before this module existed.
 */
#include "hal.h"
#include "pc_present.h"
#include "pc_settings.h"
#include "pc_async_io.h"
#include "pc_menu.h"
#include "pc_crt.h"
#include "pc_ui_draw.h"
#include "gpu_scanout.h"
#include <SDL2/SDL.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern u16 g_vram[512][1024];
extern void gpu_get_scanout_state(GpuScanoutState *state);
extern void gpu_display(void);
extern void hal_request_quit(void);
extern int hal_is_running(void);
extern void pad_handle_event(const SDL_Event *e);
extern void pc_watchdog_presented(void);
/* pc/hal/gpu_hires.c: the N x display, NULL at 1x; weak for the harnesses */
extern const unsigned int *gpu_hires_pack_display(int *width, int *height) __attribute__((weak));
/* pc/hal/gpu_hires.c: the scale this run draws at; weak for the harnesses */
extern int gpu_hires_running_scale(void) __attribute__((weak));
extern void gpu_pgxp_set_enabled(int on) __attribute__((weak));
/* pc/hal/gpu_texrep.c: texture dumping and packs; weak for the harnesses */
extern int gpu_texrep_set_dump(int on) __attribute__((weak));
extern int gpu_texrep_set_pack(const char *pack) __attribute__((weak));
extern int gpu_texrep_needs_restart(void) __attribute__((weak));
extern void lang_menu_register(void) __attribute__((weak));
extern const char *gpu_texrep_packs_dir(void) __attribute__((weak));
extern int gpu_texrep_reload(void) __attribute__((weak));
/* pc/hal/spu.c; weak so this file never forces the mixer into a link */
extern void spu_hal_apply_volumes(void) __attribute__((weak));
extern void spu_hal_set_paused(int paused) __attribute__((weak));
/* pc/hal/pad.c and pc/hal/profile.c, for the settings menu */
extern void pad_suppress_until_release(void) __attribute__((weak));
extern void pad_set_mouse_mode(int mode) __attribute__((weak));
extern void pad_script_key_taken(const SDL_Event *e) __attribute__((weak));
extern void pad_hold_changed(int held) __attribute__((weak));
extern unsigned pad_menu_key_mask(int scancode) __attribute__((weak));
extern unsigned pad_menu_button_mask(int button) __attribute__((weak));
extern int pad_stick_menu_button(const SDL_Event *e) __attribute__((weak));
extern const char *hal_profile_name(void) __attribute__((weak));
/* pc/hal/backlog.c / backlog_view.c: the enhanced dialogue backlog, held
 * in this pause loop like the menu; weak for the harnesses */
extern int backlog_enabled(void) __attribute__((weak));
extern int backlog_is_open(void) __attribute__((weak));
extern void backlog_open(void) __attribute__((weak));
extern void backlog_close(void) __attribute__((weak));
extern int backlog_count(void) __attribute__((weak));
extern void backlog_replay_last(void) __attribute__((weak));
extern void backlog_menu_register(void) __attribute__((weak));
extern void assist_menu_register(void) __attribute__((weak));   /* pc/hal/assist_menu.c */
extern void backlog_view_draw(SDL_Renderer *ren, int x, int y, int w, int h) __attribute__((weak));
extern int backlog_view_event(const SDL_Event *e) __attribute__((weak));
static int bl_open(void) { return backlog_is_open && backlog_is_open(); }

static SDL_Window *g_win;
static SDL_Renderer *g_ren;
static PcWindowState g_ws;
static char g_state_path[PC_SETTINGS_PATH_MAX];
static int g_layout_mode = -1;      /* scale mode the renderer is set up for */

/* host pause: hotkey and/or focus loss */
static int g_user_paused, g_focus_paused;

/* toast */
static char g_toast[64];
static Uint32 g_toast_until;
#define TOAST_MS 1600u

/* on-screen list (the quicksave slots): newline-separated lines */
static char g_osd[1200];
static Uint32 g_osd_until;

/* notes that must be read (pc_present_note): queued, timed while shown */
static PresentNotes g_notes;

/* a quicksave / quickload hotkey pressed while the game is held: it acts
 * when the game resumes (the save needs the main-loop boundary); said on
 * the pause label and the menu's footer until then */
static char g_deferred[48];

/* pc/hal/quicksave.c; weak so a harness without it still links */
extern int qs_hotkeys_enabled(void) __attribute__((weak));
extern void qs_request_save(int slot) __attribute__((weak));
extern void qs_request_load(int slot) __attribute__((weak));
extern void qs_slot_step(int delta) __attribute__((weak));
/* pc/hal/save_browser.c / save_auto.c: the saves pages, the resume offer,
 * a quit waiting for its resume state */
extern void save_browser_register(void) __attribute__((weak));
extern void sa_launch(void) __attribute__((weak));
/* pc/hal/extras_view.c: ENDINGS / GALLERY / ACHIEVEMENTS (weak: harnesses) */
extern void extras_view_register(void) __attribute__((weak));
extern void guide_menu_register(void) __attribute__((weak));   /* pc/hal/guide_view.c */
/* pc/hal/controls_view.c: the controls legend page and the one-time hint */
extern void controls_menu_register(void) __attribute__((weak));
extern int controls_page(void) __attribute__((weak));
extern void controls_first_run_hint(void) __attribute__((weak));
extern int sa_quit_pending(void) __attribute__((weak));
extern int save_browser_page(void) __attribute__((weak));
/* pc/hal/status_view.c: the enhanced status page and its HUD line */
extern void status_menu_register(void) __attribute__((weak));
extern int status_page(void) __attribute__((weak));
extern int status_hud_text(char *out, int cap) __attribute__((weak));

/* hotkeys, resolved once */
#define HK_PER 4
typedef struct { int scancode; unsigned mods; } Hotkey;
static Hotkey g_hk[PC_HK_COUNT][HK_PER];
static int g_hk_n[PC_HK_COUNT];
static int g_hk_ready;

static int g_scanlines_last = 50;   /* what the scanline toggle restores */

/* the settings menu's controller button(s) ([input] menu_button) */
static int g_menu_btn[HK_PER];
static int g_menu_btn_n;
static int g_menu_scroll;           /* first row shown */
/* the dialogue backlog's controller button(s) ([input] backlog_button) */
static int g_backlog_btn[HK_PER];
static int g_backlog_btn_n;
static void menu_builtin(void);
static int held_here(void);
static void menu_close(void);

/* PC_PRESENT_WINDOW_SHOT=PATH (debugging the presenter): the screenshot
 * hotkey also saves the next presented window, overlays included, there. */
static int g_window_shot_pending;

/* ------------------------------------------------------------------ */

static void toast(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void shot_poll_failure(void);

/* The clock of toasts and OSD lines.  A player's run: wall time.  An
 * automated run (PC_TURBO, traces, scripted input...): 1000/60 ms per
 * presented frame, so the frames a toast is shown on do not depend on how
 * fast the host is -- the presenter hashes (PC_PRESENT_HASH) of two runs,
 * or of the Linux and the Windows build, are then comparable
 * (pc/tools/windows_parity.py `menu`). */
static unsigned long g_overlay_frames;
static Uint32 overlay_now(void) {
    if (pc_settings_get()->harness) return (Uint32)(g_overlay_frames * 1000ul / 60ul);
    return SDL_GetTicks();
}
static void toast(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_toast, sizeof(g_toast), fmt, ap);
    va_end(ap);
    printf("[PRESENT] %s\n", g_toast);
    fflush(stdout);
    g_toast_until = overlay_now() + TOAST_MS;
}

/* An error toast: shown even with [system] toasts off (a note, queued). */
static void toast_err(const char *text);

/* The quicksave module's notes (it runs at the main-loop boundary). */
void pc_present_toast(const char *text) {
    snprintf(g_toast, sizeof(g_toast), "%s", text);
    g_toast_until = overlay_now() + TOAST_MS;
    g_osd_until = 0;     /* a result replaces the slot list */
}

void pc_present_osd(const char *text, unsigned ms) {
    snprintf(g_osd, sizeof(g_osd), "%s", text);
    g_osd_until = overlay_now() + ms;
    g_toast_until = 0;
}

/* A note (pc/include/pc_present.h "Notes"): queued behind the others, and
 * shown for MS of time on screen, however long the menu, the pause or
 * another message covers it first. */
void pc_present_note(const char *text, unsigned ms) {
    int r = present_notes_push(&g_notes, text, ms);
    printf("[PRESENT] note%s: %s\n", r < 0 ? " DROPPED (queue full)" : r > 0 ? " (already queued)" : "",
           text);
    fflush(stdout);
}

static void toast_err(const char *text) {
    printf("[PRESENT] %s\n", text);
    fflush(stdout);
    pc_present_note(text, 1600);   /* as long as a toast */
}

static void resolve_hotkeys(void) {
    const PcSettings *s = pc_settings_get();
    int i, j, n;
    char tok[8][64];
    if (g_hk_ready) return;
    g_hk_ready = 1;
    for (i = 0; i < PC_HK_COUNT; i++) {
        n = pc_bind_tokens(s->hotkey[i], tok, 8);
        g_hk_n[i] = 0;
        for (j = 0; j < n && g_hk_n[i] < HK_PER; j++) {
            unsigned mods;
            const char *name = pc_hotkey_split(tok[j], &mods);
            SDL_Scancode sc;
            if (!SDL_strcasecmp(tok[j], "none")) continue;
            sc = SDL_GetScancodeFromName(name);
            if (sc == SDL_SCANCODE_UNKNOWN) {
                fprintf(stderr, "[SETTINGS] hotkeys.%s: unknown key '%s'\n",
                        pc_hotkey_names[i], tok[j]);
                continue;
            }
            g_hk[i][g_hk_n[i]].scancode = (int)sc;
            g_hk[i][g_hk_n[i]].mods = mods;
            g_hk_n[i]++;
        }
    }
    n = pc_bind_tokens(s->menu_button, tok, 8);
    for (j = 0; j < n && g_menu_btn_n < HK_PER; j++) {
        SDL_GameControllerButton b;
        if (!SDL_strcasecmp(tok[j], "none")) continue;
        b = SDL_GameControllerGetButtonFromString(tok[j]);
        if (b == SDL_CONTROLLER_BUTTON_INVALID) {
            fprintf(stderr, "[SETTINGS] input.menu_button: unknown button '%s'\n", tok[j]);
            continue;
        }
        g_menu_btn[g_menu_btn_n++] = (int)b;
    }
    n = pc_bind_tokens(s->backlog_button, tok, 8);
    for (j = 0; j < n && g_backlog_btn_n < HK_PER; j++) {
        SDL_GameControllerButton b;
        if (!SDL_strcasecmp(tok[j], "none")) continue;
        b = SDL_GameControllerGetButtonFromString(tok[j]);
        if (b == SDL_CONTROLLER_BUTTON_INVALID) {
            fprintf(stderr, "[SETTINGS] input.backlog_button: unknown button '%s'\n", tok[j]);
            continue;
        }
        g_backlog_btn[g_backlog_btn_n++] = (int)b;
    }
}

static int is_backlog_button(int b) {
    int i;
    resolve_hotkeys();
    for (i = 0; i < g_backlog_btn_n; i++)
        if (g_backlog_btn[i] == b) return 1;
    return 0;
}

static int is_menu_button(int b) {
    int i;
    resolve_hotkeys();
    for (i = 0; i < g_menu_btn_n; i++)
        if (g_menu_btn[i] == b) return 1;
    return 0;
}

static unsigned mods_of(Uint16 m) {
    unsigned r = 0;
    if (m & KMOD_CTRL) r |= PC_MOD_CTRL;
    if (m & KMOD_ALT) r |= PC_MOD_ALT;
    if (m & KMOD_SHIFT) r |= PC_MOD_SHIFT;
    if (m & KMOD_GUI) r |= PC_MOD_GUI;
    return r;
}

static int hotkey_of(const SDL_KeyboardEvent *k) {
    unsigned m = mods_of(k->keysym.mod);
    int i, j;
    resolve_hotkeys();
    for (i = 0; i < PC_HK_COUNT; i++) {
        /* inert in automated runs and with PC_DBG_SLOT: the key then
         * reaches the debug toolkit's own F5 / F9 (pc/hal/debug.c) */
        if (i >= PC_HK_QUICKSAVE && i <= PC_HK_QSLOT_PREV &&
            !(qs_hotkeys_enabled && qs_hotkeys_enabled()))
            continue;
        for (j = 0; j < g_hk_n[i]; j++)
            if (g_hk[i][j].scancode == (int)k->keysym.scancode && g_hk[i][j].mods == m)
                return i;
    }
    return -1;
}

/* Is hotkey HK held now (one of its keys down with exactly its modifiers)?
 * For the hold-to-use keys (rewind, pc/hal/rewind.c); the key state is as
 * of the last event pump. */
int pc_present_hotkey_held(int hk) {
    const Uint8 *keys;
    int n = 0, j;
    unsigned m;
    if (hk < 0 || hk >= PC_HK_COUNT) return 0;
    resolve_hotkeys();
    keys = SDL_GetKeyboardState(&n);
    if (!keys) return 0;
    m = mods_of((Uint16)SDL_GetModState());
    for (j = 0; j < g_hk_n[hk]; j++)
        if (g_hk[hk][j].scancode < n && keys[g_hk[hk][j].scancode] && g_hk[hk][j].mods == m)
            return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Window                                                              */
/* ------------------------------------------------------------------ */

static int rect_on_some_display(int x, int y, int w, int h) {
    int i, n = SDL_GetNumVideoDisplays();
    for (i = 0; i < n; i++) {
        SDL_Rect b;
        if (SDL_GetDisplayUsableBounds(i, &b)) continue;
        /* at least a 64x32 grab area of the title region is visible */
        if (x + w - 64 > b.x && x + 64 < b.x + b.w && y >= b.y - 8 && y + 32 < b.y + b.h)
            return 1;
    }
    return 0;
}

void pc_present_window_params(int *w, int *h, int *x, int *y, Uint32 *flags) {
    const PcSettings *s = pc_settings_get();
    SDL_Rect ub;
    PcWindowState ws;
    const char *forced = getenv("PC_PRESENT_WINDOW");
    int fw = 0, fh = 0;
    /* PC_PRESENT_WINDOW=WxH (testing the presenter at a real size, e.g. the
     * CRT filter at 1080p): the first window size, automated runs too */
    if (!forced || sscanf(forced, "%dx%d", &fw, &fh) != 2 || fw < PRESENT_SRC_W ||
        fh < PRESENT_SRC_H)
        forced = NULL;
    if (forced) { *w = fw; *h = fh; }
    if (s->harness) return;     /* automated runs: the historical 640x480 */
    if (SDL_GetDisplayUsableBounds(0, &ub) == 0) present_auto_window(ub.w, ub.h, w, h);
    else present_auto_window(0, 0, w, h);
    if (s->window_width >= 160 && s->window_height >= 120) {
        *w = s->window_width;
        *h = s->window_height;
    }
    /* [state] of settings.ini (pc/include/pc_settings.h) */
    snprintf(g_state_path, sizeof(g_state_path), "%s", s->file_path);
    if (s->remember_window && pc_window_state_load(g_state_path, &ws) == 0 &&
        (SDL_GetDisplayUsableBounds(0, &ub) != 0 || SDL_GetNumVideoDisplays() > 1 ||
         (ws.w <= ub.w && ws.h <= ub.h))) {   /* a too-big remembered window is forgotten */
        *w = ws.w;
        *h = ws.h;
        if (rect_on_some_display(ws.x, ws.y, ws.w, ws.h)) { *x = ws.x; *y = ws.y; }
        if (ws.maximized) *flags |= SDL_WINDOW_MAXIMIZED;
    }
    if (forced) { *w = fw; *h = fh; }
    if (s->fullscreen) *flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    /* desktop fullscreen should not minimise when another window is used */
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    printf("[PRESENT] window %dx%d%s\n", *w, *h, s->fullscreen ? " (fullscreen)" : "");
}

Uint32 pc_present_renderer_flags(void) {
    return pc_settings_get()->vsync ? SDL_RENDERER_PRESENTVSYNC : 0;
}

static void apply_layout(void) {
    const PcSettings *s = pc_settings_get();
    if (!g_ren) return;
    if (s->scale_mode == PC_SCALE_STRETCH) {
        int w, h;
        SDL_RenderSetLogicalSize(g_ren, 0, 0);
        if (SDL_GetRendererOutputSize(g_ren, &w, &h) == 0)
            SDL_RenderSetScale(g_ren, (float)w / PRESENT_SRC_W, (float)h / PRESENT_SRC_H);
    } else {
        SDL_RenderSetLogicalSize(g_ren, PRESENT_SRC_W, PRESENT_SRC_H);
        SDL_RenderSetIntegerScale(g_ren, s->scale_mode == PC_SCALE_INTEGER ? SDL_TRUE : SDL_FALSE);
    }
    g_layout_mode = s->scale_mode;
}

void pc_present_attach(SDL_Window *win, SDL_Renderer *ren) {
    const PcSettings *s = pc_settings_get();
    g_win = win;
    g_ren = ren;
    resolve_hotkeys();
    if (win) {
        Uint32 f = SDL_GetWindowFlags(win);
        SDL_GetWindowPosition(win, &g_ws.x, &g_ws.y);
        SDL_GetWindowSize(win, &g_ws.w, &g_ws.h);
        g_ws.maximized = (f & SDL_WINDOW_MAXIMIZED) != 0;
        g_ws.valid = !(f & SDL_WINDOW_FULLSCREEN_DESKTOP);
        SDL_SetWindowMinimumSize(win, PRESENT_SRC_W, PRESENT_SRC_H);
    }
    /* The default (aspect) is what system.c already set up; only a
     * different mode touches the renderer. */
    if (s->scale_mode != PC_SCALE_ASPECT) apply_layout();
    else g_layout_mode = PC_SCALE_ASPECT;
    if (s->scanlines > 0) g_scanlines_last = s->scanlines;
    menu_builtin();
    if (save_browser_register) save_browser_register();
    if (extras_view_register) extras_view_register();
    if (guide_menu_register) guide_menu_register();     /* GUIDE: last on the main page */
    if (controls_menu_register) controls_menu_register();   /* GUIDE > Controls */
    if (sa_launch) sa_launch();         /* may open the menu on "continue?" */
    if (controls_first_run_hint) controls_first_run_hint();   /* once per install */
}

void pc_present_shutdown(void) {
    const PcSettings *s = pc_settings_get();
    /* a quit taken while the menu was open (an event drain outside the
     * pause loop: the main loop's, the toolkit's) still saves its changes */
    if (pc_menu_is_open()) menu_close();
    if (bl_open() && backlog_close) backlog_close();
    if (s->harness || !s->remember_window || !g_ws.valid || !g_state_path[0]) return;
    if (pc_window_state_save(g_state_path, &g_ws) == 0)
        printf("[PRESENT] window state saved: %s\n", g_state_path);
}

static void track_window(void) {
    Uint32 f;
    if (!g_win) return;
    f = SDL_GetWindowFlags(g_win);
    if (f & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_MINIMIZED)) return;
    g_ws.maximized = (f & SDL_WINDOW_MAXIMIZED) != 0;
    if (!g_ws.maximized) {
        SDL_GetWindowPosition(g_win, &g_ws.x, &g_ws.y);
        SDL_GetWindowSize(g_win, &g_ws.w, &g_ws.h);
    }
    g_ws.valid = 1;
}

/* ------------------------------------------------------------------ */
/* Per-frame                                                           */
/* ------------------------------------------------------------------ */

/* Before the game texture is copied: its filter, and the layout (stretch
 * needs the scale refreshed when the window size changes). */
void pc_present_frame(SDL_Renderer *ren, SDL_Texture *tex) {
    const PcSettings *s = pc_settings_get();
    if (ren != g_ren) return;
    if (tex)
        SDL_SetTextureScaleMode(tex, s->filter == PC_FILTER_LINEAR ? SDL_ScaleModeLinear
                                                                     : SDL_ScaleModeNearest);
    if (g_layout_mode != s->scale_mode || s->scale_mode == PC_SCALE_STRETCH) apply_layout();
}

static void fill(int x, int y, int w, int h) {
    SDL_Rect r;
    r.x = x; r.y = y; r.w = w; r.h = h;
    SDL_RenderFillRect(g_ren, &r);
}

/* UI text (pc/include/pc_ui.h): latin12, u output pixels a font pixel. */
static int draw_text(int x, int y, int u, unsigned rgba, int style, const char *t) {
    return ui_draw_text(g_ren, x, y, u, rgba, style, t);
}

/* PC_PRESENT_HASH=PATH (testing the presenter): one line per presented
 * window, "N HASH WxH [menu|backlog|paused|toast]", where HASH is FNV-1a
 * over the RGB24 pixels the window shows -- the game frame with every
 * presenter overlay on it -- and the tag says the pause loop showed it
 * (menu, backlog, paused) or a note / the slot list is drawn on it (toast),
 * or the status HUD line ([enhanced] status_hud) is (hud).
 * Observation only: it reads the back buffer and changes nothing.  The
 * settings menu's "closed = untouched" check compares these logs across
 * builds (pc/tools/test_settings_menu.py). */
static FILE *g_hash_log;
static int g_hash_state;            /* 0 unknown, 1 on, -1 off */
static unsigned long g_hash_frames;

static void hash_frame(SDL_Renderer *ren, int ow, int oh, int noted, int hud) {
    unsigned char *rgb;
    unsigned long h = 2166136261ul;
    size_t i, n = (size_t)ow * oh * 3;
    rgb = (unsigned char *)malloc(n);
    if (!rgb) return;
    if (SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_RGB24, rgb, ow * 3) == 0) {
        for (i = 0; i < n; i++) h = ((h ^ rgb[i]) * 16777619ul) & 0xfffffffful;
        fprintf(g_hash_log, "%lu %08lx %dx%d%s\n", g_hash_frames++, h, ow, oh,
                pc_menu_is_open() ? " menu" : bl_open() ? " backlog"
                : g_user_paused || g_focus_paused ? " paused" : noted ? " toast"
                : hud ? " hud" : "");
        fflush(g_hash_log);
    }
    free(rgb);
}

static int hash_on(void);

/* ------------------------------------------------------------------ */
/* The CRT filter (model and math: pc/include/pc_crt.h)                */
/* ------------------------------------------------------------------ */

/* TOKI_CRT / TOKI_CRT_CURVATURE override settings.ini (and work in
 * automated runs, which ignore it): a preset name, or a boolean. */
static int crt_env_mode(void) {
    const char *e = getenv("TOKI_CRT");
    int i;
    if (!e || !*e) return -1;
    for (i = PC_CRT_OFF + 1; i < PC_CRT_COUNT; i++)
        if (!SDL_strcasecmp(e, pc_crt_preset(i)->name)) return i;
    return PC_CRT_OFF;
}
static int crt_env_curve(void) {
    const char *e = getenv("TOKI_CRT_CURVATURE");
    if (!e || !*e) return -1;
    return !(!strcmp(e, "0") || !SDL_strcasecmp(e, "off") || !SDL_strcasecmp(e, "false") ||
             !SDL_strcasecmp(e, "no"));
}
static int crt_mode(void) {
    int e = crt_env_mode();
    return e >= 0 ? e : pc_settings_get()->crt;
}
static int crt_curved(void) {
    int e = crt_env_curve();
    return e >= 0 ? e : pc_settings_get()->crt_curvature;
}

#define CRT_GX 24                   /* mesh quads across / down */
#define CRT_GY 18
#define CRT_GLOW_W 80               /* the glow picture */
#define CRT_GLOW_H 60
static int g_crt_drawn;             /* this frame went through the filter */
static SDL_Texture *g_crt_mask;     /* scanline x phosphor mask, rect-sized */
static int g_crt_mask_w, g_crt_mask_h, g_crt_mask_mode;
static double g_crt_boost;
static SDL_Texture *g_crt_glow;
static unsigned int g_crt_glow_px[CRT_GLOW_W * CRT_GLOW_H];
static unsigned int *g_crt_pic_px; /* the gamma-adjusted picture */
static size_t g_crt_pic_n;
static unsigned char g_crt_lut[256];
static double g_crt_lut_gamma;
/* PC_PRESENT_CRT_STATS=N: every N filtered frames, the mean time of the
 * filter's own work and of waiting for the renderer to finish it */
static int g_crt_stats = -1;
static double g_crt_cpu_ms, g_crt_sync_ms;
static int g_crt_stat_n;

/* Flat, each pass is a plain SDL_RenderCopy into the rectangle (the fast
 * path on every renderer, the software one included); curved, it goes
 * through a bent vertex mesh (SDL_RenderGeometry, SDL 2.0.18+), so the
 * scanlines and the mask bend with the picture. */
static SDL_Rect g_crt_dst;
static int g_crt_curved;
#define CRT_VIG_W 64                /* the vignette, stretched smooth */
#define CRT_VIG_H 48
static SDL_Texture *g_crt_vig;
static int g_crt_vig_mode;
#if SDL_VERSION_ATLEAST(2, 0, 18)
#define CRT_CAN_CURVE 1
static SDL_Vertex g_crt_vtx[(CRT_GX + 1) * (CRT_GY + 1)];
static int g_crt_idx[CRT_GX * CRT_GY * 6];
#else
#define CRT_CAN_CURVE 0             /* no geometry: always flat */
#endif

static void crt_mesh(PresentRect r, int curved) {
    g_crt_dst.x = r.x;
    g_crt_dst.y = r.y;
    g_crt_dst.w = r.w;
    g_crt_dst.h = r.h;
    g_crt_curved = curved && CRT_CAN_CURVE;
#if SDL_VERSION_ATLEAST(2, 0, 18)
    if (g_crt_curved) {
        int i, j, k = 0;
        for (j = 0; j <= CRT_GY; j++)
            for (i = 0; i <= CRT_GX; i++) {
                SDL_Vertex *v = &g_crt_vtx[j * (CRT_GX + 1) + i];
                double u = (double)i / CRT_GX, t = (double)j / CRT_GY, x, y;
                pc_crt_warp(u, t, PC_CRT_CURVE, &x, &y);
                v->position.x = (float)(r.x + x * r.w);
                v->position.y = (float)(r.y + y * r.h);
                v->tex_coord.x = (float)u;
                v->tex_coord.y = (float)t;
            }
        for (j = 0; j < CRT_GY; j++)
            for (i = 0; i < CRT_GX; i++) {
                int a = j * (CRT_GX + 1) + i, b = a + 1, c = a + CRT_GX + 1, d = c + 1;
                g_crt_idx[k++] = a; g_crt_idx[k++] = b; g_crt_idx[k++] = c;
                g_crt_idx[k++] = b; g_crt_idx[k++] = d; g_crt_idx[k++] = c;
            }
    }
#endif
}

/* One pass: tex times the grey level c, with its blend mode already set. */
static void crt_pass(SDL_Renderer *ren, SDL_Texture *tex, Uint8 c) {
#if SDL_VERSION_ATLEAST(2, 0, 18)
    if (g_crt_curved) {
        int i, n = (CRT_GX + 1) * (CRT_GY + 1);
        for (i = 0; i < n; i++) {
            g_crt_vtx[i].color.r = g_crt_vtx[i].color.g = g_crt_vtx[i].color.b = c;
            g_crt_vtx[i].color.a = 255;
        }
        SDL_RenderGeometry(ren, tex, g_crt_vtx, n, g_crt_idx, CRT_GX * CRT_GY * 6);
        return;
    }
#endif
    SDL_SetTextureColorMod(tex, c, c, c);
    SDL_RenderCopy(ren, tex, NULL, &g_crt_dst);
    SDL_SetTextureColorMod(tex, 255, 255, 255);
}

/* The vignette texture for a preset (NULL: none). */
static SDL_Texture *crt_vignette(SDL_Renderer *ren, const PcCrtParams *p, int mode) {
    static unsigned int px[CRT_VIG_W * CRT_VIG_H];
    int x, y;
    if (p->vignette <= 0.0) return NULL;
    if (g_crt_vig && g_crt_vig_mode == mode) return g_crt_vig;
    if (!g_crt_vig) {
        g_crt_vig = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STATIC,
                                      CRT_VIG_W, CRT_VIG_H);
        if (!g_crt_vig) return NULL;
        SDL_SetTextureBlendMode(g_crt_vig, SDL_BLENDMODE_MOD);
        SDL_SetTextureScaleMode(g_crt_vig, SDL_ScaleModeLinear);
    }
    for (y = 0; y < CRT_VIG_H; y++)
        for (x = 0; x < CRT_VIG_W; x++) {
            int g = (int)(pc_crt_vignette(p, (x + 0.5) / CRT_VIG_W, (y + 0.5) / CRT_VIG_H) *
                          255.0 + 0.5);
            unsigned v = (unsigned)(g < 0 ? 0 : g > 255 ? 255 : g);
            px[y * CRT_VIG_W + x] = (v << 24) | (v << 16) | (v << 8) | 0xFFu;
        }
    SDL_UpdateTexture(g_crt_vig, NULL, px, CRT_VIG_W * 4);
    g_crt_vig_mode = mode;
    return g_crt_vig;
}

/* gpu.c, instead of copying the finished picture `tex` (px: its w x h
 * RGBA8888 pixels, `stride` per row, as the game made them -- tex holds
 * pc_present_crt_picture()'s version) into the logical space: draw it
 * through the CRT filter.  Returns 0 -- having drawn nothing -- when the
 * filter is off or the picture is under 2x; gpu.c then copies as before.
 * Reads the picture only; nothing it does reaches VRAM or the game. */
/* The preset in use for a picture presented by ren, or NULL: off, another
 * renderer, or a picture under 2x (*r: where it lands, in output pixels). */
static const PcCrtParams *crt_active(SDL_Renderer *ren, PresentRect *r, int *ow, int *oh) {
    const PcCrtParams *p = pc_crt_preset(crt_mode());
    if (!p || ren != g_ren || !ren || SDL_GetRendererOutputSize(ren, ow, oh)) return NULL;
    *r = present_layout_rect(*ow, *oh, pc_settings_get()->scale_mode);
    if (r->w < PRESENT_SRC_W * PC_CRT_MIN_SCALE || r->h < PRESENT_SRC_H * PC_CRT_MIN_SCALE)
        return NULL;
    return p;
}

/* gpu.c, before it uploads the finished picture (px: w x h RGBA8888,
 * `stride` per row) into its texture: the pixels to upload.  px itself,
 * or -- a CRT preset with a gamma -- a gamma-adjusted copy (w per row),
 * so the filter costs no extra texture upload. */
const unsigned int *pc_present_crt_picture(SDL_Renderer *ren, const unsigned int *px,
                                           int w, int h, int stride) {
    PresentRect r;
    int ow, oh, x, y;
    const PcCrtParams *p = crt_active(ren, &r, &ow, &oh);
    size_t n = (size_t)w * (h > 0 ? h : 0);
    if (!p || p->gamma == 1.0 || !px || w <= 0 || h <= 0) return px;
    if (g_crt_pic_n < n) {
        unsigned int *b = (unsigned int *)realloc(g_crt_pic_px, n * 4);
        if (!b) return px;
        g_crt_pic_px = b;
        g_crt_pic_n = n;
    }
    if (g_crt_lut_gamma != p->gamma) {
        pc_crt_gamma_lut(p->gamma, g_crt_lut);
        g_crt_lut_gamma = p->gamma;
    }
    for (y = 0; y < h; y++) {
        const unsigned int *srow = px + (size_t)y * stride;
        unsigned int *drow = g_crt_pic_px + (size_t)y * w;
        for (x = 0; x < w; x++) {
            unsigned int v = srow[x];
            drow[x] = ((unsigned)g_crt_lut[v >> 24] << 24) |
                      ((unsigned)g_crt_lut[(v >> 16) & 0xFF] << 16) |
                      ((unsigned)g_crt_lut[(v >> 8) & 0xFF] << 8) | 0xFFu;
        }
    }
    return g_crt_pic_px;
}

static void crt_stats(SDL_Renderer *ren, const char *name, Uint64 t0, int ow, int oh,
                      int w, int h) {
    Uint64 t1 = SDL_GetPerformanceCounter(), t2;
    double f = 1000.0 / (double)SDL_GetPerformanceFrequency();
    Uint32 one;
    SDL_Rect px1 = { 0, 0, 1, 1 };
    SDL_RendererInfo info;
    SDL_RenderReadPixels(ren, &px1, SDL_PIXELFORMAT_RGBA8888, &one, 4);  /* wait for the GPU */
    t2 = SDL_GetPerformanceCounter();
    g_crt_cpu_ms += (double)(t1 - t0) * f;
    g_crt_sync_ms += (double)(t2 - t1) * f;
    if (++g_crt_stat_n < g_crt_stats) return;
    if (SDL_GetRendererInfo(ren, &info)) info.name = "?";
    printf("[PRESENT] crt stats: %s in %dx%d (%s renderer), picture %dx%d: cpu %.3f ms, "
           "render wait %.3f ms (mean of %d)\n", name, ow, oh, info.name, w, h,
           g_crt_cpu_ms / g_crt_stat_n, g_crt_sync_ms / g_crt_stat_n, g_crt_stat_n);
    fflush(stdout);
    g_crt_cpu_ms = g_crt_sync_ms = 0.0;
    g_crt_stat_n = 0;
}

int pc_present_crt(SDL_Renderer *ren, SDL_Texture *tex, const unsigned int *px,
                   int w, int h, int stride) {
    int mode = crt_mode(), curved, ow, oh;
    const PcCrtParams *p;
    SDL_Texture *pic = tex;            /* already gamma-adjusted (above) */
    SDL_Texture *vig;
    SDL_BlendMode old_blend;
    Uint64 t0 = 0;
    PresentRect r;
    g_crt_drawn = 0;
    if (ren != g_ren || !ren || !tex || !px || w <= 0 || h <= 0) return 0;
    if (g_crt_stats < 0) {
        const char *e = getenv("PC_PRESENT_CRT_STATS");
        g_crt_stats = e && *e ? atoi(e) : 0;
    }
    p = crt_active(ren, &r, &ow, &oh);
    if (!p) {
        /* off: gpu.c copies the picture -- except when measuring the plain
         * path, where the same copy is done (and timed) here, as the baseline */
        if (g_crt_stats <= 0 || mode != PC_CRT_OFF || SDL_GetRendererOutputSize(ren, &ow, &oh))
            return 0;
        t0 = SDL_GetPerformanceCounter();
        SDL_RenderCopy(ren, tex, NULL, NULL);
        crt_stats(ren, "off", t0, ow, oh, w, h);
        return 1;
    }
    if (g_crt_stats > 0) t0 = SDL_GetPerformanceCounter();
    curved = crt_curved();

    /* the mask: rebuilt when the rectangle or the preset changes */
    if (!g_crt_mask || g_crt_mask_w != r.w || g_crt_mask_h != r.h || g_crt_mask_mode != mode) {
        unsigned int *m = (unsigned int *)malloc((size_t)r.w * r.h * 4);
        if (!m) return 0;
        if (g_crt_mask && (g_crt_mask_w != r.w || g_crt_mask_h != r.h)) {
            SDL_DestroyTexture(g_crt_mask);
            g_crt_mask = NULL;
        }
        if (!g_crt_mask)
            g_crt_mask = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA8888,
                                           SDL_TEXTUREACCESS_STATIC, r.w, r.h);
        if (!g_crt_mask) { free(m); return 0; }
        g_crt_boost = pc_crt_boost(p, pc_crt_build_mask(p, r.w, r.h, PRESENT_SRC_H, m));
        SDL_UpdateTexture(g_crt_mask, NULL, m, r.w * 4);
        free(m);
        SDL_SetTextureBlendMode(g_crt_mask, SDL_BLENDMODE_MOD);
        g_crt_mask_w = r.w;
        g_crt_mask_h = r.h;
        g_crt_mask_mode = mode;
        printf("[PRESENT] crt %s %dx%d (boost %.2f)\n", p->name, r.w, r.h, g_crt_boost);
        fflush(stdout);
    }
    /* curved, the mask is resampled: smooth it; flat, it maps 1:1 */
    SDL_SetTextureScaleMode(g_crt_mask, curved ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);

    /* the glow */
    if (p->glow > 0.0) {
        if (!g_crt_glow) {
            g_crt_glow = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA8888,
                                           SDL_TEXTUREACCESS_STREAMING, CRT_GLOW_W, CRT_GLOW_H);
            if (g_crt_glow) {
                SDL_SetTextureBlendMode(g_crt_glow, SDL_BLENDMODE_ADD);
                SDL_SetTextureScaleMode(g_crt_glow, SDL_ScaleModeLinear);
            }
        }
        if (g_crt_glow) {
            pc_crt_glow(px, w, h, stride, g_crt_glow_px, CRT_GLOW_W, CRT_GLOW_H);
            SDL_UpdateTexture(g_crt_glow, NULL, g_crt_glow_px, CRT_GLOW_W * 4);
        }
    }

    /* the passes, in output pixels */
    SDL_RenderSetLogicalSize(ren, 0, 0);
    SDL_RenderSetScale(ren, 1.0f, 1.0f);
    crt_mesh(r, curved);
    SDL_GetTextureBlendMode(pic, &old_blend);
    SDL_SetTextureScaleMode(pic, SDL_ScaleModeLinear);       /* a beam has no square pixels */
    SDL_SetTextureBlendMode(pic, SDL_BLENDMODE_NONE);
    crt_pass(ren, pic, 255);                                  /* 1. picture */
    crt_pass(ren, g_crt_mask, 255);                           /* 2. x mask */
    if (g_crt_boost > 0.0) {                                  /* 3. + boost */
        SDL_SetTextureBlendMode(pic, SDL_BLENDMODE_ADD);
        crt_pass(ren, pic, (Uint8)(g_crt_boost * 255.0 + 0.5));
    }
    if (p->glow > 0.0 && g_crt_glow)                          /* 4. + glow */
        crt_pass(ren, g_crt_glow, (Uint8)(p->glow * 255.0 + 0.5));
    if ((vig = crt_vignette(ren, p, mode)) != NULL)           /* 5. x vignette */
        crt_pass(ren, vig, 255);
    SDL_SetTextureBlendMode(pic, old_blend);
    apply_layout();
    g_crt_drawn = 1;

    if (g_crt_stats > 0) crt_stats(ren, p->name, t0, ow, oh, w, h);
    return 1;
}

static int hash_on(void) {
    if (!g_hash_state) {
        const char *path = getenv("PC_PRESENT_HASH");
        g_hash_state = -1;
        if (path && *path && (g_hash_log = fopen(path, "w")) != NULL) g_hash_state = 1;
    }
    return g_hash_state > 0;
}

/* ------------------------------------------------------------------ */
/* The settings menu: drawing                                          */
/* ------------------------------------------------------------------ */

/* The panel is the game's message window (pc/include/pc_ui_draw.h), in
 * menu units: one unit is a game pixel, u output pixels.  Inside the
 * 6-unit bevel: the title, a copper rule, MENU_ROWS rows of UI_ROW, a
 * rule and two lines of key help. */
#define MENU_W 304
#define MENU_H 228
#define MENU_PAD 10                 /* text inset from the panel's edge */
#define MENU_TITLE_Y 9
#define MENU_TOP 27                 /* first row below the title */
#define MENU_FOOT 33                /* footer height, the bottom bevel included */
#define MENU_ROW UI_ROW
#define MENU_ROWS ((MENU_H - MENU_TOP - MENU_FOOT) / MENU_ROW)
#define MENU_LABEL_X 20             /* after the selection heart */
#define MENU_VALUE_MIN 120          /* the value column: after the page's labels, */
#define MENU_VALUE_MAX 168          /* within these */

static void color(Uint8 r, Uint8 g, Uint8 b, Uint8 a) { SDL_SetRenderDrawColor(g_ren, r, g, b, a); }

/* A custom page's canvas (pc/include/pc_menu.h): menu units -> output
 * pixels, clipped to the panel's body. */
typedef struct { int x0, y0, u; SDL_Rect clip; } MenuCanvas;
static SDL_Texture *g_canvas_tex;
static unsigned long g_canvas_key;
static int g_canvas_tw, g_canvas_th;

static void canvas_color(unsigned rgba) {
    SDL_SetRenderDrawColor(g_ren, (Uint8)(rgba >> 24), (Uint8)(rgba >> 16), (Uint8)(rgba >> 8),
                           (Uint8)rgba);
}
static void canvas_fill(void *ctx, int x, int y, int w, int h, unsigned rgba) {
    const MenuCanvas *c = (const MenuCanvas *)ctx;
    canvas_color(rgba);
    fill(c->x0 + x * c->u, c->y0 + y * c->u, w * c->u, h * c->u);
}
static void canvas_text(void *ctx, int x, int y, unsigned rgba, const char *t) {
    const MenuCanvas *c = (const MenuCanvas *)ctx;
    draw_text(c->x0 + x * c->u, c->y0 + y * c->u, c->u, rgba, UI_SHADOW, t);
}
static void canvas_image(void *ctx, int x, int y, int w, int h, const unsigned char *rgb,
                         int iw, int ih, unsigned long key) {
    const MenuCanvas *c = (const MenuCanvas *)ctx;
    SDL_Rect dst;
    if (!g_canvas_tex || g_canvas_tw != iw || g_canvas_th != ih) {
        if (g_canvas_tex) SDL_DestroyTexture(g_canvas_tex);
        g_canvas_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING,
                                         iw, ih);
        g_canvas_tw = iw;
        g_canvas_th = ih;
        g_canvas_key = key + 1;
        if (!g_canvas_tex) return;
        SDL_SetTextureScaleMode(g_canvas_tex, SDL_ScaleModeLinear);
    }
    if (g_canvas_key != key) {
        SDL_UpdateTexture(g_canvas_tex, NULL, rgb, iw * 3);
        g_canvas_key = key;
    }
    dst.x = c->x0 + x * c->u;
    dst.y = c->y0 + y * c->u;
    dst.w = w * c->u;
    dst.h = h * c->u;
    SDL_RenderCopy(g_ren, g_canvas_tex, NULL, &dst);
}

/* A row's text cut to w units (an ellipsis at the cut). */
static int draw_fit(int x, int y, int u, unsigned rgba, int w, const char *t) {
    char buf[160];
    ui_text_fit(t, w, buf, sizeof buf);
    return draw_text(x, y, u, rgba, UI_SHADOW, buf);
}

/* The two key-help lines under the copper rule; `note` (may be NULL) on
 * the second line's right. */
static void menu_footer(int x0, int y0, int u, const char *h0, const char *h1,
                        const char *note, unsigned note_col) {
    int fy = y0 + (MENU_H - MENU_FOOT) * u, w = MENU_W - 2 * MENU_PAD;
    int nw = note ? ui_text_width(note) : 0;
    ui_fill(g_ren, x0 + MENU_PAD * u, fy + u, w * u, u, UI_C_RULE);
    draw_fit(x0 + MENU_PAD * u, fy + 4 * u, u, UI_C_KEY, w, h0);
    draw_fit(x0 + MENU_PAD * u, fy + 15 * u, u, UI_C_KEY, nw ? w - nw - 12 : w, h1);
    if (nw) draw_text(x0 + (MENU_W - MENU_PAD - nw) * u, fy + 15 * u, u, note_col, UI_SHADOW, note);
}

/* The value column of an item page: right of its widest label that has a
 * value beside it (sliders, toggles, choices). */
static int menu_value_x(const PcMenuView *v) {
    int i, w = 0;
    for (i = 0; i < v->nrows; i++) {
        const PcMenuRow *row = &v->rows[i];
        if (row->type == PC_MENU_HEADER || row->type == PC_MENU_PAGE || row->type == PC_MENU_ACTION)
            continue;
        if (ui_text_width(row->label) > w) w = ui_text_width(row->label);
    }
    w += MENU_LABEL_X + 14;
    return w < MENU_VALUE_MIN ? MENU_VALUE_MIN : w > MENU_VALUE_MAX ? MENU_VALUE_MAX : w;
}

static void draw_menu(PresentRect r) {
    static PcMenuView v;
    const PcSettings *s = pc_settings_get();
    int u = r.h / PRESENT_SRC_H, x0, y0, i, last, vcol, right;
    char line[96];
    pc_menu_view(&v);
    if (u < 1) u = 1;
    if (u > 4) u = 4;
    while (u > 1 && (MENU_W * u > r.w || MENU_H * u > r.h)) u--;
    x0 = r.x + (r.w - MENU_W * u) / 2;
    y0 = r.y + (r.h - MENU_H * u) / 2;
    right = MENU_W - MENU_PAD;

    ui_fill(g_ren, r.x, r.y, r.w, r.h, 0x00000096u);         /* dim the game */
    ui_draw_panel(g_ren, x0, y0, MENU_W, MENU_H, u);
    draw_text(x0 + MENU_PAD * u, y0 + MENU_TITLE_Y * u, u, UI_C_HEAD, UI_SHADOW, v.title);
    ui_fill(g_ren, x0 + MENU_PAD * u, y0 + (MENU_TOP - 4) * u, (MENU_W - 2 * MENU_PAD) * u, u,
            UI_C_RULE);

    if (v.custom) {
        /* a page that draws itself (the save browser) in the body */
        MenuCanvas mc;
        PcMenuCanvas cv;
        const char *h0 = v.custom->help ? v.custom->help(v.custom->ctx, 0) : NULL;
        const char *h1 = v.custom->help ? v.custom->help(v.custom->ctx, 1) : NULL;
        mc.x0 = x0 + MENU_PAD * u;
        mc.y0 = y0 + MENU_TOP * u;
        mc.u = u;
        mc.clip.x = mc.x0;
        mc.clip.y = mc.y0;
        mc.clip.w = (MENU_W - 2 * MENU_PAD) * u;
        mc.clip.h = (MENU_H - MENU_TOP - MENU_FOOT) * u;
        cv.ctx = &mc;
        cv.w = MENU_W - 2 * MENU_PAD;
        cv.h = MENU_H - MENU_TOP - MENU_FOOT;
        cv.fill = canvas_fill;
        cv.text = canvas_text;
        cv.image = canvas_image;
        SDL_RenderSetClipRect(g_ren, &mc.clip);
        if (v.custom->draw) v.custom->draw(v.custom->ctx, &cv);
        SDL_RenderSetClipRect(g_ren, NULL);
        menu_footer(x0, y0, u, h0 ? h0 : "\xE2\x86\x91\xE2\x86\x93 Select   Enter OK",
                    h1 ? h1 : "Esc Back", g_deferred[0] ? g_deferred : NULL, UI_C_WARN);
        return;
    }

    /* keep the selection in view */
    if (v.selected >= 0) {
        if (v.selected < g_menu_scroll) g_menu_scroll = v.selected;
        if (v.selected >= g_menu_scroll + MENU_ROWS) g_menu_scroll = v.selected - MENU_ROWS + 1;
        /* show the header above the first item */
        if (g_menu_scroll > 0 && g_menu_scroll == v.selected &&
            v.rows[g_menu_scroll - 1].type == PC_MENU_HEADER)
            g_menu_scroll--;
    }
    if (g_menu_scroll > v.nrows - MENU_ROWS) g_menu_scroll = v.nrows - MENU_ROWS;
    if (g_menu_scroll < 0) g_menu_scroll = 0;
    last = g_menu_scroll + MENU_ROWS;
    if (last > v.nrows) last = v.nrows;
    vcol = menu_value_x(&v);

    for (i = g_menu_scroll; i < last; i++) {
        const PcMenuRow *row = &v.rows[i];
        int y = y0 + (MENU_TOP + (i - g_menu_scroll) * MENU_ROW) * u;
        int sel = i == v.selected;
        int vx = x0 + vcol * u, end = MENU_LABEL_X, nw, nr;
        if (row->type == PC_MENU_HEADER) {
            int w = draw_fit(x0 + MENU_PAD * u, y, u, UI_C_HEAD, right - MENU_PAD, row->label);
            /* a copper line runs on from the header to the edge */
            if (MENU_PAD + w + 6 < right)
                ui_fill(g_ren, x0 + (MENU_PAD + w + 6) * u, y + 6 * u,
                        (right - MENU_PAD - w - 6) * u, u, UI_C_RULE);
            continue;
        }
        if (sel) {
            ui_fill(g_ren, x0 + (MENU_PAD - 2) * u, y, (MENU_W - 2 * MENU_PAD + 4) * u, MENU_ROW * u,
                    UI_C_SEL);
            draw_text(x0 + MENU_PAD * u, y, u, UI_C_HEART, UI_SHADOW, "\xE2\x99\xA5");
        }
        if (row->type == PC_MENU_PAGE || row->type == PC_MENU_ACTION) {
            int room = right - MENU_LABEL_X - (row->type == PC_MENU_PAGE ? PRESENT_MENU_ARROW_W : 0);
            if (row->note && row->note[0]) room -= ui_text_width(row->note) + 10;
            end += draw_fit(x0 + MENU_LABEL_X * u, y, u, UI_C_TEXT, room, row->label);
            if (row->type == PC_MENU_PAGE)
                draw_text(x0 + (right - 5) * u, y, u, sel ? UI_C_HEART : UI_C_DIM, UI_SHADOW,
                          "\xE2\x96\xB6");
        } else {
            draw_fit(x0 + MENU_LABEL_X * u, y, u, UI_C_TEXT, vcol - MENU_LABEL_X - 6, row->label);
            if (row->slider) {
                int span = row->max - row->min, w = 50;
                int k = span > 0 ? (row->pos - row->min) * (w - 2) / span : 0;
                if (k < 0) k = 0;
                if (k > w - 2) k = w - 2;
                ui_fill(g_ren, vx, y + 2 * u, w * u, 7 * u, UI_C_RULE);
                ui_fill(g_ren, vx + u, y + 3 * u, (w - 2) * u, 5 * u, UI_C_TROUGH);
                ui_fill(g_ren, vx + u, y + 3 * u, k * u, 5 * u, UI_C_BAR);
                end = vcol + w + 4 + draw_text(vx + (w + 4) * u, y, u, UI_C_TEXT, UI_SHADOW, row->value);
            } else if (row->value[0]) {
                end = vcol + draw_fit(vx, y, u, UI_C_TEXT, right - vcol - 12, row->value);
                if (sel && row->type == PC_MENU_CHOICE) {
                    /* the choice's pointers, either side of the value */
                    draw_text(vx - 8 * u, y, u, UI_C_HEART, UI_SHADOW, "\xE2\x97\x80");
                    draw_text(x0 + (end + 3) * u, y, u, UI_C_HEART, UI_SHADOW, "\xE2\x96\xB6");
                    end += 8;
                }
            }
        }
        /* the note right-aligned, short of a page row's arrow */
        if (row->note && row->note[0] &&
            (nw = present_menu_note_room(right, end, row->type == PC_MENU_PAGE, &nr)) > 12) {
            char buf[96];
            int w = ui_text_fit(row->note, nw, buf, sizeof buf);
            draw_text(x0 + (nr - w) * u, y, u, UI_C_DIM, UI_SHADOW, buf);
        }
    }
    if (g_menu_scroll > 0 || last < v.nrows) {
        int w;
        snprintf(line, sizeof(line), "%d/%d", (v.selected < 0 ? 0 : v.selected) + 1, v.nrows);
        w = ui_text_width(line);
        draw_text(x0 + (right - w) * u, y0 + MENU_TITLE_Y * u, u, UI_C_KEY, UI_SHADOW, line);
    }

    menu_footer(x0, y0, u,
                "\xE2\x86\x91\xE2\x86\x93 Select   \xE2\x86\x90\xE2\x86\x92 Change   Enter OK   "
                "Esc Back",
                "", g_deferred[0] ? g_deferred
                    : s->harness ? "Automated run: changes not saved" : "Saved to settings.ini on close",
                g_deferred[0] || s->harness ? UI_C_WARN : UI_C_DIM);
}

/* After the frame (and the debug HUD) is drawn, before it is presented. */
void pc_present_overlay(SDL_Renderer *ren) {
    const PcSettings *s = pc_settings_get();
    const char *text = NULL;
    char hud[64], paused_note[80];
    int ow, oh, hashing, scan, menu = pc_menu_is_open(), backlog = bl_open() && backlog_view_draw;
    int has_hud = 0;
    PresentRect r;
    /* a CRT frame (pc_present_crt) has scanlines of its own */
    scan = s->scanlines > 0 && !g_crt_drawn;
    g_crt_drawn = 0;
    if (ren != g_ren || !ren) return;
    snprintf(paused_note, sizeof paused_note, "Paused\n%s", g_deferred);
    g_overlay_frames++;
    /* the status HUD line (off by default; nothing is read when off) */
    if (!menu && !backlog && status_hud_text) has_hud = status_hud_text(hud, sizeof hud);
    if (menu || backlog) text = NULL;    /* the menu says it all */
    else if (g_user_paused || g_focus_paused) text = g_deferred[0] ? paused_note : "Paused";
    else if (g_osd[0] && (Sint32)(g_osd_until - overlay_now()) > 0) text = g_osd;
    else if (g_toast[0] && s->toasts && (Sint32)(g_toast_until - overlay_now()) > 0) text = g_toast;
    /* a note when nothing else holds the slot (else its time stands still) */
    if (!menu && !backlog && !text) text = present_notes_show(&g_notes, overlay_now(), 1);
    else present_notes_show(&g_notes, overlay_now(), 0);
    hashing = hash_on();
    if (!scan && !text && !g_window_shot_pending && !hashing && !menu && !backlog &&
        !has_hud)
        return;
    if (SDL_GetRendererOutputSize(ren, &ow, &oh)) return;
    r = present_layout_rect(ow, oh, s->scale_mode);

    /* draw in output pixels, then put the game's logical space back */
    SDL_RenderSetLogicalSize(ren, 0, 0);
    SDL_RenderSetScale(ren, 1.0f, 1.0f);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    if (scan && r.h >= PRESENT_SRC_H * 2) {
        int row, y, h;
        SDL_SetRenderDrawColor(ren, 0, 0, 0, (Uint8)(s->scanlines * 255 / 100));
        for (row = 0; row < PRESENT_SRC_H; row++) {
            present_scanline_band(r, row, &y, &h);
            fill(r.x, y, r.w, h);
        }
    }
    if (text) {
        /* a small game-style window at the top left; a multi-line text
         * (the slot list) is a line per row */
        int u = r.h / PRESENT_SRC_H;
        int maxw, tw = 0, lines = 0, y;
        const char *p = text;
        char line[160], cut[160];
        if (u < 1) u = 1;
        if (u > 4) u = 4;
        maxw = r.w / u - 20;
        while (*p) {
            int n = (int)strcspn(p, "\n"), w;
            snprintf(line, sizeof(line), "%.*s", n, p);
            w = ui_text_fit(line, maxw, cut, sizeof cut);
            if (w > tw) tw = w;
            lines++;
            p += n;
            if (*p) p++;
        }
        ui_draw_box(ren, r.x + 4 * u, r.y + 4 * u, tw + 10, lines * UI_ROW + 4, u);
        for (p = text, y = r.y + 6 * u; *p; y += UI_ROW * u) {
            int n = (int)strcspn(p, "\n");
            snprintf(line, sizeof(line), "%.*s", n, p);
            ui_text_fit(line, maxw, cut, sizeof cut);
            draw_text(r.x + 9 * u, y, u, UI_C_TEXT, UI_SHADOW, cut);
            p += n;
            if (*p) p++;
        }
    }
    if (has_hud && hud[0]) {
        /* top right, one line in the same small window */
        int u = r.h / PRESENT_SRC_H, tw;
        if (u < 1) u = 1;
        if (u > 3) u = 3;
        tw = ui_text_width(hud) + 10;
        ui_draw_box(ren, r.x + r.w - (tw + 4) * u, r.y + 4 * u, tw, UI_ROW + 4, u);
        draw_text(r.x + r.w - (tw - 1) * u, r.y + 6 * u, u, UI_C_HEAD, UI_SHADOW, hud);
    }
    if (menu) draw_menu(r);
    else if (backlog) backlog_view_draw(ren, r.x, r.y, r.w, r.h);
    if (backlog && g_deferred[0]) {
        /* over the backlog (it has no footer of its own) */
        int u = r.h / PRESENT_SRC_H;
        if (u < 1) u = 1;
        if (u > 4) u = 4;
        ui_draw_box(ren, r.x + 4 * u, r.y + 4 * u, ui_text_width(g_deferred) + 10, UI_ROW + 4, u);
        draw_text(r.x + 9 * u, r.y + 6 * u, u, UI_C_WARN, UI_SHADOW, g_deferred);
    }
    if (g_window_shot_pending) {
        static unsigned shots;
        const char *pat = getenv("PC_PRESENT_WINDOW_SHOT");
        char path[PC_SETTINGS_PATH_MAX];
        unsigned char *rgb = (unsigned char *)malloc((size_t)ow * oh * 3);
        g_window_shot_pending = 0;
        if (rgb && pat && present_shot_path(pat, shots++, path, sizeof path) == 0 &&
            SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_RGB24, rgb, ow * 3) == 0 &&
            present_png_write(path, rgb, ow, oh) == 0)
            printf("[PRESENT] window shot: %s (%dx%d)\n", path, ow, oh);
        free(rgb);
    }
    shot_poll_failure();
    if (hashing) hash_frame(ren, ow, oh, text != NULL, has_hud && hud[0]);
    apply_layout();
}

/* ------------------------------------------------------------------ */
/* Screenshot                                                          */
/* ------------------------------------------------------------------ */

/* A screenshot written on the writer thread (pc/hal/async_io.c; weak). */
PC_ASYNC_DECLARE_WEAK
typedef struct { char path[PC_SETTINGS_PATH_MAX + 64]; unsigned char *rgb; int w, h; } ShotJob;
static SDL_atomic_t g_shot_failed;
static void shot_job(void *arg) {
    ShotJob *j = (ShotJob *)arg;
    if (present_png_write(j->path, j->rgb, j->w, j->h) != 0) {
        fprintf(stderr, "[PRESENT] screenshot: cannot write %s\n", j->path);
        SDL_AtomicSet(&g_shot_failed, 1);
    }
    free(j->rgb);
    free(j);
}

/* Called once per presented frame: a failed background write is told. */
static void shot_poll_failure(void) {
    if (SDL_AtomicGet(&g_shot_failed) && SDL_AtomicCAS(&g_shot_failed, 1, 0))
        toast_err("Screenshot failed");
}

static void screenshot(void) {
    const PcSettings *s = pc_settings_get();
    static unsigned int pixels[GPU_SCANOUT_PIXELS];
    GpuScanoutState d;
    char dir[PC_SETTINGS_PATH_MAX], path[PC_SETTINGS_PATH_MAX + 64], stamp[32];
    unsigned char *rgb;
    int w, h, k = s->screenshot_scale, x, y, i;
    time_t now = time(NULL);
    struct tm tmv;
    FILE *probe;

    const unsigned int *src = NULL;
    int src_w = GPU_SCANOUT_WIDTH;

    /* With the internal resolution raised the screenshot is what is shown:
     * the N x picture, at 1:1 (pc/hal/gpu_hires.c). */
    if (gpu_hires_pack_display && (src = gpu_hires_pack_display(&w, &h)) != NULL) {
        k = 1;
        src_w = w;
    } else {
        gpu_get_scanout_state(&d);
        if (!gpu_scanout_pack(&d, &g_vram[0][0], pixels, GPU_SCANOUT_PIXELS, &w, &h)) return;
        src = pixels;
    }
    if (s->screenshot_dir[0]) snprintf(dir, sizeof(dir), "%s", s->screenshot_dir);
    else snprintf(dir, sizeof(dir), "%s/screenshots", s->data_dir);
    if (pc_settings_mkdirs(dir, 1)) { toast_err("Screenshot failed"); return; }
    localtime_r(&now, &tmv);
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
    if (pc_async_flush) pc_async_flush();      /* the probe must see earlier shots */
    for (i = 0; i < 100; i++) {
        if (i) snprintf(path, sizeof(path), "%s/tokimeki-%s-%d.png", dir, stamp, i);
        else snprintf(path, sizeof(path), "%s/tokimeki-%s.png", dir, stamp);
        probe = fopen(path, "rb");
        if (!probe) break;
        fclose(probe);
    }
    rgb = (unsigned char *)malloc((size_t)w * k * h * k * 3);
    if (!rgb) return;
    for (y = 0; y < h * k; y++)
        for (x = 0; x < w * k; x++) {
            unsigned int p = src[(y / k) * src_w + x / k];
            unsigned char *o = rgb + ((size_t)y * w * k + x) * 3;
            o[0] = (unsigned char)(p >> 24);
            o[1] = (unsigned char)(p >> 16);
            o[2] = (unsigned char)(p >> 8);
        }
    if (getenv("PC_PRESENT_WINDOW_SHOT")) g_window_shot_pending = 1;
    if (pc_async_run) {
        /* written on the writer thread (pc/hal/async_io.c): a slow disk or
         * an on-access scan does not stall the game; a failure is toasted
         * at the next presented frame (shot_poll_failure) */
        ShotJob *j = (ShotJob *)malloc(sizeof *j);
        if (j) {
            snprintf(j->path, sizeof j->path, "%s", path);
            j->rgb = rgb;
            j->w = w * k;
            j->h = h * k;
            printf("[PRESENT] screenshot: %s\n", path);
            toast("Screenshot saved");
            pc_async_run(shot_job, j, (size_t)j->w * j->h * 3u);
            return;
        }
    }
    if (present_png_write(path, rgb, w * k, h * k) == 0) {
        printf("[PRESENT] screenshot: %s\n", path);
        toast("Screenshot saved");
    } else {
        toast_err("Screenshot failed");
    }
    free(rgb);
}

/* ------------------------------------------------------------------ */
/* The settings menu: entries and input                                */
/* ------------------------------------------------------------------ */

static void menu_open(void) {
    if (pc_menu_is_open()) return;
    pc_menu_open();
    g_menu_scroll = 0;
    printf("[PRESENT] settings menu open\n");
    fflush(stdout);
}

/* Whatever closed it (BACK on the main page, the hotkey, START): the keys
 * that did are still held, and must not reach the game. */
static void menu_closed(void) {
    if (pad_suppress_until_release) pad_suppress_until_release();
    printf("[PRESENT] settings menu closed\n");
    fflush(stdout);
}

static void menu_close(void) {
    if (pc_menu_close() == -2) return;
    menu_closed();
}

static void menu_input(int in) {
    pc_menu_input(in);
    if (!pc_menu_is_open()) menu_closed();
}

static int menu_save(const char *const *keys, int n) {
    const PcSettings *s = pc_settings_get();
    int i, r;
    if (s->harness || !s->file_path[0]) {
        printf("[PRESENT] settings not saved: %s:", s->harness ? "automated run" : "no settings file");
        for (i = 0; i < n; i++) printf(" %s", keys[i]);
        printf("\n");
        toast_err("Settings not saved");
        return -1;
    }
    r = pc_settings_save_keys(s, s->file_path, keys, n);
    printf("[PRESENT] settings %s %s:", r ? "NOT saved to" : "saved to", s->file_path);
    for (i = 0; i < n; i++) printf(" %s", keys[i]);
    printf("\n");
    fflush(stdout);
    { if (r) toast_err("Settings not saved"); else toast("Settings saved"); };
    return r;
}

/* ---- built-in entries: getters / setters over the live settings ---- */

static PcSettings *ms(void) { return pc_settings_mut(); }
static int get_int(void *p) { return *(int *)p; }
static void set_int(void *p, int v) { *(int *)p = v; }
static void set_layout(void *p, int v) { *(int *)p = v; apply_layout(); }
static void set_volume_item(void *p, int v) {
    *(int *)p = v;
    if (spu_hal_apply_volumes) spu_hal_apply_volumes();
}
static void set_scanlines(void *p, int v) {
    *(int *)p = v;
    if (v > 0) g_scanlines_last = v;
}

/* The CRT rows: the value is settings.ini's; TOKI_CRT* win over it. */
static const char *crt_note(void *p) {
    int ow, oh;
    PresentRect r;
    (void)p;
    if (crt_env_mode() >= 0) return "Set by env var";
    if (crt_mode() == PC_CRT_OFF || !g_ren || SDL_GetRendererOutputSize(g_ren, &ow, &oh)) return "";
    r = present_layout_rect(ow, oh, ms()->scale_mode);
    if (r.w < PRESENT_SRC_W * PC_CRT_MIN_SCALE || r.h < PRESENT_SRC_H * PC_CRT_MIN_SCALE)
        return "Needs 2x";
    return "";
}
static const char *crt_curve_note(void *p) {
    (void)p;
    if (crt_env_curve() >= 0) return "Set by env var";
    return crt_mode() == PC_CRT_OFF ? "CRT off" : "";
}
static const char *scanlines_note(void *p) {
    (void)p;
    return crt_mode() != PC_CRT_OFF && ms()->scanlines > 0 ? "CRT on" : "";
}

static int get_fullscreen(void *p) {
    (void)p;
    if (g_win) return (SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    return ms()->fullscreen;
}
static void set_fullscreen(void *p, int v) {
    (void)p;
    ms()->fullscreen = v;
    if (!g_win) return;
    if (v) track_window();
    SDL_SetWindowFullscreen(g_win, v ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

static void set_vsync(void *p, int v) {
    (void)p;
    if (!g_ren || SDL_RenderSetVSync(g_ren, v) == 0) ms()->vsync = v;
    else toast_err("VSync unavailable");
}

static void set_mouse_mode(void *p, int v) {
    *(int *)p = v;
    if (pad_set_mouse_mode) pad_set_mouse_mode(v);
}

static int get_profile(void *p) { (void)p; return !SDL_strcasecmp(ms()->profile, "enhanced"); }
static void set_profile(void *p, int v) {
    (void)p;
    snprintf(ms()->profile, sizeof(ms()->profile), "%s", v ? "enhanced" : "parity");
}
static const char *profile_note(void *p) {
    const char *now = hal_profile_name ? hal_profile_name() : "parity";
    (void)p;
    /* the running profile can also come from --profile / TOKI_PROFILE /
     * toki.cfg, which win over settings.ini */
    if (SDL_strcasecmp(now, get_profile(NULL) ? "enhanced" : "parity")) return "Restart needed";
    return "On restart";
}

static void format_ff(void *p, int v, char *out, int n) {
    (void)p;
    if (v <= 0) snprintf(out, (size_t)n, "Max");
    else snprintf(out, (size_t)n, "%dx", v);
}
static const char *ff_note(void *p) {
    const char *e = getenv("TOKI_FF_SPEED");
    (void)p;
    if (e && *e) return "Set by env var";     /* TOKI_FF_SPEED wins */
    if (hal_profile_name && strcmp(hal_profile_name(), "enhanced")) return "Enhanced only";
    return "";
}

static const char *res_note(void *p) {
    const char *e = getenv("TOKI_RES_SCALE");
    int now = gpu_hires_running_scale ? gpu_hires_running_scale() : 1;
    (void)p;
    if (e && *e) return "Set by env var";     /* TOKI_RES_SCALE wins */
    if (hal_profile_name && strcmp(hal_profile_name(), "enhanced")) return "Enhanced only";
    /* the shadow VRAM is sized once, at start-up */
    if (now != ms()->resolution_scale) return "Restart needed";
    return "On restart";
}

extern void music_pack_menu_register(void) __attribute__((weak));
/* Texture packs (pc/hal/gpu_texrep.h): the choice lists "NONE" and the
 * folders under <data dir>/textures/packs, re-read by RELOAD TEXTURE PACK;
 * a value that is not one of them (a path) is listed as itself. */
#define PACK_CHOICES 32
static char g_pack_names[PACK_CHOICES][64];
static const char *g_pack_choices[PACK_CHOICES + 1];

static int pack_name_cmp(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

static void pack_list(void) {
    const char *dir = gpu_texrep_packs_dir ? gpu_texrep_packs_dir() : NULL;
    const char *cur = ms()->texture_pack;
    DIR *d = dir ? opendir(dir) : NULL;
    struct dirent *de;
    int n = 1, i, have = !*cur;
    snprintf(g_pack_names[0], sizeof g_pack_names[0], "None");
    /* Folders only, in name order: readdir's order is the file system's
     * (hash order on ext4, name order on NTFS), and a Windows folder holds
     * files such as desktop.ini or Thumbs.db that are not packs. */
    while (d && (de = readdir(d)) != NULL && n < PACK_CHOICES - 1) {
        char full[PC_SETTINGS_PATH_MAX + 80];
        struct stat st;
        if (de->d_name[0] == '.' || strlen(de->d_name) >= sizeof g_pack_names[0]) continue;
        snprintf(full, sizeof full, "%s/%s", dir, de->d_name);
        if (stat(full, &st) || !S_ISDIR(st.st_mode)) continue;
        snprintf(g_pack_names[n++], sizeof g_pack_names[0], "%s", de->d_name);
    }
    if (d) closedir(d);
    if (n > 2) qsort(g_pack_names[1], (size_t)(n - 1), sizeof g_pack_names[0], pack_name_cmp);
    for (i = 1; i < n; i++) have |= !strcmp(g_pack_names[i], cur);
    if (!have && strlen(cur) < sizeof g_pack_names[0])
        snprintf(g_pack_names[n++], sizeof g_pack_names[0], "%s", cur);
    for (i = 0; i < n; i++) g_pack_choices[i] = g_pack_names[i];
    g_pack_choices[n] = NULL;
    printf("[PRESENT] texture pack choices:");
    for (i = 0; i < n; i++) printf(" [%s]", g_pack_names[i]);
    printf("\n");
}

static int get_pack(void *p) {
    int i;
    (void)p;
    if (!ms()->texture_pack[0]) return 0;
    for (i = 1; g_pack_choices[i]; i++)
        if (!strcmp(g_pack_choices[i], ms()->texture_pack)) return i;
    return 0;
}
static void set_pack(void *p, int v) {
    const char *name = v > 0 && g_pack_choices[v] ? g_pack_choices[v] : "";
    (void)p;
    snprintf(ms()->texture_pack, sizeof(ms()->texture_pack), "%s", name);
    if (gpu_texrep_set_pack && !getenv("TOKI_TEXTURE_PACK")) gpu_texrep_set_pack(name);
}
static const char *texture_note(const char *env) {
    const char *e = getenv(env);
    if (e && *e) return "Set by env var";
    if (hal_profile_name && strcmp(hal_profile_name(), "enhanced")) return "Enhanced only";
    return "";
}
static const char *pack_note(void *p) {
    const char *n = texture_note("TOKI_TEXTURE_PACK");
    (void)p;
    if (*n) return n;
    if (gpu_texrep_needs_restart && gpu_texrep_needs_restart()) return "Restart needed";
    return "";
}
static const char *dump_note(void *p) { (void)p; return texture_note("TOKI_TEXTURE_DUMP"); }
static const char *subpixel_note(void *p) {
    const char *n = texture_note("TOKI_SUBPIXEL");
    (void)p;
    if (*n) return n;
    if (!gpu_hires_running_scale || gpu_hires_running_scale() < 2) return "Needs 3D resolution 2x+";
    return "";
}
static void set_subpixel(void *p, int v) {
    *(int *)p = v;
    if (gpu_pgxp_set_enabled && !getenv("TOKI_SUBPIXEL")) gpu_pgxp_set_enabled(v);
}
static void set_dump(void *p, int v) {
    *(int *)p = v;
    if (gpu_texrep_set_dump && !getenv("TOKI_TEXTURE_DUMP")) gpu_texrep_set_dump(v);
}
static void reload_pack(void *p) {
    (void)p;
    pack_list();
    if (gpu_texrep_reload && ms()->texture_pack[0] && !*texture_note("TOKI_TEXTURE_PACK"))
        toast("Texture pack: %d files", gpu_texrep_reload());
    else
        toast("No texture pack");
}

static void add_header(const char *label) {
    PcMenuItem it;
    memset(&it, 0, sizeof(it));
    it.type = PC_MENU_HEADER;
    it.label = label;
    pc_menu_add(&it);
}

static void add_item(int type, const char *label, const char *key, int *field,
                     int (*get)(void *), void (*set)(void *, int)) {
    PcMenuItem it;
    memset(&it, 0, sizeof(it));
    it.type = type;
    it.label = label;
    it.key = key;
    it.ctx = field;
    it.get = get ? get : get_int;
    it.set = set ? set : set_int;
    pc_menu_add(&it);
}

static void add_slider(const char *label, const char *key, int *field, int lo, int hi,
                       int step, const char *unit, void (*set)(void *, int)) {
    PcMenuItem it;
    memset(&it, 0, sizeof(it));
    it.type = PC_MENU_SLIDER;
    it.label = label;
    it.key = key;
    it.ctx = field;
    it.min = lo;
    it.max = hi;
    it.step = step;
    it.unit = unit;
    it.get = get_int;
    it.set = set ? set : set_int;
    pc_menu_add(&it);
}

static void add_choice(const char *label, const char *key, int *field,
                       const char *const *choices, void (*set)(void *, int)) {
    PcMenuItem it;
    memset(&it, 0, sizeof(it));
    it.type = PC_MENU_CHOICE;
    it.label = label;
    it.key = key;
    it.ctx = field;
    it.choices = choices;
    it.get = get_int;
    it.set = set ? set : set_int;
    pc_menu_add(&it);
}

/* The built-in entries: every settings.ini key that can change while the
 * game runs, plus the profile (next launch).  Added once, when the window
 * is attached, ahead of anything other features register. */
static void menu_builtin(void) {
    static const char *const scale_names[] = { "Aspect 4:3", "Integer", "Stretch", NULL };
    static const char *const filter_names[] = { "Sharp", "Smooth", NULL };
    static const char *const crt_names[] = { "Off", "Lite", "Aperture", "Shadow mask", NULL };
    static const char *const mouse_names[] = { "Absolute", "Relative", NULL };
    static const char *const profile_names[] = { "Parity", "Enhanced", NULL };
    static int done;
    PcSettings *s = ms();
    PcMenuItem it;
    if (done) return;
    done = 1;
    pc_menu_set_save(menu_save);

    add_header("Display");
    add_item(PC_MENU_TOGGLE, "Fullscreen", "display.fullscreen", NULL, get_fullscreen, set_fullscreen);
    add_choice("Scale", "display.scale", &s->scale_mode, scale_names, set_layout);
    add_choice("Filter", "display.filter", &s->filter, filter_names, NULL);
    memset(&it, 0, sizeof(it));
    it.type = PC_MENU_SLIDER;
    it.label = "Scanlines";
    it.key = "display.scanlines";
    it.ctx = &s->scanlines;
    it.min = 0;
    it.max = 100;
    it.step = 10;
    it.unit = "%";
    it.get = get_int;
    it.set = set_scanlines;
    it.note = scanlines_note;
    pc_menu_add(&it);
    /* the CRT filter (pc/include/pc_crt.h): presentation only */
    memset(&it, 0, sizeof(it));
    it.type = PC_MENU_CHOICE;
    it.label = "CRT filter";
    it.key = "display.crt";
    it.ctx = &s->crt;
    it.choices = crt_names;
    it.get = get_int;
    it.set = set_int;
    it.note = crt_note;
    pc_menu_add(&it);
    memset(&it, 0, sizeof(it));
    it.type = PC_MENU_TOGGLE;
    it.label = "CRT curvature";
    it.key = "display.crt_curvature";
    it.ctx = &s->crt_curvature;
    it.get = get_int;
    it.set = set_int;
    it.note = crt_curve_note;
    pc_menu_add(&it);
    add_item(PC_MENU_TOGGLE, "VSync", "display.vsync", &s->vsync, NULL, set_vsync);
    memset(&it, 0, sizeof(it));
    it.type = PC_MENU_SLIDER;
    it.label = "3D resolution";
    it.key = "graphics.resolution_scale";
    it.ctx = &s->resolution_scale;
    it.min = 1;
    it.max = 4;
    it.step = 1;
    it.unit = "x";
    it.flags = PC_MENU_RESTART;
    it.get = get_int;
    it.set = set_int;
    it.note = res_note;
    pc_menu_add(&it);
    memset(&it, 0, sizeof(it));
    it.type = PC_MENU_TOGGLE;
    it.label = "Sub-pixel 3D";
    it.key = "graphics.subpixel";
    it.ctx = &s->subpixel;
    it.get = get_int;
    it.set = set_subpixel;
    it.note = subpixel_note;
    pc_menu_add(&it);
    pack_list();
    memset(&it, 0, sizeof(it));
    it.type = PC_MENU_CHOICE;
    it.label = "Texture pack";
    it.key = "graphics.texture_pack";
    it.choices = g_pack_choices;
    it.get = get_pack;
    it.set = set_pack;
    it.note = pack_note;
    pc_menu_add(&it);
    memset(&it, 0, sizeof(it));
    it.type = PC_MENU_ACTION;
    it.label = "Reload texture pack";
    it.activate = reload_pack;
    pc_menu_add(&it);
    memset(&it, 0, sizeof(it));
    it.type = PC_MENU_TOGGLE;
    it.label = "Dump textures";
    it.key = "graphics.texture_dump";
    it.ctx = &s->texture_dump;
    it.get = get_int;
    it.set = set_dump;
    it.note = dump_note;
    pc_menu_add(&it);

    add_header("Audio");
    add_slider("Master", "audio.master", &s->vol_master, 0, 100, 5, "%", set_volume_item);
    add_slider("Music", "audio.music", &s->vol_music, 0, 100, 5, "%", set_volume_item);
    add_slider("Sound effects", "audio.sfx", &s->vol_sfx, 0, 100, 5, "%", set_volume_item);
    add_slider("Voice", "audio.voice", &s->vol_voice, 0, 100, 5, "%", set_volume_item);

    add_header("Input");
    add_choice("Mouse mode", "input.mouse_mode", &s->mouse_mode, mouse_names, set_mouse_mode);

    add_header("Game");
    memset(&it, 0, sizeof(it));
    it.type = PC_MENU_CHOICE;
    it.label = "Profile";
    it.key = "profile";
    it.choices = profile_names;
    it.flags = PC_MENU_RESTART;
    it.get = get_profile;
    it.set = set_profile;
    it.note = profile_note;
    pc_menu_add(&it);
    if (lang_menu_register) lang_menu_register();     /* LANGUAGE (pc/hal/lang.c) */
    memset(&it, 0, sizeof(it));
    it.type = PC_MENU_SLIDER;
    it.label = "Fast-forward";
    it.key = "enhanced.fast_forward_speed";
    it.ctx = &s->ff_speed;
    it.min = 0;
    it.max = 16;
    it.step = 1;
    it.get = get_int;
    it.set = set_int;
    it.format = format_ff;
    it.note = ff_note;
    pc_menu_add(&it);

    /* other features' pages (weak: harnesses without them) */
    if (music_pack_menu_register) music_pack_menu_register();
    if (backlog_menu_register) backlog_menu_register();
    if (status_menu_register) status_menu_register();
    if (assist_menu_register) assist_menu_register();
}

/* Keys while the menu is open: the fixed keys (arrows, Enter / Space,
 * Escape / Backspace) and whatever the player bound to the d-pad, circle
 * (OK), cross (back) and START (close). */
static void menu_from_mask(unsigned m) {
    if (m & PC_PAD_UP) menu_input(PC_MENU_UP);
    else if (m & PC_PAD_DOWN) menu_input(PC_MENU_DOWN);
    else if (m & PC_PAD_LEFT) menu_input(PC_MENU_LEFT);
    else if (m & PC_PAD_RIGHT) menu_input(PC_MENU_RIGHT);
    else if (m & PC_PAD_CIRCLE) menu_input(PC_MENU_OK);
    else if (m & PC_PAD_CROSS) menu_input(PC_MENU_BACK);
    else if (m & PC_PAD_START) menu_close();
}

static void menu_key(const SDL_KeyboardEvent *k) {
    switch (k->keysym.scancode) {
    case SDL_SCANCODE_UP: menu_input(PC_MENU_UP); return;
    case SDL_SCANCODE_DOWN: menu_input(PC_MENU_DOWN); return;
    case SDL_SCANCODE_LEFT: menu_input(PC_MENU_LEFT); return;
    case SDL_SCANCODE_RIGHT: menu_input(PC_MENU_RIGHT); return;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:
    case SDL_SCANCODE_SPACE: if (!k->repeat) menu_input(PC_MENU_OK); return;
    case SDL_SCANCODE_ESCAPE:
    case SDL_SCANCODE_BACKSPACE: if (!k->repeat) menu_input(PC_MENU_BACK); return;
    default: break;
    }
    if (k->keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_GUI)) return;
    if (pad_menu_key_mask) {
        unsigned m = pad_menu_key_mask((int)k->keysym.scancode);
        if (k->repeat) m &= PC_PAD_UP | PC_PAD_DOWN | PC_PAD_LEFT | PC_PAD_RIGHT;
        menu_from_mask(m);
    }
}

static void menu_button(int b) {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: menu_input(PC_MENU_UP); return;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: menu_input(PC_MENU_DOWN); return;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: menu_input(PC_MENU_LEFT); return;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: menu_input(PC_MENU_RIGHT); return;
    default: break;
    }
    if (pad_menu_button_mask) menu_from_mask(pad_menu_button_mask(b));
}

/* ------------------------------------------------------------------ */
/* Hotkeys and events                                                  */
/* ------------------------------------------------------------------ */

/* The backlog key / button / wheel: open over the paused game, or close.
 * Not over the settings menu (one overlay at a time); nothing when the
 * backlog is off (parity, or [enhanced] backlog = false). */
static void backlog_closed(void) {
    if (pad_suppress_until_release) pad_suppress_until_release();
}
static void backlog_toggle(void) {
    if (!backlog_open || !backlog_enabled || !backlog_enabled()) return;
    if (bl_open()) {
        backlog_close();
        backlog_closed();
    } else if (!pc_menu_is_open()) {
        backlog_open();
    }
}
/* An event for the open backlog. */
static int backlog_event(const SDL_Event *e) {
    if (backlog_view_event) backlog_view_event(e);
    if (!bl_open()) backlog_closed();
    return 1;
}

static void set_volume(int delta) {
    PcSettings *s = pc_settings_mut();
    s->vol_master = present_volume_step(s->vol_master, delta);
    if (spu_hal_apply_volumes) spu_hal_apply_volumes();
    toast("Volume %d%%", s->vol_master);
}

/* Hotkey changes (fullscreen, filter, scale, scanlines, vsync, volume)
 * last for the session and are not written to settings.ini: the player's
 * contract (PLAYER-GUIDE.md "Settings": "Changes made with hotkeys while
 * playing ... last until you quit"), so a quick adjustment never
 * overwrites a chosen setting.  The settings menu is the way to keep a
 * change.  (Decided 2026-11-03, docs/pc-port/STATE-MACHINES.md A9;
 * test_state_machines.HotkeysAreSessionOnly.) */
static void run_hotkey(int hk, int repeat) {
    PcSettings *s = pc_settings_mut();
    static const char *const scale_names[] = { "Aspect 4:3", "Integer", "Stretch" };
    if (repeat && hk != PC_HK_VOLUME_UP && hk != PC_HK_VOLUME_DOWN) return;
    switch (hk) {
    case PC_HK_FULLSCREEN:
        if (g_win) {
            int fs = (SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
            if (!fs) track_window();
            SDL_SetWindowFullscreen(g_win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            toast(fs ? "Windowed" : "Fullscreen");
        }
        break;
    case PC_HK_SCREENSHOT: screenshot(); break;
    case PC_HK_PAUSE:
        g_user_paused = !g_user_paused;
        if (!g_user_paused) toast("Resumed");
        break;
    case PC_HK_FILTER:
        s->filter = s->filter == PC_FILTER_LINEAR ? PC_FILTER_NEAREST : PC_FILTER_LINEAR;
        toast("Filter: %s", s->filter == PC_FILTER_LINEAR ? "Smooth" : "Sharp");
        break;
    case PC_HK_SCALE:
        s->scale_mode = (s->scale_mode + 1) % 3;
        apply_layout();
        toast("Scale: %s", scale_names[s->scale_mode]);
        break;
    case PC_HK_SCANLINES:
        if (s->scanlines > 0) { g_scanlines_last = s->scanlines; s->scanlines = 0; toast("Scanlines off"); }
        else { s->scanlines = g_scanlines_last; toast("Scanlines %d%%", s->scanlines); }
        break;
    case PC_HK_VSYNC:
        if (g_ren && SDL_RenderSetVSync(g_ren, !s->vsync) == 0) {
            s->vsync = !s->vsync;
            toast("VSync %s", s->vsync ? "on" : "off");
        } else {
            toast_err("VSync unavailable");
        }
        break;
    case PC_HK_VOLUME_UP: set_volume(10); break;
    case PC_HK_VOLUME_DOWN: set_volume(-10); break;
    case PC_HK_QUIT: hal_request_quit(); break;
    case PC_HK_MENU:
        if (bl_open()) break;           /* one overlay at a time */
        if (pc_menu_is_open()) menu_close();
        else menu_open();
        break;
    case PC_HK_QUICKSAVE:
    case PC_HK_QUICKLOAD:
        /* acted on at the next main-loop boundary: while the game is held
         * (paused, the menu, the backlog) that is when it resumes -- said
         * on screen until then, not only in a toast the pause would hide */
        if (hk == PC_HK_QUICKSAVE ? !qs_request_save : !qs_request_load) break;
        if (hk == PC_HK_QUICKSAVE) qs_request_save(-1); else qs_request_load(-1);
        if (held_here()) {
            snprintf(g_deferred, sizeof g_deferred, "%s when you resume",
                     hk == PC_HK_QUICKSAVE ? "Quicksave" : "Quickload");
            printf("[PRESENT] %s\n", g_deferred);
            fflush(stdout);
        }
        break;
    case PC_HK_QSLOT_NEXT: if (qs_slot_step) qs_slot_step(1); break;
    case PC_HK_QSLOT_PREV: if (qs_slot_step) qs_slot_step(-1); break;
    case PC_HK_BACKLOG: backlog_toggle(); break;
    case PC_HK_VOICE_REPLAY:
        if (backlog_replay_last && backlog_enabled && backlog_enabled()) backlog_replay_last();
        break;
    case PC_HK_SAVES:
        /* the save browser, straight (BACK closes it); again: close */
        if (bl_open()) break;           /* one overlay at a time (as MENU, STATUS) */
        if (pc_menu_is_open()) menu_close();
        else if (save_browser_page && save_browser_page() > 0) {
            pc_menu_open_page(save_browser_page());
            g_menu_scroll = 0;
            printf("[PRESENT] settings menu open (saves)\n");
            fflush(stdout);
        }
        break;
    case PC_HK_STATUS:
        /* the status page, straight (BACK closes it); again: close */
        if (bl_open()) break;
        if (pc_menu_is_open()) menu_close();
        else if (status_page && status_page() > 0) {
            pc_menu_open_page(status_page());
            g_menu_scroll = 0;
            printf("[PRESENT] settings menu open (status)\n");
            fflush(stdout);
        }
        break;
    case PC_HK_CONTROLS:
        /* the controls legend, straight (BACK closes it); again: close */
        if (bl_open()) break;
        if (pc_menu_is_open()) menu_close();
        else if (controls_page && controls_page() > 0) {
            pc_menu_open_page(controls_page());
            g_menu_scroll = 0;
            printf("[PRESENT] settings menu open (controls)\n");
            fflush(stdout);
        }
        break;
    default:
        break;
    }
}

/* Returns 1 when the event was a PC hotkey (the game and the debug keys do
 * not see it).  Window events are observed and passed on (return 0). */
int pc_present_handle_event(const SDL_Event *e) {
    const PcSettings *s = pc_settings_get();
    /* a test script's key (TOKI_INPUT_SCRIPT keydown / keyup): the key
     * state the game polls follows the events as they are taken */
    if (pad_script_key_taken) pad_script_key_taken(e);
    switch (e->type) {
    case SDL_KEYDOWN: {
        int hk = hotkey_of(&e->key);
        if (hk >= 0) {
            run_hotkey(hk, e->key.repeat);
            return 1;
        }
        if (bl_open()) return backlog_event(e);
        if (!pc_menu_is_open()) return 0;
        menu_key(&e->key);
        return 1;                       /* the game never sees menu keys */
    }
    case SDL_KEYUP:
        return pc_menu_is_open() || bl_open() || hotkey_of(&e->key) >= 0;
    case SDL_CONTROLLERBUTTONDOWN:
        if (is_menu_button(e->cbutton.button) && !bl_open()) {
            if (pc_menu_is_open()) menu_close();
            else menu_open();
            return 1;
        }
        if (is_backlog_button(e->cbutton.button) && backlog_enabled && backlog_enabled() &&
            !pc_menu_is_open()) {
            backlog_toggle();
            return 1;
        }
        if (bl_open()) return backlog_event(e);
        if (!pc_menu_is_open()) return 0;
        menu_button(e->cbutton.button);
        return 1;
    case SDL_CONTROLLERBUTTONUP:
        return pc_menu_is_open() || bl_open() || is_menu_button(e->cbutton.button) ||
               (is_backlog_button(e->cbutton.button) && backlog_enabled && backlog_enabled());
    case SDL_CONTROLLERAXISMOTION: {
        /* the left stick moves the menu and the backlog as the d-pad does
         * (pc/hal/pad.c tracks it always, so a push held from the game is
         * not a press when the menu opens) */
        int dir = pad_stick_menu_button ? pad_stick_menu_button(e) : -1;
        if (!pc_menu_is_open() && !bl_open()) return 0;
        if (dir >= 0) {
            SDL_Event b;
            memset(&b, 0, sizeof b);
            b.type = SDL_CONTROLLERBUTTONDOWN;
            b.cbutton.which = e->caxis.which;
            b.cbutton.button = (Uint8)dir;
            b.cbutton.state = SDL_PRESSED;
            return pc_present_handle_event(&b);
        }
        return 1;
    }
    case SDL_MOUSEWHEEL:
        if (bl_open()) return backlog_event(e);
        /* the wheel up opens the backlog (the PS1 mouse has no wheel) */
        if (!pc_menu_is_open() && backlog_enabled && backlog_enabled() && backlog_count &&
            backlog_count() > 0 &&
            (e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? e->wheel.y < 0 : e->wheel.y > 0)) {
            backlog_toggle();
            return 1;
        }
        return pc_menu_is_open();
    case SDL_MOUSEMOTION:
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        if (bl_open()) return backlog_event(e);
        return pc_menu_is_open();
    case SDL_WINDOWEVENT:
        switch (e->window.event) {
        case SDL_WINDOWEVENT_FOCUS_LOST:
            if (s->pause_on_focus_loss && !s->harness) g_focus_paused = 1;
            break;
        case SDL_WINDOWEVENT_FOCUS_GAINED:
            g_focus_paused = 0;
            break;
        case SDL_WINDOWEVENT_SIZE_CHANGED:
        case SDL_WINDOWEVENT_MOVED:
        case SDL_WINDOWEVENT_MAXIMIZED:
        case SDL_WINDOWEVENT_RESTORED:
            track_window();
            if (s->scale_mode == PC_SCALE_STRETCH) apply_layout();
            break;
        }
        return 0;
    default:
        return 0;
    }
}

/* Called from hal_frame_begin after the event poll: while paused, hold the
 * game thread here (emulated time does not advance), keep the window live
 * and the watchdog fed, and silence the mixer.  The settings menu is open
 * only in here.  Each pass shows the (frozen) frame first, so the pause
 * label or the menu appears at once, then takes events until a key or
 * button press -- one press per shown frame, so the menu is redrawn after
 * every step -- and leaves without showing another frame once nothing
 * holds it any more. */
static int held_here(void) {
    /* a quit waiting for its resume state lets the game reach the boundary */
    if (sa_quit_pending && sa_quit_pending()) return 0;
    return g_user_paused || g_focus_paused || pc_menu_is_open() || bl_open();
}

/* system.c: stop polling, the pause loop is about to take over. */
int pc_present_holding(void) { return held_here(); }

void pc_present_pause_loop(void) {
    if (!held_here()) return;
    printf("[PRESENT] paused\n");
    fflush(stdout);
    if (spu_hal_set_paused) spu_hal_set_paused(1);
    /* the pointer is the player's while the game is held (pc/hal/pad.c) */
    if (pad_hold_changed) pad_hold_changed(1);
    while (held_here() && hal_is_running()) {
        SDL_Event e;
        pc_watchdog_presented();
        gpu_display();
        if (g_ren) SDL_RenderPresent(g_ren);
        if (SDL_WaitEventTimeout(&e, 100)) {
            do {
                int press = e.type == SDL_KEYDOWN || e.type == SDL_CONTROLLERBUTTONDOWN;
                int taken;
                if (e.type == SDL_QUIT) { hal_request_quit(); break; }
                taken = pc_present_handle_event(&e);
                /* a mouse button let go during the hold is let go for the
                 * game too (a press is not passed on: the game is held) */
                if ((!taken && (e.type == SDL_WINDOWEVENT || e.type == SDL_CONTROLLERDEVICEADDED ||
                                e.type == SDL_CONTROLLERDEVICEREMOVED ||
                                e.type == SDL_JOYDEVICEADDED || e.type == SDL_JOYDEVICEREMOVED)) ||
                    e.type == SDL_MOUSEBUTTONUP)
                    pad_handle_event(&e);
                if (press) break;
            } while (SDL_PollEvent(&e));
        }
    }
    if (pc_menu_is_open()) menu_close();     /* quitting with it open still saves */
    if (bl_open()) backlog_close();
    /* whatever ended the hold -- the pause key's second press, the window
     * getting focus back -- may still be down: those keys and buttons reach
     * the game only once released (Ctrl let go before Space would else
     * hand Space's circle to the game) */
    if (pad_suppress_until_release) pad_suppress_until_release();
    if (pad_hold_changed) pad_hold_changed(0);
    g_deferred[0] = 0;
    if (spu_hal_set_paused) spu_hal_set_paused(0);
    printf("[PRESENT] resumed\n");
    fflush(stdout);
}
