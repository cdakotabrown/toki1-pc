/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Completion observations through actual image and OT adapters. */
#define main gpu_ot_fixture_main
#include "gpu_ot_harness.c"
#undef main
#include <assert.h>

static const char *operation;
static int notifications;
static u32 input = 0x03e0001f, output = 0xcafecafe;

void nbn_trace_load_image(const void *data, u32 bytes,
                         s32 x, s32 y, s32 w, s32 h) {
    (void)data; (void)bytes; (void)x; (void)y; (void)w; (void)h;
}
void nbn_trace_move_image(s32 x, s32 y, s32 w, s32 h, s32 dx, s32 dy) {
    (void)x; (void)y; (void)w; (void)h; (void)dx; (void)dy;
}

static void complete(void) {
    u32 activity;
    notifications++;
    memcpy(&activity, graph_env + 8, 4);
    assert(activity == 0);
    if (!strcmp(operation, "load")) {
        assert(g_vram[0][0] == 31 && g_vram[0][1] == 992);
    } else if (!strcmp(operation, "store")) {
        assert(output == input);
    } else if (!strcmp(operation, "move")) {
        assert(g_vram[5][4] == 31 && g_vram[5][5] == 992);
    } else {
        assert(g_vram[11][10] == 31 && g_vram[11][11] == 31);
    }
}

int main(int argc, char **argv) {
    RECT rect;
    u32 tile[4] = { 0x03ffffff, 0x600000f8, 0x000b000a, 0x00010002 };
    (void)argc;
    operation = argv[1];
    g_use_mdfn_gpu = 0;
    gpu_queue_reset(1);
    graph_env[1] = 1;
    g_draw_w = 320; g_draw_h = 240;
    rect.x = rect.y = 0; rect.w = 2; rect.h = 1;
    assert(DrawSyncCallback(complete) == 0);
    if (!strcmp(operation, "load")) {
        LoadImage(&rect, &input);
    } else if (!strcmp(operation, "store")) {
        g_vram[0][0] = 31; g_vram[0][1] = 992;
        StoreImage(&rect, &output);
    } else if (!strcmp(operation, "move")) {
        g_vram[0][0] = 31; g_vram[0][1] = 992;
        MoveImage(&rect, 4, 5);
    } else {
        DrawOTag(tile);
    }
    assert(notifications == 1);
    assert(DrawSyncCallback(0) == complete);
    return 0;
}
