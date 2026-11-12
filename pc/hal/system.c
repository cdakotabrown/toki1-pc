/* SPDX-License-Identifier: GPL-3.0-or-later */
/* system.c — HAL system init, timing, main loop via SDL2 */
#include "hal.h"
#include "pc_irq.h"
#include "cpu_cost.h"
#include "profile.h"
#include "gpu_reset.h"
#include "gpu_cpu.h"
#include "pc_watchdog.h"
#include "pc_setup.h"
#include "pc_data_root.h"
/* weak: the harnesses that compile this file alone have no setup screen */
extern const char *pc_setup_run(SDL_Window *win, SDL_Renderer *ren) __attribute__((weak));
extern void hal_lang_init(const char *asset_dir) __attribute__((weak));
#include <SDL2/SDL.h>
#ifdef _WIN32
#include <SDL2/SDL_syswm.h>
#endif
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

static char g_asset_dir[512];
static SDL_Window *g_window;
static SDL_Renderer *g_renderer;
static int g_running = 1;
static u32 g_frame_start;
static u64 g_frame_start_tick;
static u64 g_pacing_remainder;
static int g_pace_resync;      /* a state was loaded: start a new pacing schedule */

extern u64 hal_clock_ticks(void);
extern u32 hal_clock_ticks_per_second(void);
static int g_headless_mode = 0;

/* The presenter (pc/hal/present.c: window size, fullscreen, scaling,
 * overlays, PC hotkeys, host pause).  Weak: the harnesses that compile
 * this file without it keep the historical 640x480 window, the 320x240
 * logical size and Escape as quit. */
extern void pc_present_window_params(int *w, int *h, int *x, int *y,
                                     Uint32 *flags) __attribute__((weak));
extern Uint32 pc_present_renderer_flags(void) __attribute__((weak));
extern void pc_present_attach(SDL_Window *w, SDL_Renderer *r) __attribute__((weak));
extern int pc_present_handle_event(const SDL_Event *e) __attribute__((weak));
extern void pc_present_pause_loop(void) __attribute__((weak));
extern int pc_present_holding(void) __attribute__((weak));
extern void pc_present_shutdown(void) __attribute__((weak));
/* pc/hal/quicksave.c / save_auto.c (the player's saves); weak: the
 * harnesses link this file alone */
extern void qs_shutdown(void) __attribute__((weak));
extern void sa_frame(void) __attribute__((weak));
extern int sa_defer_quit(void) __attribute__((weak));

/* One SDL event, as both event loops dispatch it. */
static void dispatch_event(SDL_Event *e);

/* Detect if running in WSL by checking for WSL-specific kernel version string */
static int is_wsl(void) {
    FILE *f = fopen("/proc/version", "r");
    if (!f) return 0;
    char buf[256];
    if (fgets(buf, sizeof(buf), f)) {
        fclose(f);
        return (strstr(buf, "microsoft") != NULL || strstr(buf, "WSL") != NULL);
    }
    fclose(f);
    return 0;
}

/* Select video driver: prefer X11/Wayland, fallback to offscreen if they hang */
static const char *select_video_driver(void) {
    const char *env_driver = getenv("SDL_VIDEODRIVER");
    if (env_driver && strlen(env_driver) > 0) {
        printf("[HAL] Using SDL_VIDEODRIVER=%s from environment\n", env_driver);
        return NULL; /* Let SDL use env var */
    }

#if defined(__linux__)
    /* Linux without an X11 or Wayland display: no window can open, so run
     * offscreen.  Linux only -- Windows (and macOS) have no DISPLAY variable
     * and always have a desktop: the check there sent the player build to
     * the offscreen driver, so it ran with no window (2026-11-02). */
    const char *display = getenv("DISPLAY");
    const char *wayland = getenv("WAYLAND_DISPLAY");
    if ((!display || strlen(display) == 0) && (!wayland || !*wayland)) {
        printf("[HAL] No DISPLAY or WAYLAND_DISPLAY set, using offscreen driver\n");
        return "offscreen";
    }
#endif

    /* WSL note: some WSLg setups may hang on SDL_CreateWindow. If that happens,
     * run with: SDL_VIDEODRIVER=dummy ./run_game.sh --headless
     * But try X11 first - modern WSLg usually works fine.
     */
    if (is_wsl()) {
        printf("[HAL] WSL detected - trying X11 (if it hangs, use --headless)\n");
    }

    /* Non-WSL Linux: use default driver selection (usually x11 or wayland) */
    return NULL;
}

/* asset_dir NULL: there is no game data yet -- the window opens with the
 * first-run setup screen (pc/hal/setup_screen.c), which prepares it from
 * the player's disc; the game then starts in the same window. */
void hal_init(const char *asset_dir) {
    if (asset_dir) {
        strncpy(g_asset_dir, asset_dir, sizeof(g_asset_dir) - 1);
        printf("[HAL] Init with assets: %s\n", g_asset_dir);
        fflush(stdout);

        printf("[HAL] Calling assets_init...\n"); fflush(stdout);
        assets_init(asset_dir);
        printf("[HAL] assets_init done\n"); fflush(stdout);
    }

    /* Select video driver before SDL_Init */
    const char *driver = select_video_driver();
    if (driver) {
        SDL_SetHint(SDL_HINT_VIDEODRIVER, driver);
        if (strcmp(driver, "offscreen") == 0 || strcmp(driver, "dummy") == 0) {
            g_headless_mode = 1;
        }
    }
    fflush(stdout);

    {
        /* pc/hal/pad.c: controller hints; weak for the harnesses */
        extern void pad_sdl_hints(void) __attribute__((weak));
        if (pad_sdl_hints) pad_sdl_hints();
    }
    printf("[HAL] Calling SDL_Init...\n"); fflush(stdout);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
        printf("[HAL] SDL_Init failed: %s\n", SDL_GetError());
        fflush(stdout);
        return;
    }
    printf("[HAL] SDL_Init done (driver: %s)\n", SDL_GetCurrentVideoDriver());
    fflush(stdout);

    {
        int ww = 640, wh = 480;
        int wx = SDL_WINDOWPOS_CENTERED, wy = SDL_WINDOWPOS_CENTERED;
        Uint32 wflags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
        if (pc_present_window_params) pc_present_window_params(&ww, &wh, &wx, &wy, &wflags);
        g_window = SDL_CreateWindow("Heartthrob Memorial PC Enhanced",
                                    wx, wy, ww, wh, wflags);
    }
    if (!g_window) {
        printf("[HAL] Window creation failed: %s\n", SDL_GetError());
        fflush(stdout);
        return;
    }
    printf("[HAL] Window created\n"); fflush(stdout);
#ifdef _WIN32
    {
        /* pc/hal/win/win_compat.c: the DPI awareness the manifest gave */
        extern void pc_win_dpi_report(void *hwnd);
        SDL_SysWMinfo wm;
        SDL_VERSION(&wm.version);
        pc_win_dpi_report(SDL_GetWindowWMInfo(g_window, &wm) && wm.subsystem == SDL_SYSWM_WINDOWS
                          ? (void *)wm.info.win.window : NULL);
    }
#endif

    {
        Uint32 rflags = pc_present_renderer_flags ? pc_present_renderer_flags() : 0;
        g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED | rflags);
        if (!g_renderer) {
            /* Try software renderer as fallback (needed for SDL_VIDEODRIVER=dummy) */
            g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE | rflags);
        }
        if (!g_renderer && rflags)
            g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!g_renderer) {
        printf("[HAL] Renderer creation failed: %s (continuing headless)\n", SDL_GetError());
        fflush(stdout);
        /* Don't return - continue without renderer for dummy/headless mode */
    } else {
        printf("[HAL] Renderer created\n"); fflush(stdout);
    }

    SDL_RenderSetLogicalSize(g_renderer, 320, 240);
    if (!asset_dir && pc_setup_run) {
        asset_dir = pc_setup_run(g_window, g_renderer);
        if (!asset_dir) {
            /* the player closed the setup window: just leave */
            if (g_renderer) SDL_DestroyRenderer(g_renderer);
            SDL_DestroyWindow(g_window);
            SDL_Quit();
            exit(0);
        }
        strncpy(g_asset_dir, asset_dir, sizeof(g_asset_dir) - 1);
        printf("[HAL] Init with assets: %s\n", g_asset_dir);
        assets_init(asset_dir);
        /* the language layer (pc/hal/lang.c) needs the folder too; main()
         * runs it itself when the folder existed before the setup */
        if (hal_lang_init) hal_lang_init(asset_dir);
        fflush(stdout);
    }
    if (pc_present_attach) pc_present_attach(g_window, g_renderer);
    {
        int pw = 0, ph = 0;
        SDL_GetWindowSize(g_window, &pw, &ph);
        printf("[HAL] SDL2 window created (320x240 logical, %dx%d physical)\n", pw, ph);
    }
    fflush(stdout);

    printf("[HAL] Calling debug_init...\n"); fflush(stdout);
    debug_init();
    printf("[HAL] debug_init done\n"); fflush(stdout);

    /* Initialize audio subsystem early — game may call SsSeqPlay before SpuInit */
    printf("[HAL] Calling spu_hal_init...\n"); fflush(stdout);
    extern void spu_hal_init(void);
    spu_hal_init();
    printf("[HAL] spu_hal_init done\n"); fflush(stdout);

    printf("[HAL] Calling memcard_init...\n"); fflush(stdout);
    memcard_init();
    printf("[HAL] memcard_init done\n"); fflush(stdout);
}

void hal_shutdown(void) {
    if (qs_shutdown) qs_shutdown();       /* a save still being written */
    {
        /* pc/hal/card_writer.c (weak: harnesses link this file alone) */
        extern void card_writer_wait(void) __attribute__((weak));
        if (card_writer_wait) card_writer_wait();   /* the memory card file */
    }
    if (pc_present_shutdown) pc_present_shutdown();
    debug_shutdown();
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_window) SDL_DestroyWindow(g_window);
    SDL_Quit();
    printf("[HAL] Shutdown\n");
}

extern void debug_handle_input(SDL_Event *e);
extern void debug_frame(void);
extern void pad_handle_event(const SDL_Event *e);   /* pc/hal/pad.c: mouse, focus, controllers */

/* PC hotkeys first (they never reach the game or the debug keys), then
 * the input HAL, quit, and the debug keys.  Without the presenter, Escape
 * still quits as it always did; with it, Escape is a game key (cancel) and
 * quitting is the window's close button or the quit hotkey (Ctrl+Q). */
static void dispatch_event(SDL_Event *e) {
    if (pc_present_handle_event && pc_present_handle_event(e)) {
        /* a mouse button let go over the menu / backlog is let go for the
         * game too (pc/hal/pad.c would else hold it down) */
        if (e->type == SDL_MOUSEBUTTONUP) pad_handle_event(e);
        return;
    }
    pad_handle_event(e);
    if (e->type == SDL_QUIT) {
        hal_request_quit();
    }
    if (!pc_present_handle_event && e->type == SDL_KEYDOWN &&
        e->key.keysym.sym == SDLK_ESCAPE) {
        g_running = 0;
    }
    debug_handle_input(e);
}

void hal_frame_begin(void) {
    SDL_Event e;
    /* An event that pauses or opens the settings menu ends the poll: the
     * pause loop takes the events queued behind it, so the menu sees
     * them, not the game. */
    while (SDL_PollEvent(&e)) {
        dispatch_event(&e);
        if (pc_present_holding && pc_present_holding()) break;
    }
    if (pc_present_pause_loop) pc_present_pause_loop();
    if (sa_frame) sa_frame();             /* autosave / resume: due yet? (reads only) */
    if (pc_log_frame) pc_log_frame();     /* caps the player log (pc/hal/data_root.c) */
    {
        /* TOKI_TEST_QUIT_FRAMES=N (pc/tools/fresh_install.py): a player-like
         * run (no harness variables, so no PC_TRACE_FRAMES) that quits the
         * normal way -- window state and settings saved -- after N frames */
        static int quit_at = -2, frames;
        if (quit_at == -2) {
            const char *q = getenv("TOKI_TEST_QUIT_FRAMES");
            quit_at = q && *q ? atoi(q) : -1;
        }
        if (quit_at > 0 && ++frames == quit_at) hal_request_quit();
    }
    g_frame_start = SDL_GetTicks();
    g_frame_start_tick = hal_clock_ticks();
    debug_frame();
}

/* Fast-forward (pc/hal/enhance.c; the enhanced profile's TOKI_FAST_FORWARD):
 * with it held, only every Nth frame is presented and the wall pacing below
 * runs N times faster (N = TOKI_FF_SPEED; 0: no pacing).  Emulated time is
 * untouched -- the game runs exactly the frames it would, only sooner.
 * Weak: harnesses link this file alone; parity always gets 1. */
extern int hal_ff_frames(void) __attribute__((weak));
/* pc/hal/parity.c (PC_FRAME_HASH); weak: harnesses link this file alone */
extern void parity_frame_hash(void) __attribute__((weak));
/* pc/hal/sweep_shot.c (PC_SWEEP_DIR, visual sweeps); weak: harnesses */
extern void sweep_shot_frame_end(void) __attribute__((weak));
/* pc/hal/gpu_texrep.c (texture dumps); weak: harnesses link this file alone */
extern void gpu_texrep_frame_end(int scene) __attribute__((weak));
/* pc/hal/backlog_capture.c (TOKI_BACKLOG, enhanced): watches the text
 * engine once per game frame, read only; weak for the harnesses */
extern void backlog_capture_frame(void) __attribute__((weak));
/* pc/hal/quick_text.c ([enhanced] quick_text): a page completed by the
 * player's press is completed here, at the frame boundary; weak for the
 * harnesses */
extern void quick_text_frame_end(void) __attribute__((weak));
/* pc/hal/extras.c (TOKI_EXTRAS, enhanced): endings, achievements and the
 * gallery, observed once per game frame, read only; weak for the harnesses */
extern void extras_frame_end(void) __attribute__((weak));
static unsigned g_ff_skipped;

/* PC_FRAME_TIME=PATH: one line per frame end with the host time since the
 * previous one (game code, drawing, presentation -- with PC_TURBO nothing
 * else) and the screen (now_step).  Performance measurement only. */
extern u8 now_step __attribute__((weak));   /* weak: the pacing harness */
static void frame_time_record(void) {
    static int init;
    static FILE *out;
    static struct timespec last;
    static unsigned long frames;
    extern u32 hal_clock_vblanks(void) __attribute__((weak));
    struct timespec now;
    if (!init) {
        const char *path = getenv("PC_FRAME_TIME");
        init = 1;
        if (!path || !*path) return;
        out = fopen(path, "w");
        clock_gettime(CLOCK_MONOTONIC, &last);
    }
    if (!out) return;
    clock_gettime(CLOCK_MONOTONIC, &now);
    fprintf(out, "%lu vblank=%lu step=%u us=%ld\n", frames++,
            hal_clock_vblanks ? (unsigned long)hal_clock_vblanks() : 0ul, &now_step ? (unsigned)now_step : 0u,
            (long)((now.tv_sec - last.tv_sec) * 1000000L +
                   (now.tv_nsec - last.tv_nsec) / 1000L));
    fflush(out);
    last = now;
}

/* pc/hal/quicksave.c after a load: the emulated clock just jumped to the
 * state's (g_frame_start_tick, kept from this session, is from another
 * time line), so the next frame must not be paced against it -- the
 * difference was the whole emulated time between the two moments, which
 * the pacing below slept away (a resume loaded at frame 0 of a new session
 * waited ~77 s and tripped the watchdog).  Host pacing only; emulated time
 * is untouched. */
void hal_pacing_resync(void) {
    g_frame_start = SDL_GetTicks();
    g_frame_start_tick = hal_clock_ticks();
    g_pacing_remainder = 0;
    g_pace_resync = 1;
}

void hal_frame_end(void) {
    extern void gpu_display(void);
    static int fe_trace = 0;
    int ff = hal_ff_frames ? hal_ff_frames() : 1;
    if (parity_frame_hash) parity_frame_hash();
    if (sweep_shot_frame_end) sweep_shot_frame_end();
    if (gpu_texrep_frame_end) gpu_texrep_frame_end(&now_step ? (int)now_step : -1);
    if (quick_text_frame_end) quick_text_frame_end();
    if (backlog_capture_frame) backlog_capture_frame();
    if (extras_frame_end) extras_frame_end();
    if (ff != 1) {
        if (ff > 1 && ++g_ff_skipped < (unsigned)ff) goto pace;
        if (ff == 0 && ++g_ff_skipped < 16u) goto pace;
        g_ff_skipped = 0;
    }
    /* Present fresh scanout, not SDL's discarded previous backbuffer. */
    gpu_display();
    if (fe_trace < 5) {
        printf("[HAL] hal_frame_end: calling SDL_RenderPresent\n");
        fe_trace++;
    }
    SDL_RenderPresent(g_renderer);
    frame_time_record();
pace:
    /* A movie consumes several hardware frames between VSync calls.
     * Pace the elapsed emulated ticks, not a fixed 16 ms per present.
     * Carry fractional milliseconds.  This changes wall pacing, not
     * emulated time.
     *
     * Since 2026-11-01 the frames keep an absolute schedule: each frame's
     * deadline is the previous deadline plus its emulated time, so the time
     * lost between frames (SDL_Delay oversleeping, the event poll before
     * the next frame starts) is made up instead of lost.  Measured before:
     * the emulated clock ran 0.6-0.7% behind the wall clock in both
     * profiles, i.e. behind the audio device, so every XA stream (voice,
     * movie, streamed music) slowly ran out of data and the sequencer ran
     * slow.  A frame later than TOKI_PACE_SLACK_MS (default 50) behind its
     * deadline -- a host stall, a host pause, fast-forward -- starts a new
     * schedule from now: at most that much is ever caught up, in a few
     * back-to-back frames.  TOKI_PACE_SLACK_MS=0 is the previous pacing
     * (each frame timed from its own start, lateness never made up). */
    {
        extern int trace_turbo(void);
        static long slack = -1;
        static int scheduled;           /* g_pace_deadline is valid */
        static u32 g_pace_deadline;     /* SDL_GetTicks() the last frame was due at */
        if (slack < 0) {
            const char *e = getenv("TOKI_PACE_SLACK_MS");
            slack = e && *e ? atol(e) : 50;
            if (slack < 0) slack = 0;
        }
        if (g_pace_resync) {
            g_pace_resync = 0;
            scheduled = 0;
        }
        if (!trace_turbo() && ff != 0) {
            u64 now = hal_clock_ticks();
            u64 delta = (now >= g_frame_start_tick ? now - g_frame_start_tick : 0) /
                        (u64)(ff > 1 ? ff : 1);
            u32 rate = hal_clock_ticks_per_second();
            u64 scaled = delta * 1000u + g_pacing_remainder;
            u64 target = scaled / rate;
            u32 wall = SDL_GetTicks();
            u32 base = slack > 0 && scheduled ? g_pace_deadline : g_frame_start;
            u32 due = base + (u32)target;
            g_pacing_remainder = scaled % rate;
            if ((s32)(due - wall) > 0) {
                SDL_Delay(due - wall);
                g_pace_deadline = due;
            } else if ((s32)(wall - due) <= slack) {
                g_pace_deadline = due;              /* late: made up by the next frames */
            } else {
                g_pace_deadline = wall;             /* too late: a new schedule */
            }
            scheduled = 1;
        } else {
            scheduled = 0;
        }
    }
}

/* Host-only graphics diagnostics do not execute VSync or consume console
 * ticks. Give that loop a 60 Hz wall budget without advancing game time or
 * imposing a minimum wait on normal-game presentation. */
void hal_debug_frame_end(void) {
    extern int trace_turbo(void);
    static u32 remainder;
    u32 target;
    u32 elapsed;

    hal_frame_end();
    if (trace_turbo()) return;
    remainder += 1000;
    target = remainder / 60;
    remainder %= 60;
    elapsed = SDL_GetTicks() - g_frame_start;
    if (target > elapsed) SDL_Delay(target - elapsed);
}

int hal_is_running(void) {
    return g_running;
}

/* The window's close button, the quit hotkey and scripted input
 * (TOKI_INPUT_SCRIPT `quit`) end the run.  During a game, with [saves]
 * resume on (never in automated runs), the quit waits for the next
 * main-loop boundary, where pc/hal/save_auto.c writes the resume state and
 * calls hal_quit_now(); a second request, or 4 s without a boundary, quits
 * at once. */
void hal_request_quit(void) {
    if (sa_defer_quit && sa_defer_quit()) return;
    g_running = 0;
}

void hal_quit_now(void) {
    g_running = 0;
}

/* As hal_frame_begin: an event that pauses or opens the settings menu ends
 * the drain, so the events behind it (the menu's keys, a close) go to the
 * pause loop of the next frame -- not to an open menu here, outside the
 * loop that saves its changes when it closes. */
void hal_poll_quit(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        dispatch_event(&e);
        if (pc_present_holding && pc_present_holding()) break;
    }
}

/* ResetGraph(1)'s CPU work (reference Interpreter, warm; the
 * 1,170 vblank_function calls of a 2,400-frame no-input boot): 92 ticks to
 * _reset's SetIntrMask(0), 247 masked, then 662 to return (the restore,
 * then two byte-loop memsets invalidating the cached DRAWENV/DISPENV):
 * 1,001 in all. The other modes are not measured and stay untimed. */
#define RESETGRAPH1_TO_MASK    92u
#define RESETGRAPH1_MASKED     247u
#define RESETGRAPH1_TO_RETURN  662u
extern void hal_cpu_spend(u32 ticks);

void ResetGraph(s32 mode) {
    int soft = (mode & 3) == 1;
    if (soft) hal_cpu_spend(RESETGRAPH1_TO_MASK);
    gpu_reset_graph(mode);
    if (soft) gpu_cpu_masked(RESETGRAPH1_MASKED, RESETGRAPH1_TO_RETURN);
}

/* ---- VSync and the vertical counter, on the system clock ----
 * pc/hal/cd.c owns the clock (PS1 master-clock ticks; the CD drive model
 * advances it while libcd waits) and raises hal_vblank_interrupt() at every
 * VBlank it crosses.  Game code takes no time on the PC, so the clock moves
 * only where the console would wait: VSync, the drive, libstr frame arrival.
 *
 * Psy-Q libetc semantics (psyz libetc.c / the SDK manual):
 *   VSync(-1)  SDK IRQ counter (no wait; independent of physical edges)
 *   VSync(1)   horizontal lines since the last VSync (no wait)
 *   VSync(0)   wait for the next VBlank
 *   VSync(n)   wait until n VBlanks have passed since the last VSync
 * A waiting VSync is also where the PC presents the frame and polls host
 * events (hal_frame_end / hal_frame_begin): that is when the console's
 * display would change.  Until 2026-10-16 every VSync call, even VSync(-1),
 * presented a frame and none waited. */
extern u32 hal_clock_vblanks(void);
extern u32 hal_clock_ticks_per_frame(void);
extern void cd_hal_vblank_tick(void);
#define LINES_PER_FRAME 263u
static u64 g_last_vsync_tick;
static u32 g_last_vsync_vbl;

static s32 lines_since(u64 t) {
    u64 now = hal_clock_ticks();
    u64 per_line = hal_clock_ticks_per_frame() / LINES_PER_FRAME;
    return (s32)((now > t ? now - t : 0) / per_line);
}

s32 VSync(s32 mode) {
    s32 lines;
    if (mode < 0)
        return hal_sdk_vcount();
    if (mode == 1)
        return lines_since(g_last_vsync_tick);
    if (mode == 0) {
        cd_hal_vblank_tick();
    } else {
        u32 target = g_last_vsync_vbl + (u32)mode;
        do cd_hal_vblank_tick(); while (hal_clock_vblanks() < target);
    }
    lines = lines_since(g_last_vsync_tick);
    g_last_vsync_tick = hal_clock_ticks();
    g_last_vsync_vbl = hal_clock_vblanks();
    hal_frame_end();
    hal_frame_begin();
    pc_watchdog_presented();
    return lines;
}

/* libgs GsClearVcount / GsGetVcount: root counter 1 counting horizontal
 * lines (GsInitVcount: SetRCnt(RCntCNT1, 0xFFFF, RCntMdINTR)), i.e. lines
 * since the last clear.  vblank_function compares it with vcount_lim (262)
 * to skip VSync(0) on a frame that already overran. */
static u64 g_vcount_clear_tick;
/* libgs GsClearVcount / GsGetVcount and libapi ResetRCnt: their retail
 * CPU time (cpu_cost.h HAL_SDK_COST). */
void GsClearVcount(void) { HAL_SDK_COST("GsClearVcount"); g_vcount_clear_tick = hal_clock_ticks(); }
void GsInitVcount(void) { GsClearVcount(); }   /* SetRCnt + StartRCnt: the counter starts from 0 */
s32 GsGetVcount(void) { HAL_SDK_COST("GsGetVcount"); return lines_since(g_vcount_clear_tick); }

/* Root counters (Psy-Q libapi) */
/* StartRCnt: 52 ticks of BIOS CPU (the boot's two calls, cost table row;
 * BIOS_CPU_COST=0 is the negative control). */
s32 StartRCnt(s32 spec) {
    if (hal_model_on("BIOS_CPU_COST")) HAL_CPU_COST("StartRCnt");
    return (spec & 0xffff) < 3;
}
s32 StopRCnt(s32 spec) { return (spec & 0xffff) < 3; }
s32 ResetRCnt(s32 spec) { HAL_SDK_COST("ResetRCnt"); return (spec & 0xffff) < 3; }

/* The VBlank interrupt (raised by the system clock, pc/hal/cd.c) has two
 * retail-visible levels. The exception itself runs the kernel's enabled
 * VBlank events (vblank_counter_tick -> vblnk_counter) and the controller
 * poll the BIOS does at VBlank (pc/hal/pad.c), gated only by the CPU
 * enable: during card startup retail delivers 76 BIOS callbacks while no
 * libetc handler runs. The libetc chain additionally needs the I_MASK
 * VBlank bit and is entered late, after those BIOS callbacks. */
extern void hal_bios_vblank_tick(void);
extern void pad_update_buffers(void);
extern void trace_vblank_begin(void);
extern void trace_vblank(void);
static void deliver_edge(void) {
    hal_bios_vblank_tick();
    pad_update_buffers();
}
/* CPU time of one VBlank exception that runs the libetc chain, measured on
 * the reference emulator (Interpreter, SCPH-5500): twelve back-to-back BIOS
 * memcpy(2048) calls in title_data_read take 418,062 ticks each, and the
 * seven that span a VBlank take 24,438..24,677 more (median 24,449).  It
 * covers the kernel VBlank events, the BIOS pad poll and libetc's
 * trapIntr/trapIntrVSync with the title's handlers installed.  An exception
 * that runs only the kernel level (I_MASK VBlank bit clear, as in card
 * startup) is not measured and is not charged. */
#define VBLANK_EXCEPTION_TICKS 24449u
/* With the game-code CPU model on, the exception is instead its measured
 * base plus the libsnd sequencer tick it ran (pc/hal/cpu_cost.c: the
 * sequencer is the part that varies, 2,700..18,000 ticks a VBlank). */
/* Before the BIOS runs a card or pad driver at VBlank (StartCARD /
 * StartPAD, from _main's input_events_init), the exception is the kernel's
 * dispatch plus libetc's trapIntr / trapIntrVSync only: 3,353 ticks on all
 * 46 VBlanks between _main's entry and input_events_init on the reference
 * boot (the reference emulator, SCPH-5500, -tracepc on the exception vector
 * and its returns), against ~21,000..24,500 once the drivers run.  The
 * port charged the in-game length from the first VBlank.
 * BOOT_CPU_COST=0 is the negative control. */
#define VBLANK_EXCEPTION_BARE_TICKS 3353u
extern int hal_bios_vblank_drivers(void) __attribute__((weak));
static int vblank_bare(void) {
    static int on = -1;
    if (on < 0) {
        on = hal_model_on("BOOT_CPU_COST");
    }
    return on && hal_bios_vblank_drivers && !hal_bios_vblank_drivers();
}
/* VBLANK_NO_SEQ_BASE: the cost model's base (cpu_cost_vblank_base, 21,930:
 * the in-game median of the exception less the sequencer's functions)
 * includes trapIntrVSync's call of libsnd's VSync callback and its return.
 * Before SsStart2 installs that callback the exception has no call: 21,522
 * ticks at the boot (the reference emulator, frame 554: trapIntrVSync, no
 * SsSeqCalledTbyT; the next frames with the sequencer, 24,098..24,306 =
 * the base + its ~2,400).  The port charged the base: the boot's reverb
 * clear ran ~400 ticks late after each such edge.  An exception in which
 * no sequencer function ran is charged 21,522.  VBLANK_NO_SEQ_BASE=0 is
 * the negative control. */
#define VBLANK_EXCEPTION_NO_SEQ_TICKS 21522u
/* Weak: harnesses link system.c without the cost tables. */
extern const unsigned cpu_cost_vblank_base __attribute__((weak));
static int vblank_no_seq_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("VBLANK_NO_SEQ_BASE");
        on = !(e && e[0] == '0');
    }
    return on;
}
extern int hal_cpu_cost_vblank_ticks(unsigned *ticks) __attribute__((weak));
extern void hal_cpu_cost_seq_clear(void) __attribute__((weak));
extern void hal_cpu_interrupt_occupancy(u32 ticks);
static void deliver_sdk(void) {
    /* BIOS callbacks precede libetc's late trapIntr acknowledgment. */
    if (GetIntrMask() & hal_irq_status() & 1u) {
        unsigned ticks = VBLANK_EXCEPTION_TICKS;
        hal_irq_ack(1u);
        if (hal_cpu_cost_seq_clear) hal_cpu_cost_seq_clear();
        hal_sdk_irq_vblank();
        if (hal_cpu_cost_vblank_ticks && !hal_cpu_cost_vblank_ticks(&ticks))
            ticks = VBLANK_EXCEPTION_TICKS;
        else if (hal_cpu_cost_vblank_ticks && &cpu_cost_vblank_base &&
                 ticks == cpu_cost_vblank_base && vblank_no_seq_on())
            ticks = VBLANK_EXCEPTION_NO_SEQ_TICKS;
        if (vblank_bare()) ticks = VBLANK_EXCEPTION_BARE_TICKS;
        hal_cpu_interrupt_occupancy(ticks);
    }
}

void hal_vblank_interrupt(void) {
    /* OT_TRACE accounting (src/main/pc-port/prim_arg_fold.c).  Weak so the
     * harness binaries that link this file without the game sources still
     * build; the hook is a no-op unless OT_TRACE is set. */
    extern void ot_trace_tick(void) __attribute__((weak));
    extern void spu_mmio_sync(void) __attribute__((weak));
    /* the frame watchdog counts host time without emulated progress
     * (pc/hal/watchdog.c); weak for harnesses without it */
    extern void pc_watchdog_vblank(void) __attribute__((weak));
    /* PC_TEST_FAULT (pc/src/main.c, tests only); weak for harnesses */
    extern void pc_test_fault_tick(void) __attribute__((weak));
    if (pc_watchdog_vblank) pc_watchdog_vblank();
    if (pc_test_fault_tick) pc_test_fault_tick();
    trace_vblank_begin();
    hal_irq_edge(deliver_edge);
    hal_irq_vblank(deliver_sdk);
    /* SPU registers the game wrote this frame reach the core before this
     * VBlank's audio is mixed (pc/hal/spu.c spu_mmio_sync).  Weak like the
     * OT_TRACE hook, for harnesses that link this file without the SPU. */
    if (spu_mmio_sync) spu_mmio_sync();
    trace_vblank();
    if (ot_trace_tick) ot_trace_tick();
}

/* A VBlank that another interrupt's exception absorbs (pc/hal/cd.c decides
 * when): only libetc's level runs, inside that exception -- trapIntr
 * acknowledges the bit and calls trapIntrVSync -- and the kernel's VBlank
 * events and pad poll never see the edge.  Its CPU time lengthens the
 * exception that absorbed it (cd.c), so nothing is charged here.  Returns
 * 0, doing nothing, when the I_MASK VBlank bit is clear: the edge then
 * stays latched for a later exception, which runs both levels. */
static void deliver_sdk_absorbed(void) {
    if (GetIntrMask() & hal_irq_status() & 1u) {
        hal_irq_ack(1u);
        hal_sdk_irq_vblank();
    }
}

int hal_vblank_absorbed(void) {
    extern void ot_trace_tick(void) __attribute__((weak));
    extern void spu_mmio_sync(void) __attribute__((weak));
    if (!(GetIntrMask() & 1u)) return 0;
    trace_vblank_begin();
    hal_irq_vblank_in_exception(deliver_sdk_absorbed);
    if (spu_mmio_sync) spu_mmio_sync();
    trace_vblank();
    if (ot_trace_tick) ot_trace_tick();
    return 1;
}

s32 hal_load_file(const char *path, void *buf, s32 max_size) {
    char full[1024];
    snprintf(full, sizeof(full), "%s/%s", g_asset_dir, path);
    FILE *f = fopen(full, "rb");
    if (!f) return -1;
    s32 n = fread(buf, 1, max_size, f);
    fclose(f);
    return n;
}

SDL_Renderer *hal_get_renderer(void) {
    return g_renderer;
}
