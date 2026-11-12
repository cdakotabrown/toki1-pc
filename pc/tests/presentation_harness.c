/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Real SDL software-renderer regression; no window or game assets.
 * Poisoning the backbuffer after every present models SDL's documented
 * invalidation contract, rather than relying on one driver's retention. */
#include "../hal/system.c"
#include "../hal/gpu.c"

u8 now_step, now_sub_step;
u8 graph_env[128];
s16 D_800EBD64 = 1024, D_800EBD66 = 512;
u8 D_800EBD62, libgpu_clearimage_name_str;
u32 libgpu_clr_packet_buf[12];
__asm__(".globl libgpu_clr_restore_packet\n.set libgpu_clr_restore_packet,libgpu_clr_packet_buf+32\n");
const char libgpu_badrect_fmt[] = "label", libgpu_badrect_rect_fmt[] = "rectangle";
const char libgpu_rectdump_name_fmt[] = "label", libgpu_rectdump_rect_fmt[] = "rectangle";
s32 func_800B4850(const char *format, ...) { (void)format; return 0; }
int g_frame_dr_mode_count;
void mdfn_set_drawing_area(int a, int b, int c, int d) { abort(); }
void mdfn_set_draw_offset(int a, int b) { abort(); }
void mdfn_set_texpage(uint16_t p) { abort(); }
void mdfn_set_texwindow(uint8_t a, uint8_t b, uint8_t c, uint8_t d) { abort(); }
void mdfn_set_mask(int a, int b) { abort(); }
void mdfn_clear_cache(void) { abort(); }
void mdfn_invalidate_tex_cache(void) { abort(); }
s32 libapi_video_mode;
u8 libgpu_gp1_shadow_tbl[256], libgpu_graph_reverse_flag;
void (*GPU_printf)(void);
u8 libgpu_putdispenv_trace_fmt;
volatile int g_crash_count;
const char *overlay_get_name(u8 scene) { (void)scene; return "fixture"; }
static int turbo = 1, debug_overlay;
int debug_overlay_is_active(void) { return debug_overlay; }
static Uint32 wall = 1000, waited;
int trace_turbo(void) { return turbo; }
Uint32 __wrap_SDL_GetTicks(void) { return wall; }
void __wrap_SDL_Delay(Uint32 ms) { wall += ms; waited += ms; }
static u64 cpu_ticks;
u64 hal_clock_ticks(void) { return cpu_ticks; }
s32 libgpu_alarm_deadline_vcount, libgpu_alarm_spin_count;
u32 hal_clock_vblanks(void) { return (u32)(cpu_ticks / 566204u); }
/* This fixture delivers every instrumented edge. */
s32 hal_sdk_vcount(void) { return (s32)hal_clock_vblanks(); }
void hal_cpu_spend(u32 ticks) { cpu_ticks += ticks; }
u32 hal_clock_ticks_per_second(void) { return 33868800u; }

/* Compile the actual debug draw entry with asset-independent drawing stubs.
 * The generated include is read from gfx_test.c, not a copied implementation. */
#define VIEW_ROWS 10
static int g_active = 1, g_tim_count, g_cursor, g_3d_mode;
static int g_tmd_loaded, g_cube_rot_x, g_cube_rot_y, g_cube_rot_z;
static int g_tmd_vert_count, g_tmd_prim_count;
static char g_tmd_name[64], g_status[64];
static void gt_text(int x, int y, const char *text, int r, int g, int b) {
    (void)x; (void)y; (void)text; (void)r; (void)g; (void)b;
}
static void draw_tmd_model(int x, int y, int z, int distance) {
    (void)x; (void)y; (void)z; (void)distance;
}
static void draw_3d_cube(void) { }
static void draw_all_primitives(void) { g_vram[40][40] = 0x03e0; }
void debug_screenshot(const char *path) { (void)path; }
void debug_log(const char *format, ...) { (void)format; }
#include "gfx_draw_fixture.h"

static u32 captured[320 * 240];
static int presents;

extern void __real_SDL_RenderPresent(SDL_Renderer *renderer);
void __wrap_SDL_RenderPresent(SDL_Renderer *renderer) {
    if (SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_RGBA8888,
                             captured, 320 * 4))
        exit(90);
    presents++;
    __real_SDL_RenderPresent(renderer);
    SDL_SetRenderDrawColor(renderer, 255, 0, 255, 255);
    SDL_RenderClear(renderer);
}

static int check_clear(const char *name) {
    RECT rect;
    int x, y, inside;
    rect.x = !strcmp(name, "clear-texture") ? 320 :
             !strcmp(name, "clear-mainrom") ? 960 : 0;
    rect.y = !strcmp(name, "clear-buffer1") ? 240 :
             !strcmp(name, "clear-mainrom") ? 128 : 0;
    rect.w = rect.x == 960 ? 64 : rect.x == 320 ? 128 : 320;
    rect.h = rect.x ? 32 : 240;
    for (y = 0; y < VRAM_HEIGHT; y++)
        for (x = 0; x < VRAM_WIDTH; x++) g_vram[y][x] = 0x1234;
#ifdef CLEARIMAGE_LEGACY_TEXTURE_HOOK
    mark_textures_loaded_low_x();
#endif
    g_mainrom_loaded = 1;
    ClearImage(&rect, 0, 248, 0);
    for (y = 0; y < VRAM_HEIGHT; y++) {
        for (x = 0; x < VRAM_WIDTH; x++) {
            inside = x >= rect.x && x < rect.x + rect.w &&
                     y >= rect.y && y < rect.y + rect.h;
            if (g_vram[y][x] != (inside ? 0x03e0 : 0x1234)) {
                fprintf(stderr, "wrong ClearImage destination at %d,%d\n",
                        x, y);
                return 1;
            }
        }
    }
    return 0;
}

static int expect_pixel(int x, int y, u32 color) {
    u32 actual = captured[y * 320 + x];
    if (actual == color) return 0;
    fprintf(stderr, "present=%d pixel=%d,%d actual=%08x expected=%08x\n",
            presents, x, y, actual, color);
    return 1;
}

int main(int argc, char **argv) {
    g_use_mdfn_gpu = 0;
    SDL_Surface *surface;
    int result = 0;
    int i;
    if (argc != 2 || SDL_Init(SDL_INIT_VIDEO)) return 91;
    surface = SDL_CreateRGBSurfaceWithFormat(0, 320, 240, 32,
                                             SDL_PIXELFORMAT_RGBA8888);
    if (!surface) return 92;
    g_renderer = SDL_CreateSoftwareRenderer(surface);
    if (!g_renderer) return 93;
    g_display_tex = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_RGBA8888,
                                      SDL_TEXTUREACCESS_STREAMING, 320, 240);
    if (!g_display_tex) return 94;
    g_disp_x = g_disp_y = 0;
    g_disp_w = 320;
    g_disp_h = 240;
    g_disp_mask = 1;
    for (i = 0; i < 320 * 240; i++)
        g_vram[i / 320][i % 320] = 0x001f;

    DrawSync(0);
    if (!strcmp(argv[1], "repeat")) {
        hal_frame_end();
        result |= expect_pixel(10, 10, 0xf80000ff);
        /* MDEC/LoadImage can update VRAM without another DrawSync. */
        g_vram[10][10] = 0x03e0;
        hal_frame_end();
        result |= expect_pixel(10, 10, 0x00f800ff);
        hal_frame_end();
        result |= expect_pixel(10, 10, 0x00f800ff);
    } else if (!strcmp(argv[1], "mask")) {
        SetDispMask(0);
        hal_frame_end();
        for (i = 0; i < 320 * 240; i++) {
            if (captured[i] != 0x000000ff) { result = 1; break; }
        }
        if (g_vram[10][10] != 0x001f) result = 2;
    } else if (!strcmp(argv[1], "size")) {
        g_disp_w = g_disp_h = 16;
        gpu_display();
        hal_frame_end();
        result |= expect_pixel(10, 10, 0xf80000ff);
        result |= expect_pixel(100, 100, 0x000000ff);
    } else if (!strcmp(argv[1], "origin")) {
        g_disp_x = g_disp_y = 1;
        g_vram[1][1] = 0x03e0;
        gpu_display();
        hal_frame_end();
        result |= expect_pixel(0, 0, 0x00f800ff);
        if (g_disp_x != 1 || g_disp_y != 1) result = 2;
    } else if (!strcmp(argv[1], "dispenv")) {
        u32 storage[5];
        u8 *bytes = (u8 *)storage;
        memset(storage, 0xff, sizeof(storage));
        SetDefDispEnv((DISPENV *)storage, 5, 1, 2, 1);
        if (sizeof(DISPENV) != 20) result = 1;
        for (i = 16; i < 20; i++) if (bytes[i]) result = 2;
    } else if (!strcmp(argv[1], "rgb24")) {
        u32 storage[5];
        DISPENV *env = (DISPENV *)storage;
        memset(storage, 0, sizeof(storage));
        SetDefDispEnv(env, 5, 1, 2, 1);
        ((u8 *)env)[17] = 1; /* retail DISPENV.isrgb24 offset */
        g_vram[1][5] = 0x7f01;
        g_vram[1][6] = 0xf6ff;
        g_vram[1][7] = 0x4121;
        PutDispEnv(env);
        hal_frame_end();
        result |= expect_pixel(0, 0, 0x017fffff);
        result |= expect_pixel(1, 0, 0xf62141ff);
        result |= expect_pixel(2, 0, 0x000000ff);
        ((u8 *)env)[17] = 0;
        PutDispEnv(env);
        hal_frame_end();
        result |= expect_pixel(0, 0, psx_to_rgba(0x7f01));
    } else if (!strcmp(argv[1], "hud")) {
        hal_frame_end();
        result |= expect_pixel(10, 235, 0xf80000ff);
        debug_overlay = 1;
        hal_frame_end();
        if (captured[235 * 320 + 10] == 0xf80000ff) result = 2;
        debug_overlay = 0;
        hal_frame_end();
        result |= expect_pixel(10, 235, 0xf80000ff);
    } else if (!strncmp(argv[1], "clear-", 6)) {
        result = check_clear(argv[1]);
    } else if (!strncmp(argv[1], "debug", 5)) {
        u64 debug_start_tick;
        if (!strcmp(argv[1], "debug-rgb24")) {
            u32 storage[5];
            DISPENV *env = (DISPENV *)storage;
            int x, y;
            for (y = 240; y < 480; y++)
                for (x = 640; x < 960; x++) g_vram[y][x] = 0x001f;
            memset(storage, 0, sizeof(storage));
            SetDefDispEnv(env, 640, 240, 320, 240);
            ((u8 *)env)[17] = 1;
            PutDispEnv(env);
        }
        turbo = !strcmp(argv[1], "debug-turbo");
        g_disp_x = 640;
        g_disp_y = 240;
        g_draw_x = 17;
        g_draw_y = 23;
        debug_start_tick = hal_clock_ticks();
        libgpu_alarm_deadline_vcount = 17;
        libgpu_alarm_spin_count = 23;
        for (i = 0; i < 60; i++) {
            g_frame_start = wall;
            g_frame_start_tick = debug_start_tick;
            if (!strcmp(argv[1], "debug-work")) wall += 5;
            gfxtest_draw();
            result |= expect_pixel(40, 40, 0x00f800ff);
            if (g_disp_x != 640 || g_disp_y != 240 ||
                g_draw_x != 17 || g_draw_y != 23) result = 2;
        }
        if (presents != 60 || hal_clock_ticks() != debug_start_tick ||
            libgpu_alarm_deadline_vcount != 17 || libgpu_alarm_spin_count != 23) result = 3;
        if (waited != (turbo ? 0 :
            !strcmp(argv[1], "debug-work") ? 700 : 1000)) result = 4;
        if (!strcmp(argv[1], "debug-rgb24")) {
            hal_frame_end();
            result |= expect_pixel(10, 10, 0x1f001fff);
        }
    } else {
        result = 95;
    }
    SDL_DestroyTexture(g_display_tex);
    SDL_DestroyRenderer(g_renderer);
    SDL_FreeSurface(surface);
    SDL_Quit();
    return result;
}
