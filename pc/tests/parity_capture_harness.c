/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Standalone deterministic capture fixture; no SDL/assets required. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "parity.h"
#include "gpu_display.h"

u16 g_vram[512][1024];
s32 g_disp_x = 32, g_disp_y = 256, g_disp_w = 320, g_disp_h = 240;
s32 frame_counter, step_frame_cnt, vblnk_counter, activeBuff;
s32 pad_data, pad_trig, pad_rel;
u8 now_step = 15, now_sub_step = 2, now_sub_sub_step = 3, now_girl = 4, step_mode;
static u32 ticks;
static unsigned long mixed;
void gpu_get_scanout_state(GpuScanoutState *state) {
    state->x = g_disp_x;
    state->y = g_disp_y;
    state->width = g_disp_w;
    state->height = g_disp_h;
    state->rgb24 = 0;
    state->enabled = 1;
}
u32 hal_clock_vblanks(void) { return ticks; }
u32 hal_clock_ticks_per_frame(void) { return 566204u; }
void spu_parity_mix(s16 *samples, int frames) {
    int i;
    for (i = 0; i < frames; i++) {
        samples[i * 2] = (s16)((mixed + (unsigned long)i) & 32767);
        samples[i * 2 + 1] = (s16)-samples[i * 2];
    }
    mixed += (unsigned long)frames;
}
static void advance(int count) {
    int i;
    for (i = 0; i < count; i++) {
        ticks++;
        frame_counter = 1000 + (s32)ticks;
        vblnk_counter = (s32)ticks;
        parity_vblank();
    }
}
int main(int argc, char **argv) {
    int count;
    count = argc > 1 ? atoi(argv[1]) : 4;
    parity_init();
    g_vram[256][32] = 31;
    g_vram[256][33] = 31 << 5;
    g_vram[256][34] = 31 << 10;
    advance(3); /* Boot advances synthesis, but produces no artifacts. */
    if (getenv("NO_ANCHOR")) return 0;
    parity_anchor();
    if (getenv("RESET_ANCHOR")) {
        advance(1);
        parity_anchor();
    }
    if (getenv("SMALL_VIEWPORT")) { g_disp_w = 16; g_disp_h = 8; }
    advance(count);
    /* Reproduce trace's destructor-bypassing exit. */
    _exit(0);
}
