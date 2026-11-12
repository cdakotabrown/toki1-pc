/* SPDX-License-Identifier: GPL-3.0-or-later */
/* debug.c — Debug tools: logging, scene jump, state editor, save tools
 *
 * Press F12 to toggle debug overlay.
 * Press F2 to open scene select (type scene ID 0-255, Enter to jump).
 * Press F3 to dump current game state to console.
 * Press F5 to save a state, F9 to load it, F6 to pause, F10 to step a
 * frame (the debug toolkit, docs/pc-port/DEBUG-TOOLS.md).  In an
 * interactive run without PC_DBG_SLOT the presenter takes F5 / F9 first
 * for the player's quicksave (pc/hal/quicksave.c).
 *
 * All debug output goes to stdout and optionally to debug.log.
 */
#include "hal.h"
#include "pc_data_root.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>

/* Debug state */
static int g_debug_overlay = 0;
static int g_debug_scene_select = 0;
static char g_scene_input[8] = {0};
static int g_scene_input_pos = 0;
static FILE *g_log_file = NULL;
static int g_frame_count = 0;

int debug_get_frame(void) { return g_frame_count; }
int debug_overlay_is_active(void) { return g_debug_overlay; }

/* External game state pointers (set during init) */
/* The scene dispatch table and current scene ID are at known addresses.
 * For the PC port, we access them as extern globals. */
extern u8 now_step;  /* current scene ID (byte) */

void debug_init(void) {
    char cwd[512];
    if (getcwd(cwd, sizeof(cwd))) {
        printf("[DEBUG] Working directory: %s\n", cwd);
    }
    /* developer artifacts stay out of a player's run (pc_data_root.h) */
    g_log_file = pc_dev_artifacts() ? fopen("debug.log", "w") : NULL;
    if (g_log_file) {
        time_t t = time(NULL);
        fprintf(g_log_file, "=== Heartthrob Memorial PC Enhanced Debug Log ===\n");
        fprintf(g_log_file, "Started: %s\n", ctime(&t));
        printf("[DEBUG] Log file opened: debug.log\n");
    } else if (pc_dev_artifacts()) {
        printf("[DEBUG] ERROR: Cannot open debug.log for writing\n");
    }
    debug_log("Debug system initialized (F12=overlay, F2=scene, F3=state, F5/F9=save/load)");
}

void debug_shutdown(void) {
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
}

void debug_log(const char *fmt, ...) {
    va_list args;
    char buf[1024];

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    printf("[DEBUG] %s\n", buf);
    if (g_log_file) {
        fprintf(g_log_file, "[%06d] %s\n", g_frame_count, buf);
        fflush(g_log_file);
    }
}

extern int soundtest_is_active(void);
extern int soundtest_handle_key(SDL_Event *e);
extern void soundtest_toggle(void);

extern int gfxtest_is_active(void);
extern int gfxtest_handle_key(SDL_Event *e);
extern void gfxtest_toggle(void);

void debug_handle_input(SDL_Event *e) {
    if (e->type != SDL_KEYDOWN) return;

    /* When the sound test is open, let it claim navigation keys (arrows,
     * Tab, Enter, Space) before the normal handlers see them. F8 always
     * toggles open/close regardless. */
    if (e->key.keysym.sym == SDLK_F8) {
        soundtest_toggle();
        return;
    }
    if (soundtest_is_active() && soundtest_handle_key(e)) return;

    /* Graphics test: F7 toggles, claim keys when active */
    if (e->key.keysym.sym == SDLK_F7) {
        gfxtest_toggle();
        return;
    }
    if (gfxtest_is_active() && gfxtest_handle_key(e)) return;

    switch (e->key.keysym.sym) {
    case SDLK_F12:
        g_debug_overlay = !g_debug_overlay;
        debug_log("Debug overlay: %s", g_debug_overlay ? "ON" : "OFF");
        break;

    case SDLK_F2:
        g_debug_scene_select = !g_debug_scene_select;
        g_scene_input_pos = 0;
        memset(g_scene_input, 0, sizeof(g_scene_input));
        if (g_debug_scene_select) {
            debug_log("Scene select: type scene ID (0-255) and press Enter");
        }
        break;

    case SDLK_F3:
        debug_dump_state();
        break;

    case SDLK_F4:
        debug_screenshot("screenshot.ppm");
        break;

    case SDLK_F5:
        debug_save_snapshot(NULL);
        break;

    case SDLK_F9:
        debug_load_snapshot(NULL);
        break;

    case SDLK_F6:   /* debug toolkit: pause / resume at the frame boundary */
        {
            extern void dbg_request_pause_toggle(void);
            dbg_request_pause_toggle();
        }
        break;

    case SDLK_F10:  /* debug toolkit: run one frame, then pause */
        {
            extern void dbg_request_step(int frames);
            dbg_request_step(1);
        }
        break;

    case SDLK_F11:   /* plain F11 / Alt+Enter are the presenter's fullscreen */
        if (!(e->key.keysym.mod & KMOD_SHIFT)) break;
        {
            extern void gpu_toggle_vram_view(void);
            gpu_toggle_vram_view();
        }
        break;

    case SDLK_RETURN:
        if (g_debug_scene_select && g_scene_input_pos > 0) {
            int scene = atoi(g_scene_input);
            if (scene >= 0 && scene <= 255) {
                debug_log("Jumping to scene %d (0x%02X)", scene, scene);
                now_step = scene;
            } else {
                debug_log("Invalid scene ID: %s", g_scene_input);
            }
            g_debug_scene_select = 0;
        }
        break;

    default:
        if (g_debug_scene_select && g_scene_input_pos < 3) {
            char c = (char)e->key.keysym.sym;
            if (c >= '0' && c <= '9') {
                g_scene_input[g_scene_input_pos++] = c;
            }
        }
        break;
    }
}

/* Machine-readable state dump (JSON format for AI/tool parsing) */
void debug_dump_state(void) {
    FILE *f;
    if (!pc_dev_artifacts()) return;            /* a player's run writes no dumps */
    f = fopen("game_state.json", "w");
    if (!f) { debug_log("ERROR: cannot write game_state.json"); return; }

    fprintf(f, "{\n");
    fprintf(f, "  \"frame\": %d,\n", g_frame_count);
    fprintf(f, "  \"scene_id\": %d,\n", now_step);
    /* TODO: add more state as globals are identified */
    /* fprintf(f, "  \"day\": %d,\n", D_800XXXXX); */
    /* fprintf(f, "  \"girl_affection\": [%d, %d, ...],\n", ...); */
    fprintf(f, "  \"debug_overlay\": %s\n", g_debug_overlay ? "true" : "false");
    fprintf(f, "}\n");

    fclose(f);
    debug_log("State dumped to game_state.json (frame=%d, scene=%d)", g_frame_count, now_step);
}

/* F5 / F9: a save state (pc/hal/dbg_state.c).  The key arrives inside
 * VSync's event poll, so the toolkit takes the state at the next main-loop
 * boundary.  NULL path: PC_DBG_SLOT (default state_snapshot.tks). */
void debug_save_snapshot(const char *path) {
    extern void dbg_request_save(const char *path);
    dbg_request_save(path);
    debug_log("Save state requested (%s)", path ? path : "PC_DBG_SLOT");
}

void debug_load_snapshot(const char *path) {
    extern void dbg_request_load(const char *path);
    dbg_request_load(path);
    debug_log("Load state requested (%s)", path ? path : "PC_DBG_SLOT");
}

/* Save VRAM as a raw PPM screenshot */
extern u16 g_vram[512][1024];

static u16 psx_to_rgb565(u16 psx) {
    /* Convert 1555 ABGR to something visible */
    return psx;
}

void debug_screenshot(const char *path) {
    if (!pc_dev_artifacts()) return;
    FILE *f = fopen(path, "wb");
    s32 x, y;
    s32 mainrom_nonzero = 0;
    if (!f) { debug_log("Cannot write screenshot to %s", path); return; }

    /* Check MAINROM integrity before screenshot */
    for (y = 128; y < 512; y++) {
        for (x = 960; x < 1024; x++) {
            if (g_vram[y][x] != 0) mainrom_nonzero++;
        }
    }
    printf("[SCREENSHOT] MAINROM area check: %d non-zero pixels, sample g_vram[128][960]=0x%04X\n",
           mainrom_nonzero, g_vram[128][960]);

    /* Write PPM header (P6 binary RGB) */
    fprintf(f, "P6\n1024 512\n255\n");

    /* Write all VRAM as RGB */
    for (y = 0; y < 512; y++) {
        for (x = 0; x < 1024; x++) {
            u16 psx = g_vram[y][x];
            u8 r = (psx & 0x1F) << 3;
            u8 g = ((psx >> 5) & 0x1F) << 3;
            u8 b = ((psx >> 10) & 0x1F) << 3;
            fputc(r, f);
            fputc(g, f);
            fputc(b, f);
        }
    }
    fclose(f);
    debug_log("Screenshot saved: %s (1024x512 PPM)", path);
}

/* Auto-screenshot every N frames (set to 0 to disable, 60 = ~1/sec) */
static int g_auto_screenshot_interval = 0; /* disabled; use F4 for manual capture */

/* Take a single screenshot after N frames (for headless POC testing) */
static int g_auto_screenshot_at_frame = 30;

/* Verbose frame dump (enabled by VERBOSE_FRAMES env var) */
static int g_verbose_frames = 0;
static int g_verbose_frame_count = 0;

/* Per-frame rendering stats (reset each frame by DrawOTag) */
int g_frame_prim_count = 0;
int g_frame_sprt_count = 0;
int g_frame_tile_count = 0;
int g_frame_poly_count = 0;
int g_frame_dr_mode_count = 0;

/* Current GPU state for debugging (set by gpu.c) */
extern s32 g_tpage_x, g_tpage_y, g_tpage_bpp;
extern s32 g_draw_x, g_draw_y, g_disp_x, g_disp_y;

/* Dump comprehensive frame state to frame_dump.jsonl (one JSON per line) */
static void dump_frame_verbose(void) {
    static FILE *f_dump = NULL;
    extern u8 now_sub_step, now_sub_sub_step;

    if (!f_dump) {
        f_dump = fopen("frame_dump.jsonl", "w");
        if (!f_dump) return;
    }

    fprintf(f_dump, "{\"frame\":%d,\"scene\":%d,\"sub\":%d,\"sub2\":%d,"
            "\"gpu\":{\"tpage_x\":%d,\"tpage_y\":%d,\"bpp\":%d,"
            "\"draw\":[%d,%d],\"disp\":[%d,%d]},"
            "\"prims\":{\"total\":%d,\"sprt\":%d,\"tile\":%d,\"poly\":%d,\"dr_mode\":%d}}\n",
            g_frame_count, now_step, now_sub_step, now_sub_sub_step,
            g_tpage_x, g_tpage_y, g_tpage_bpp,
            g_draw_x, g_draw_y, g_disp_x, g_disp_y,
            g_frame_prim_count, g_frame_sprt_count, g_frame_tile_count,
            g_frame_poly_count, g_frame_dr_mode_count);
    fflush(f_dump);
}

void debug_frame(void) {
    static int init_done = 0;
    g_frame_count++;

    /* Check for verbose mode on first frame */
    if (!init_done) {
        const char *env = getenv("VERBOSE_FRAMES");
        if (env && env[0]) {
            g_verbose_frames = atoi(env);
            if (g_verbose_frames <= 0) g_verbose_frames = 100; /* default 100 frames */
            fprintf(stderr, "[DEBUG] Verbose frame logging enabled for %d frames\n", g_verbose_frames);
        }
        init_done = 1;
    }

    /* Log scene and sub-state changes */
    static s32 last_scene = -1;
    static s32 last_sub = -1;
    {
        extern u8 now_sub_step;
        extern u8 now_sub_sub_step;
        if (now_step != last_scene) {
            debug_log("SCENE_CHANGE old=%d new=%d sub=%d/%d frame=%d",
                      last_scene, now_step, now_sub_step, now_sub_sub_step, g_frame_count);
            last_scene = now_step;
            last_sub = now_sub_step;
        } else if (now_sub_step != last_sub) {
            debug_log("SUB_STATE scene=%d sub=%d->%d a=%d frame=%d",
                      now_step, last_sub, now_sub_step, now_sub_sub_step, g_frame_count);
            last_sub = now_sub_step;
        }
    }

    /* Verbose frame dump (first N frames when VERBOSE_FRAMES env var set) */
    if (g_verbose_frames > 0 && g_verbose_frame_count < g_verbose_frames) {
        dump_frame_verbose();
        g_verbose_frame_count++;
    }

    /* Reset per-frame stats */
    g_frame_prim_count = 0;
    g_frame_sprt_count = 0;
    g_frame_tile_count = 0;
    g_frame_poly_count = 0;
    g_frame_dr_mode_count = 0;

    /* Auto-dump state every 300 frames (~5 seconds) */
    if (g_frame_count % 300 == 0) {
        debug_dump_state();
    }

    /* Auto-screenshot at startup and periodically */
    if (g_auto_screenshot_interval > 0 &&
        (g_frame_count == 1 || g_frame_count % g_auto_screenshot_interval == 0)) {
        char path[64];
        snprintf(path, sizeof(path), "screenshot_%06d.ppm", g_frame_count);
        debug_screenshot(path);
    }

    /* Single auto-screenshot for headless POC testing */
    if (g_auto_screenshot_at_frame > 0 && g_frame_count == g_auto_screenshot_at_frame) {
        printf("[SCREENSHOT] Frame %d: g_vram[20][672]=0x%04X before save\n",
               g_frame_count, g_vram[20][672]);
        debug_screenshot("vram_debug.ppm");
        debug_log("Auto-screenshot taken at frame %d", g_frame_count);
    }

    /* Multi-frame screenshots for progression debugging.
     * SCREENSHOT_FRAMES="30,60,120,180" → vram_debug_<frame>.ppm at each. */
    {
        static int parsed = 0;
        static int frames[16];
        static int nframes = 0;
        if (!parsed) {
            const char *env = getenv("SCREENSHOT_FRAMES");
            if (env && env[0]) {
                const char *p = env;
                while (*p && nframes < 16) {
                    int v = 0;
                    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
                    if (v > 0) frames[nframes++] = v;
                    while (*p && (*p < '0' || *p > '9')) p++;
                }
                fprintf(stderr, "[DEBUG] Multi-frame screenshots at %d points\n", nframes);
            }
            parsed = 1;
        }
        {
            int i;
            for (i = 0; i < nframes; i++) {
                if (g_frame_count == frames[i]) {
                    char path[64];
                    extern u8 now_sub_step;
                    snprintf(path, sizeof(path), "vram_debug_f%04d.ppm", g_frame_count);
                    debug_screenshot(path);
                    debug_log("Multi-screenshot frame=%d scene=%d sub=%d saved: %s",
                              g_frame_count, now_step, now_sub_step, path);
                }
            }
        }
    }
}

/* A modal error dialog for a fault the game cannot recover from
 * (pc/src/main.c): the SDL message box, which works without a window. */
void hal_show_error(const char *title, const char *msg)
{
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, msg, NULL);
}
