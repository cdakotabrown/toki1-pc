/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Actual HAL reset and GPU state; no game assets or replacement renderer. */
#include <assert.h>

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#ifndef GPU_SOURCE
#define GPU_SOURCE "../hal/gpu.c"
#endif
#include GPU_SOURCE
/* Isolated GPU fixture: all its instrumented VBlanks are delivered. */
extern u32 hal_clock_vblanks(void);
s32 hal_sdk_vcount(void) { return (s32)hal_clock_vblanks(); }


int g_frame_prim_count, g_frame_sprt_count, g_frame_tile_count;
int g_frame_poly_count, g_frame_dr_mode_count;
u8 sdk_work[0x19C0 + 32];
s32 _qin, _qout, libapi_video_mode;
s32 libgpu_alarm_deadline_vcount, libgpu_alarm_spin_count;
/* These labels reproduce the overlapping PS1 SDK storage exactly. */
__asm__(".globl graph_env\n.set graph_env,sdk_work\n"
        ".globl libgpu_graph_reverse_flag\n.set libgpu_graph_reverse_flag,sdk_work+3\n"
        ".globl D_800EBD64\n.set D_800EBD64,sdk_work+4\n"
        ".globl D_800EBD66\n.set D_800EBD66,sdk_work+6\n"
        ".globl D_800EBD62\n.set D_800EBD62,sdk_work+2\n"
        ".globl libgpu_clr_packet_buf\n.set libgpu_clr_packet_buf,sdk_work+128\n"
        ".globl libgpu_clr_restore_packet\n.set libgpu_clr_restore_packet,sdk_work+160\n"
        ".globl libgpu_gp1_shadow_tbl\n.set libgpu_gp1_shadow_tbl,sdk_work+192\n"
        ".globl _que\n.set _que,sdk_work+448\n");
void (*GPU_printf)(void);
u8 libgpu_putdispenv_trace_fmt, libgpu_clearimage_name_str;
const char libgpu_badrect_fmt[] = "diagnostic-label";
const char libgpu_badrect_rect_fmt[] = "diagnostic-rectangle";
const char libgpu_rectdump_name_fmt[] = "diagnostic-always-label";
const char libgpu_rectdump_rect_fmt[] = "diagnostic-always-rectangle";
static int callbacks, backend_calls;
#ifndef GPU_REAL_CLOCK
static u64 clock_ticks;
u64 hal_clock_ticks(void) { return clock_ticks; }
u32 hal_clock_vblanks(void) { return (u32)(clock_ticks / 566204u); }
void hal_cpu_spend(u32 ticks) { clock_ticks += ticks; }
#endif
void ResetCallback(void) { callbacks++; }
/* I_MASK as libgpu sees it: the span each SDK call spends masked. */
static u32 intr_mask = 0x2du;
static u64 masked_from, masked_ticks;
static int mask_writes;
u32 SetIntrMask(s32 value) {
    u32 old = intr_mask;
#ifndef GPU_REAL_CLOCK
    if (old && !value) masked_from = clock_ticks;
    if (!old && value) masked_ticks += clock_ticks - masked_from;
#endif
    intr_mask = (u32)value;
    mask_writes++;
    return old;
}
void mdfn_set_drawing_area(int x0, int y0, int x1, int y1) {
    assert(x0 == 0 && y0 == 0 && x1 == 0 && y1 == 0);
    backend_calls++;
}
void mdfn_set_draw_offset(int x, int y) {
    assert(x == 0 && y == 0); backend_calls++;
}
void mdfn_set_texpage(uint16_t page) {
    assert(page == 0); backend_calls++;
}
void mdfn_set_texwindow(uint8_t x, uint8_t y, uint8_t ox, uint8_t oy) {
    assert(x == 0 && y == 0 && ox == 0 && oy == 0); backend_calls++;
}
void mdfn_set_mask(int a, int b) {
    assert(a == 0 && b == 0); backend_calls++;
}
/* GP1(00h) also clears the texture and CLUT caches. */
void mdfn_clear_cache(void) { backend_calls++; }
void mdfn_invalidate_tex_cache(void) { abort(); }
/* DrawOTag is linked for the sdk-cpu timing case, which submits an empty
 * OT natively; the Mednafen drawing entry points must never be reached. */
void mdfn_set_clut(uint16_t packed) { (void)packed; abort(); }
void mdfn_draw_triangle(const MdfnTri *t) { (void)t; abort(); }
void mdfn_draw_sprite(const MdfnSprite *s) { (void)s; abort(); }
void mdfn_draw_line(const MdfnLine *l) { (void)l; abort(); }

static void seed(void) {
    memset(sdk_work, 0xa5, sizeof(sdk_work));
    memset(g_vram, 0x12, sizeof(g_vram));
    _qin = 7; _qout = 8;
    g_disp_x = 8; g_disp_y = 9; g_disp_w = 640; g_disp_h = 480;
    g_disp_rgb24 = 1; g_disp_mask = 1;
    g_draw_x = 33; g_draw_y = 44; g_draw_w = 320; g_draw_h = 240;
    g_tpage_x = 14; g_tpage_y = 1; g_tpage_bpp = 2; g_tpage_abr = 3;
    g_mask_set_or = g_mask_eval_and = 0x8000;
    g_tex_window_mask_x = 1; g_tex_window_mask_y = 2;
    g_tex_window_off_x = 3; g_tex_window_off_y = 4;
    g_dither_enabled = 1;
}
static void check_environment_cache(void) {
    struct { DRAWENV env; u32 guard; } box;
    DISPENV disp;
    int i;
    g_use_mdfn_gpu = 0;
    ResetGraph(0);
    assert(sizeof(DRAWENV) == 92 && offsetof(DRAWENV, dr_env) == 28);
    memset(&box, 0xa5, sizeof(box));
    assert(SetDefDrawEnv(&box.env, 0, 240, 320, 240) == &box.env);
    assert(box.env.ofs[0] == 0 && box.env.ofs[1] == 240);
    assert(box.env.dtd == 1 && box.env.dfe == 1 && box.env.tpage == 10);
    for (i = 0; i < 16; i++) assert(box.env.dr_env[i] == 0xa5a5a5a5u);
    box.env.tw.x = 24; box.env.tw.y = 40;
    box.env.tw.w = 64; box.env.tw.h = 128;
    g_mask_set_or = g_mask_eval_and = 0x8000;
    assert(PutDrawEnv(&box.env) == &box.env);
    assert(box.guard == 0xa5a5a5a5u);
    assert(box.env.dr_env[0] == 0x06ffffffu);
    assert(box.env.dr_env[4] == 0xe100060au);
    assert(g_mask_set_or == 0 && g_mask_eval_and == 0);
    assert(g_tex_window_mask_x == 24 && g_tex_window_mask_y == 16);
    assert(g_tex_window_off_x == 3 && g_tex_window_off_y == 5);
    assert(!memcmp(sdk_work+16, &box.env, 92));
    libapi_video_mode = 1;
    assert(SetDefDispEnv(&disp, 32, 64, 320, 240) == &disp);
    assert(PutDispEnv(&disp) == &disp && disp.pad0 == 1);
    assert(!memcmp(sdk_work+108, &disp, 20));
    assert(box.guard == 0xa5a5a5a5u);
}

static void check_environment_clear(int aligned, int edge) {
    DRAWENV env;
    int x, y, start, width;
    g_use_mdfn_gpu = 0;
    ResetGraph(0);
    memset(g_vram, 0x12, sizeof(g_vram));
    start = edge ? (aligned ? 960 : 1017) : (aligned ? 64 : 3);
    width = aligned ? (edge ? 128 : 64) : 17;
    SetDefDrawEnv(&env, start, 20, width, 8);
    if (!aligned) {
        env.ofs[0] = -160; env.ofs[1] = -120;
    }
    env.isbg = 1; env.r0 = 248;
    g_mask_set_or = g_mask_eval_and = 0x8000;
    PutDrawEnv(&env);
    assert(env.dr_env[0] == 0x09ffffffu);
    assert((env.dr_env[7] >> 24) == (aligned ? 2 : 0x60));
    for (y = 0; y < 512; y++)
        for (x = 0; x < 1024; x++)
            assert(g_vram[y][x] == (y >= 20 && y < 28 &&
                   (edge ? (aligned && (x >= 960 || x < 64)) :
                    (x >= start && x < start+width)) ? 31 : 0x1212));
    assert(!memcmp(sdk_work+16, &env, 92));
}

/* GPU reset tests spy on the BIOS boundary; printf has its own real-clock
 * fixture. These are authored diagnostic data, not game strings. */
u8 libgpu_resetgraph_jtb_fmt, libgpu_default_jtb_tbl;
static int bios_prints, clear_debug, clear_debug_calls;
static u32 queue_word(s32 offset);
s32 func_800B4850(const char *format, ...) {
    va_list args;
    if (clear_debug) {
        assert(format == libgpu_badrect_fmt || format == libgpu_badrect_rect_fmt ||
               format == libgpu_rectdump_name_fmt || format == libgpu_rectdump_rect_fmt);
        assert(queue_word(8) == 0);
        assert(sdk_work[128] == 0xa5);
        va_start(args, format);
        if (format == libgpu_badrect_fmt || format == libgpu_rectdump_name_fmt) {
            const char *label = va_arg(args, const char *);
            assert((unsigned long)label ==
                   (u32)(unsigned long)&libgpu_clearimage_name_str);
        } else {
            assert(va_arg(args, int) == (clear_debug == 1 ? -3 : 3));
            assert(va_arg(args, int) == (clear_debug == 1 ? -4 : 20));
            assert(va_arg(args, int) == 17);
            assert(va_arg(args, int) == 8);
        }
        va_end(args);
        clear_debug_calls++;
        return 0;
    }
    assert(format == (const char *)&libgpu_resetgraph_jtb_fmt);
    bios_prints++;
    va_start(args, format);
    assert(va_arg(args, u32) == (u32)(unsigned long)&libgpu_default_jtb_tbl);
    assert(va_arg(args, u32) == (u32)(unsigned long)sdk_work);
    va_end(args);
    return 16;
}

u8 now_step;
static int queue_calls_a, queue_calls_b, queue_check_pixel, queue_reenter;
static DRAWENV *queue_expected_env;
static int queue_chain, queue_reset_inside;
static void queue_callback_b(void);

static u32 queue_word(s32 offset) {
    u32 value;
    memcpy(&value, sdk_work + offset, 4);
    return value;
}

static void queue_callback_a(void) {
    RECT rect;
    queue_calls_a++;
    assert(queue_word(8) == 0);
    assert(DrawSync(1) == 0);
    if (queue_check_pixel) assert(g_vram[0][0] == 31);
    if (queue_expected_env)
        assert(!memcmp(sdk_work + 16, queue_expected_env, 92));
    if (queue_reenter) {
        assert(DrawSyncCallback(0) == queue_callback_a);
        rect.x = 0; rect.y = 1; rect.w = rect.h = 1;
        ClearImage(&rect, 0, 248, 0);
        assert(g_vram[1][0] == 0x3e0);
    }
    if ((queue_chain || queue_reset_inside) && queue_calls_a == 1) {
        if (queue_reset_inside) {
            ResetGraph(0);
            assert(DrawSyncCallback(queue_callback_b) == 0);
        }
        rect.x = 0; rect.y = 1; rect.w = rect.h = 1;
        ClearImage(&rect, 0, 248, 0);
        assert(queue_calls_a == 1 && queue_calls_b == 0);
        assert(DrawSync(1) == 0);
        assert(queue_calls_a == 1 && queue_calls_b == 0);
    }
}

static void queue_callback_b(void) {
    assert(queue_word(8) == 0);
    queue_calls_b++;
}

static void check_queue_completion(int scenario) {
    RECT rect;
    DRAWENV env;
    DISPENV disp;
    u32 outer, inner, saved;
    g_use_mdfn_gpu = 0;
    ResetGraph(0);
    rect.x = rect.y = 0; rect.w = rect.h = 1;
    if (scenario == 0) {
#ifdef GPU_QUEUE_REQUIRE_HIGH_CALLBACK
        {
            GpuSyncCallback function = queue_callback_a;
            u64 address = 0;
            assert(sizeof(function) <= sizeof(address));
            memcpy(&address, &function, sizeof(function));
            assert((address >> 32) != 0);
        }
#endif
        SetDefDispEnv(&disp, 0, 0, 320, 240);
        PutDispEnv(&disp);
        SetDispMask(1);
        assert(queue_word(8) == 0);
        ClearImage(&rect, 248, 0, 0);
        assert(queue_word(8) == 1);
        assert(DrawSync(0) == 0 && queue_word(8) == 1);
        /* Registration and an idle wait do not manufacture a completion. */
        saved = queue_word(16);
        assert(DrawSyncCallback(queue_callback_a) == 0);
        assert(queue_word(16) == saved && queue_word(12) != 0);
        assert(DrawSync(1) == 0 && queue_calls_a == 0);
        queue_check_pixel = 1;
        ClearImage(&rect, 248, 0, 0);
        assert(queue_calls_a == 1 && queue_word(8) == 0);
        assert(DrawSyncCallback(queue_callback_b) == queue_callback_a);
        assert(DrawSyncCallback(0) == queue_callback_b);
        assert(queue_word(12) == 0 && queue_word(16) == saved);
    } else if (scenario == 1) {
        DrawSyncCallback(queue_callback_a);
        outer = gpu_queue_begin();
        inner = gpu_queue_begin();
        assert(DrawSync(1) == 1 && DrawSync(0) == -1);
        gpu_queue_end(inner);
        assert(queue_calls_a == 0 && DrawSync(1) == 1);
        gpu_queue_end(outer);
        assert(queue_calls_a == 1 && DrawSync(0) == 0);
    } else if (scenario == 2) {
        queue_reenter = queue_check_pixel = 1;
        DrawSyncCallback(queue_callback_a);
        ClearImage(&rect, 248, 0, 0);
        assert(queue_calls_a == 1 && queue_word(8) == 1);
        assert(DrawSyncCallback(0) == 0);
    } else if (scenario == 3) {
        DrawSyncCallback(queue_callback_a);
        outer = gpu_queue_begin();
        ResetGraph(0);
        inner = gpu_queue_begin();
        gpu_queue_end(outer);
        assert(DrawSync(1) == 1 && queue_calls_a == 0);
        gpu_queue_end(inner);
        assert(DrawSync(1) == 0 && queue_calls_a == 0);
        assert(DrawSyncCallback(0) == 0);
        DrawSyncCallback(queue_callback_a);
        outer = gpu_queue_begin();
        ResetGraph(1);
        gpu_queue_end(outer);
        assert(queue_calls_a == 0 && DrawSync(1) == 0);
        ClearImage(&rect, 248, 0, 0);
        assert(queue_calls_a == 1);
        assert(DrawSyncCallback(0) == queue_callback_a);
    } else if (scenario == 4) {
        DrawSyncCallback(queue_callback_a);
        sdk_work[1] = 0;
        ClearImage(&rect, 248, 0, 0);
        assert(queue_calls_a == 0 && queue_word(8) == 1);
        sdk_work[1] = 1;
        assert(DrawSync(0) == 0 && queue_calls_a == 0);
        ClearImage(&rect, 248, 0, 0);
        assert(queue_calls_a == 1 && queue_word(8) == 0);
    } else if (scenario == 5) {
        DrawSyncCallback(queue_callback_a);
        SetDefDrawEnv(&env, 3, 4, 17, 8);
        env.isbg = 1; env.r0 = 248;
        queue_expected_env = &env;
        PutDrawEnv(&env);
        assert(queue_calls_a == 1);
        assert(g_vram[4][3] == 31 && queue_word(8) == 0);
        assert(!memcmp(sdk_work + 16, &env, 92));
    } else if (scenario >= 7) {
        queue_chain = scenario == 7;
        queue_reset_inside = scenario == 8;
        DrawSyncCallback(queue_callback_a);
        ClearImage(&rect, 248, 0, 0);
        assert(queue_calls_a == 1 && queue_calls_b == 0);
        assert(queue_word(8) == 1);
        assert(DrawSync(1) == 0);
        assert(queue_calls_a == (scenario == 7 ? 2 : 1));
        assert(queue_calls_b == (scenario == 8 ? 1 : 0));
        assert(queue_word(8) == 0);
    } else {
        DrawSyncCallback(queue_callback_a);
        outer = gpu_queue_begin();
        assert(DrawSyncCallback(queue_callback_b) == queue_callback_a);
        gpu_queue_end(outer);
        assert(queue_calls_b == 1 && queue_calls_a == 0);
        outer = gpu_queue_begin();
        memset(sdk_work + 12, 0, 4);
        gpu_queue_end(outer);
        assert(queue_calls_b == 1 && queue_word(8) == 1);
        assert(DrawSyncCallback(0) == 0);
    }
}

#ifndef GPU_REAL_CLOCK
/* The vblank_function sequence after VSync(0), with retail's measured CPU
 * spans (reference Interpreter, warm boot frames): the native
 * clock must advance by exactly these, masking where libgpu does. */
static u64 timed_from;
static void timed_begin(void) {
    timed_from = clock_ticks; masked_ticks = 0; mask_writes = 0;
}
static void timed_end(u64 total, u64 masked, int writes) {
    assert(clock_ticks - timed_from == total);
    assert(masked_ticks == masked);
    assert(mask_writes == writes);
    assert(intr_mask == 0x2du);
}

static void check_sdk_cpu(void) {
    DRAWENV draw;
    DISPENV disp;
    u32 ot[1];
    g_use_mdfn_gpu = 0;
    ResetGraph(0);
    SetDefDispEnv(&disp, 0, 0, 320, 240);
    SetDefDrawEnv(&draw, 0, 0, 320, 240);
    timed_begin(); ResetGraph(1); timed_end(1001, 247, 2);
    timed_begin(); PutDispEnv(&disp); timed_end(942, 0, 0);
    timed_begin(); SetDispMask(1); timed_end(100, 0, 0);
    timed_begin(); PutDrawEnv(&draw); timed_end(1443, 257, 2);
    ot[0] = 0x00ffffffu;
    timed_begin(); DrawOTag(ot); timed_end(1404, 1167, 2);
    /* The other ResetGraph modes are unmeasured and stay untimed. */
    timed_begin(); ResetGraph(3); timed_end(0, 0, 0);
}
#endif

extern u32 _getctl(u32 command);

static void check_display_control(int scenario) {
    DISPENV env;
    static const int widths[] = { 280, 281, 352, 353, 400, 401, 560, 561 };
    static const int modes[] = { 0, 1, 1, 64, 64, 2, 2, 3 };
    int i;
    g_use_mdfn_gpu = 0;
    ResetGraph(0);
    libapi_video_mode = 0;
    SetDefDispEnv(&env, 5, 240, 320, 240);
    if (scenario == 0) {
        assert(PutDispEnv(&env) == &env);
        assert(_getctl(5) == 5 && _getctl(6) == 0x60);
        assert(_getctl(7) == 16 && _getctl(8) == 1);
        assert(!memcmp(sdk_work + 108, &env, 20));
        /* Origin is always issued; unchanged screen/mode fields are not. */
        sdk_work[192 + 5] = sdk_work[192 + 6] = 0xa5;
        sdk_work[192 + 7] = sdk_work[192 + 8] = 0xa5;
        libapi_video_mode = 1;
        PutDispEnv(&env);
        assert(env.pad0 == 0 && _getctl(5) == 5);
        assert(_getctl(6) == 0xa5 && _getctl(7) == 0xa5);
        assert(_getctl(8) == 0xa5);
        env.disp.x = 6;
        PutDispEnv(&env);
        assert(env.pad0 == 1 && _getctl(5) == 6 && _getctl(8) == 9);
        assert(_getctl(6) == 0xa5 && _getctl(7) == 0xa5);
        SetDispMask(0);
        assert(_getctl(3) == 1 && !g_disp_mask);
        SetDispMask(-1);
        assert(_getctl(3) == 0 && g_disp_mask);
        /* Reset invalidation forces both producers to emit again. */
        ResetGraph(1);
        PutDispEnv(&env);
        assert(_getctl(6) == 0x60 && _getctl(7) == 19);
        assert(_getctl(8) == 9);
    } else if (scenario == 1) {
        for (i = 0; i < 8; i++) {
            env.disp.w = widths[i];
            PutDispEnv(&env);
            assert(_getctl(8) == (u32)modes[i]);
        }
        sdk_work[3] = 1;
        env.isinter = 1; env.isrgb24 = 1; env.disp.h = 480;
        PutDispEnv(&env);
        assert(_getctl(8) == 0xb7);
        libapi_video_mode = 1;
        env.disp.h = 288;
        PutDispEnv(&env);
        assert(_getctl(8) == 0xbb);
        env.disp.h = 289;
        PutDispEnv(&env);
        assert(_getctl(8) == 0xbf);
    } else {
        env.screen.x = -100; env.screen.y = -100;
        PutDispEnv(&env);
        assert(_getctl(6) == 0xf4 && _getctl(7) == 0);
        env.screen.x = 1000; env.screen.y = 1000;
        PutDispEnv(&env);
        assert(_getctl(6) == (3290 & 255));
        assert(_getctl(7) == 254);
        libapi_video_mode = 1;
        env.screen.y = 1001;
        PutDispEnv(&env);
        assert(_getctl(7) == (310 & 255));
        /* Type-specific origin calculation remains the matched get_dx. */
        sdk_work[0] = 2; sdk_work[3] = 1;
        env.disp.x = 16; env.disp.w = 320;
        PutDispEnv(&env);
        assert(_getctl(5) == ((-16 - 320 / 2 + 1024) & 255));
    }
}

static const s16 clear_cases[][4] = {
    {64,20,64,8}, {3,20,17,8}, {960,508,128,8}, {1017,508,17,8},
    {-64,-4,64,8}, {-3,-4,17,8}, {0,0,1024,512}, {3,20,17,512},
    {3,20,-1,8}, {0,20,1025,8}, {0,20,64,513}, {0,20,64,0},
    {0,510,64,8}, {1000,20,64,8}, {3,20,1025,8}, {3,20,1023,1},
    {0,20,0,8}, {0,20,-64,8}, {3,20,17,-1}, {2047,20,17,8},
    {2048,1023,64,8}, {3,2047,17,8}
};
static int clear_completions;
static void clear_complete(void) {
    clear_completions++;
    assert(gpu_clear_get_info(3) == ((9u << 10) | 7u));
    assert(gpu_clear_get_info(4) == ((40u << 10) | 34u));
    assert(g_draw_x == -3 && g_draw_y == 4);
    assert(g_mask_set_or == 0 && g_mask_eval_and == 0);
}
static int clear_inside(int scenario, int x, int y) {
    switch (scenario) {
    case 0: return x>=64 && x<128 && y>=20 && y<28;
    case 1: return x>=3 && x<20 && y>=20 && y<28;
    case 2: return (x>=960 || x<64) && (y>=508 || y<4);
    case 3: return x>=1017 && (y>=508 || y<4);
    case 4: return x>=960 && (y>=508 || y<4);
    case 5: return x<14 && y<4;
    case 12: return x<64 && (y>=510 || y<6);
    case 13: return x>=1000 && y>=20 && y<28;
    case 15: return x>=3 && y==20;
    case 19: return x<16 && y>=20 && y<28;
    case 20: return x<64 && (y==511 || y<7);
    case 21: return x>=3 && x<20 && y<7;
    default: return 0;
    }
}
static void check_sdk_clear(int scenario, const char *dump) {
    RECT rect;
    FILE *file;
    u32 saved[3];
    int x, y;
    g_use_mdfn_gpu = 0;
    ResetGraph(0);
    if (scenario == 24 || scenario == 27) {
        void *mapping;
        ClearImage(NULL, 248, 0, 0);
        ClearImage((RECT *)1, 248, 0, 0);
        assert(queue_word(8) == 0);
        if (scenario == 27) return;
        mapping = mmap((void *)0x801d0000ul, 4096,
            PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|
            MAP_FIXED_NOREPLACE, -1, 0);
        assert(mapping == (void *)0x801d0000ul);
        memcpy(mapping, clear_cases[0], 8);
        ClearImage((RECT *)0xffffffff801d0000ul, 248, 0, 0);
        assert(g_vram[20][64] == 31);
        assert(munmap(mapping, 4096) == 0);
        return;
    }
    gpu_draw_command(0xe3000000u | (9u << 10) | 7u);
    gpu_draw_command(0xe4000000u | (40u << 10) | 34u);
    gpu_draw_command(0xe5000000u | (4u << 11) | 2045u);
    for (x=0; x<3; x++) saved[x] = gpu_clear_get_info(x+3);
    if (scenario == 26) {
        gpu_draw_command(0xe3000000u | (600u << 10) | 7u);
        gpu_draw_command(0xe4000000u | (900u << 10) | 34u);
        memcpy(&rect, clear_cases[1], 8);
        ClearImage(&rect, 248, 0, 0);
        assert(gpu_clear_get_info(3) == ((600u << 10) | 7u));
        assert(gpu_clear_get_info(4) == ((900u << 10) | 34u));
        assert(queue_word(164) == (0xe3000000u | (600u << 10) | 7u));
        assert(queue_word(168) == (0xe4000000u | (900u << 10) | 34u));
        assert(g_vram[20][3] == 31);
        return;
    }
    if (scenario == 25) {
        ResetGraph(1);
        for (x=0; x<3; x++) assert(gpu_clear_get_info(x+3)==saved[x]);
        ResetGraph(0);
        for (x=0; x<3; x++) assert(gpu_clear_get_info(x+3)==0);
        return;
    }
    gpu_draw_command(0xe6000003u);
    assert(g_mask_set_or == 0x8000 && g_mask_eval_and == 0x8000);
    for (y=0; y<512; y++) for (x=0; x<1024; x++) g_vram[y][x]=0x4210;
    if (scenario == 22 || scenario == 23) {
        clear_debug = scenario == 22 ? 1 : 2;
        sdk_work[2] = scenario == 22 ? 1 : 2;
        memcpy(&rect, clear_cases[scenario == 22 ? 5 : 1], 8);
        memset(sdk_work+128, 0xa5, 48);
    } else {
        assert(scenario >= 0 && scenario < 22);
        memcpy(&rect, clear_cases[scenario], 8);
    }
    DrawSyncCallback(clear_complete);
    ClearImage(&rect, 248, 0, 0);
    assert(clear_completions == 1 && queue_word(8) == 0);
    for (x=0; x<3; x++) assert(gpu_clear_get_info(x+3)==saved[x]);
    assert(g_draw_w==28 && g_draw_h==32);
    if (clear_debug) { assert(clear_debug_calls == 2); return; }
    for (y=0; y<512; y++) for (x=0; x<1024; x++)
        assert(g_vram[y][x] == (clear_inside(scenario,x,y) ? 31 : 0x4210));
    /* Complete packet and pixel dumps remain raw for reference comparison. */
    if (dump) {
        file=fopen(dump,"wb"); assert(file);
        assert(fwrite(g_vram,1,sizeof(g_vram),file)==sizeof(g_vram));
        assert(fclose(file)==0);
        printf("packet=");
        for (x=0; x<12; x++) printf("%08x",queue_word(128+4*x));
        printf("\n");
    }
}

int main(int argc, char **argv) {
    u32 tag = 0, target = 0;
    DRAWENV env;
    int mode, full, backend, i, x, y;
    if (!strncmp(argv[1], "sdk-clear-", 10)) {
        check_sdk_clear(atoi(argv[1]+10), argc>3 ? argv[3] : NULL);
        return 0;
    }
    if (!strncmp(argv[1], "queue-", 6)) {
        check_queue_completion(atoi(argv[1] + 6)); return 0;
    }
    if (!strncmp(argv[1], "display-", 8)) {
        check_display_control(atoi(argv[1] + 8)); return 0;
    }
    if (!strcmp(argv[1], "cache")) {
        check_environment_cache(); return 0;
    }
#ifndef GPU_REAL_CLOCK
    if (!strcmp(argv[1], "sdk-cpu")) {
        check_sdk_cpu(); return 0;
    }
#endif
    if (!strcmp(argv[1], "clear-aligned")) {
        check_environment_clear(1, 0); return 0;
    }
    if (!strcmp(argv[1], "clear-tile")) {
        check_environment_clear(0, 0); return 0;
    }
    if (!strcmp(argv[1], "clear-fill-edge")) {
        check_environment_clear(1, 1); return 0;
    }
    if (!strcmp(argv[1], "clear-tile-edge")) {
        check_environment_clear(0, 1); return 0;
    }
    if (!strcmp(argv[1], "clear-wrapped-offset")) {
        g_use_mdfn_gpu = 0;
        ResetGraph(0);
        memset(g_vram, 0x12, sizeof(g_vram));
        SetDefDrawEnv(&env, 3, 20, 17, 8);
        env.ofs[0] = 2047; env.ofs[1] = -2048;
        env.isbg = 1; env.r0 = 248;
        PutDrawEnv(&env);
        assert(g_draw_x == -1 && g_draw_y == 0);
        for (y = 0; y < 512; y++)
            for (x = 0; x < 1024; x++)
                assert(g_vram[y][x] == (x >= 3 && x < 20 &&
                       y >= 20 && y < 28 ? 31 : 0x1212));
        return 0;
    }
    if (!strcmp(argv[1], "clear-zero-hardware-height")) {
        g_use_mdfn_gpu = 0;
        ResetGraph(0);
        memset(g_vram, 0x12, sizeof(g_vram));
        SetDefDrawEnv(&env, 3, 0, 17, 512);
        env.isbg = 1; env.r0 = 248;
        PutDrawEnv(&env);
        for (y = 0; y < 512; y++)
            for (x = 0; x < 1024; x++) assert(g_vram[y][x] == 0x1212);
        return 0;
    }
    if (!strcmp(argv[1], "dither")) {
        g_use_mdfn_gpu = 0;
        assert(gpu_draw_command(0xe1000000u));
        assert(dither_and_pack(0, 0, 128, 128, 128) == 0x4210);
        assert(gpu_draw_command(0xe1000200u));
        assert(dither_and_pack(0, 0, 128, 128, 128) == 0x3def);
        memset(&env, 0, sizeof(env));
        PutDrawEnv(&env);
        assert(dither_and_pack(0, 0, 128, 128, 128) == 0x4210);
        env.dtd = 1;
        PutDrawEnv(&env);
        assert(dither_and_pack(0, 0, 128, 128, 128) == 0x3def);
        return 0;
    }
    mode = atoi(argv[1]); backend = atoi(argv[2]);
    full = (mode & 3) == 0;
    g_use_mdfn_gpu = backend;
    seed();
    pc_ot_setaddr(&tag, &target);
    ResetGraph(mode);
    assert(pc_ot_getaddr(&tag) == &target);
    assert(_qin == 0 && _qout == 0);
    assert(callbacks == full);
    assert(bios_prints == full);
    assert(backend_calls == (backend && full ? 6 : 0));
    for (i = 0; i < 128; i++) {
        u8 expected;
        expected = i >= 16 ? 255 : full ? 0 : 0xa5;
        if (full && i == 1) expected = 1;
        if (full && i == 5) expected = 4;
        if (full && i == 7) expected = 2;
        assert(sdk_work[i] == expected);
    }
    for (i = 128; i < 192; i++) assert(sdk_work[i] == 0xa5);
    for (i = 192; i < 448; i++) assert(sdk_work[i] == (full ? 0 : 0xa5));
    for (i = 448; i < 0x19c0; i++)
        assert(sdk_work[i] == ((i-448)%96 < 4 ? 0 : 0xa5));
    for (i = 0x19c0; i < sizeof(sdk_work); i++) assert(sdk_work[i] == 0xa5);
    for (y = 0; y < 512; y++)
        for (x = 0; x < 1024; x++) assert(g_vram[y][x] == 0x1212);
    if (full) {
        assert(g_disp_x == 0 && g_disp_y == 0 && g_disp_w == 256);
        assert(g_disp_h == 240 && g_disp_rgb24 == 0 && g_disp_mask == 0);
        assert(g_draw_x == 0 && g_draw_y == 0 &&
               g_draw_w == 1 && g_draw_h == 1);
        assert(g_tpage_x == 0 && g_tpage_y == 0 &&
               g_tpage_bpp == 0 && g_tpage_abr == 0);
        assert(g_mask_set_or == 0 && g_mask_eval_and == 0 && !g_dither_enabled);
        assert(!g_tex_window_mask_x && !g_tex_window_mask_y);
        assert(!g_tex_window_off_x && !g_tex_window_off_y);
    } else {
        assert(g_disp_x == 8 && g_disp_y == 9 && g_disp_w == 640);
        assert(g_disp_h == 480 && g_disp_rgb24 == 1 && g_disp_mask == 1);
        assert(g_draw_x == 33 && g_draw_y == 44 &&
               g_draw_w == 320 && g_draw_h == 240);
        assert(g_tpage_x == 14 && g_tpage_y == 1 &&
               g_tpage_bpp == 2 && g_tpage_abr == 3);
        assert(g_mask_set_or == 0x8000 && g_mask_eval_and == 0x8000 &&
               g_dither_enabled);
        assert(g_tex_window_mask_x == 1 && g_tex_window_mask_y == 2);
        assert(g_tex_window_off_x == 3 && g_tex_window_off_y == 4);
    }
    return 0;
}
