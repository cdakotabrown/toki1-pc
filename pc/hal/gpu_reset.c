/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ResetGraph's PS1 RAM effects, using the retail byte layout.
 * Device reset is handled by the HAL, not by calling a 32-bit SDK
 * function table through a native pointer-shaped struct.
 */
#include "gpu_reset.h"
#include "gpu_queue.h"
#include "hal.h"
#include "bios_printf.h"
#include "cpu_cost.h"
#include "profile.h"
#include <string.h>
#include <stdlib.h>

extern u8 graph_env[128];
extern u8 libgpu_resetgraph_jtb_fmt, libgpu_default_jtb_tbl;
extern u8 libgpu_gp1_shadow_tbl[256];
extern u8 _que[64 * 96];
extern s32 _qin, _qout;
extern void ResetCallback(void);
extern void hal_cpu_spend(u32 ticks);
extern void hal_vblank_display_end(int y2) __attribute__((weak));

void gpu_cache_draw_env(const void *env)
{
    memcpy(graph_env + 16, env, 92);
}

/* The matched PutDispEnv uses a PS1 table with ctl at byte 16. Give
 * that one consumer a native table rather than overlapping 64-bit
 * pointers in the general legacy graph_jtb adapter. */
typedef struct {
    u8 pad[16];
    void (*ctl)(u32);
} GpuDisplayFuncs;

void gpu_control_shadow(u32 command)
{
    /* _ctl retains only the low payload byte in its 256-byte shadow.
     * Direct device state is applied by the HAL PutDispEnv wrapper. */
    libgpu_gp1_shadow_tbl[command >> 24] = (u8)command;
}

/* PutDispEnv's CPU work, charged where it happens: before each display
 * control write the matched body issues, and after the last. Measured on
 * the reference emulator (Interpreter, warm, a 2,400-frame no-input boot),
 * ticks from PutDispEnv's entry:
 *   vblank_function (1,171 calls)  05h @98  06h @401  07h @480  08h @718
 *                                  return @942
 *   boot movie (153 calls)         05h @98  08h @425  return @652
 * so 98 before 05h, 303 between 05h and 06h, 79 to 07h, 238 from 07h to
 * 08h, 327 from 05h straight to 08h (the range writes skipped) and 224
 * to return. Orders not observed there are charged nothing. */
#define PUTDISP_TO_RETURN 224u
static u32 put_disp_previous;
void *gpu_put_disp_caller;   /* PutDispEnv's return address (pc/hal/gpu.c) */

static u32 put_disp_ticks(u32 previous, u32 command)
{
    switch (command) {
    case 0x05: return previous == 0 ? 98u : 0u;
    case 0x06: return previous == 0x05 ? 303u : 0u;
    case 0x07: return previous == 0x06 ? 79u : 0u;
    case 0x08: return previous == 0x07 ? 238u : previous == 0x05 ? 327u : 0u;
    default: return 0u;
    }
}

/* The gaps include the matched body's GetVideoMode calls, which charge
 * their own rows (pc/hal/cpu_cost.c hal_cpu_cost_net). */
static unsigned long long put_disp_mark;

static void put_disp_control(u32 command)
{
    hal_cpu_spend(hal_cost_net(put_disp_ticks(put_disp_previous, command >> 24),
                                   put_disp_mark));
    put_disp_mark = hal_cost_mark();
    put_disp_previous = command >> 24;
    /* GP1(07h): the vertical display range's end decides where the
     * VBlank interrupt falls (pc/hal/cd.c hal_vblank_display_end). */
    if ((command >> 24) == 0x07 && hal_vblank_display_end) hal_vblank_display_end((int)((command >> 10) & 0x3FFu));
    gpu_control_shadow(command);
}

static GpuDisplayFuncs display_funcs = { { 0 }, put_disp_control };
GpuDisplayFuncs *gpu_sdk_display_jtb = &display_funcs;

extern DISPENV *gpu_sdk_put_disp_env(DISPENV *env);

void gpu_apply_disp_env(void *env)
{
    /* Reuse retail conditional range/mode writes and the final cache copy.
     * The separate build renames only the entry and its dispatch pointer. */
    put_disp_previous = 0;
    put_disp_mark = hal_cost_mark();
    gpu_sdk_put_disp_env((DISPENV *)env);
    /* 224 is vblank_function's tail; at the boot, the caller's measured one
     * (cpu_cost.c hal_cpu_cost_tail, CALL_CONTINUATION). */
    hal_cpu_spend(hal_cost_net(hal_cpu_cost_tail && gpu_put_disp_caller
                               ? hal_cpu_cost_tail("PutDispEnv@tail", gpu_put_disp_caller,
                                                   PUTDISP_TO_RETURN)
                               : PUTDISP_TO_RETURN, put_disp_mark));
    gpu_put_disp_caller = NULL;
}

/* ResetGraph(0)'s CPU work besides its printf (pc/hal/bios_printf.c
 * charges that), measured on the reference emulator (Interpreter, SCPH-5500)
 * at the boot's three ResetGraph(0) calls (system_init, gpu_init,
 * system_reset): entry to printf 69 ticks (all three); printf's return
 * to _reset's SetIntrMask(0) 2,029 / 2,393 / 2,400 (memset of graph_env,
 * ResetCallback, set_jtb, the BIOS GPU_cw: 1,237 / 1,601 / 1,608 of it);
 * masked 1,542 (all three), the GP1(00h) reset write ~220 into it, before
 * _reset's memset of its 256-byte shadow; then 786 / 834 / 804 to the
 * caller's next call.  The medians are charged; the matched functions in
 * the first span (ResetCallback, startIntr) charge their own rows, which
 * are part of it (hal_cost_net).  The port charged the printf only: each
 * reset was ~4,800 ticks short.  RESETGRAPH0_CPU=0 is the negative
 * control. */
#define RESETGRAPH0_TO_PRINTF   69u
#define RESETGRAPH0_TO_MASK     2393u
#define RESETGRAPH0_TO_GP1      220u
#define RESETGRAPH0_MASKED      1542u
#define RESETGRAPH0_TO_RETURN   804u
extern s32 SetIntrMask(s32 mask);
static int resetgraph0_cpu_on(void)
{
    static int on = -1;
    if (on < 0) {
        on = hal_model_on("RESETGRAPH0_CPU");
    }
    return on;
}

void gpu_reset_graph(s32 mode)
{
    int i, timed = 0;
    s32 saved = 0;
    unsigned long long mark = 0;
    if ((mode & 3) == 0) {
        timed = resetgraph0_cpu_on();
        if (timed) hal_cpu_spend(RESETGRAPH0_TO_PRINTF);
        func_800B4850((const char *)&libgpu_resetgraph_jtb_fmt,
                      (u32)(unsigned long)&libgpu_default_jtb_tbl,
                      (u32)(unsigned long)graph_env);
        mark = hal_cost_mark();
        memset(graph_env, 0, 128);
        graph_env[1] = 1;
        ResetCallback();
        if (timed) {
            hal_cpu_spend(hal_cost_net(RESETGRAPH0_TO_MASK, mark));
            saved = SetIntrMask(0);
            hal_cpu_spend(RESETGRAPH0_TO_GP1);
        }
    }
    gpu_queue_reset((mode & 3) == 0);
    _qout = 0;
    _qin = 0;
    /* Only the function word is cleared; the ring's argument bytes remain. */
    for (i = 0; i < 64; i++) memset(_que + i * 96, 0, 4);
    if ((mode & 3) == 0) {
        gpu_reset_device();
        /* GP1(00h) resets the vertical display range to 0x3FC10 (Y2 255) */
        if (hal_vblank_display_end) hal_vblank_display_end(255);
        memset(libgpu_gp1_shadow_tbl, 0, 256);
        /* The native device implements the retail fixture's GPU type 0:
         * little-endian signed-short limits 1024 x 512, not wide VRAM. */
        graph_env[0] = 0;
        graph_env[4] = 0;
        graph_env[5] = 4;
        graph_env[6] = 0;
        graph_env[7] = 2;
        if (timed) {
            hal_cpu_spend(RESETGRAPH0_MASKED - RESETGRAPH0_TO_GP1);
            SetIntrMask(saved);
            hal_cpu_spend(RESETGRAPH0_TO_RETURN);
        }
    }
    /* Invalidate the cached DRAWENV (92 bytes) and DISPENV (20 bytes).
     * Soft reset preserves device drawing/display state and VRAM. */
    memset(graph_env + 16, 0xFF, 92);
    memset(graph_env + 108, 0xFF, 20);
}
