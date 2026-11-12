/* SPDX-License-Identifier: GPL-3.0-or-later */
/* quick_text_harness.c -- [enhanced] quick_text (pc/hal/quick_text.c)
 * against a synthetic text engine and the controller-port model
 * (pc/hal/pad_device.c): no game data.  pc/tools/test_quick_text.py
 * builds it and runs every case; each prints "ok NAME" or "FAIL NAME: why".
 *
 *   quick_text_harness            every case (TOKI_QUICK_TEXT=1 in the env)
 *   quick_text_harness off        the feature off: nothing changes
 */
#include "types.h"
#include "pad_device.h"
#include "pc_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the game's globals (PS1 RAM in the game) ---- */
s16 check_end_k_tbl0, check_end_k_tbl1, disp_string_state;
s32 serifu_state, msg_wait, serifu_wait, serifu_voice, xa_m_mode;
u8 voice_level, cd_state, k_no, k_mode, mouse_btn_mode;
u8 now_step, now_sub_step, now_sub_sub_step, step_mode;
typedef struct { s16 x, y; u8 tp, count, pal; s8 flag; } Rec;
Rec disp_string_tbl0[52];
u8 pad1_buf[8], pad2_buf[8];

u32 quick_text_buttons(u32 player);
void quick_text_frame_end(void);
unsigned quick_text_completed(void);
unsigned quick_text_cuts(void);

static int g_fail;
#define CHECK(name, cond) do { if (!(cond)) { printf("FAIL %s: %s (line %d)\n", name, #cond, __LINE__); g_fail = 1; return; } } while (0)

static void reset(void)
{
    check_end_k_tbl0 = check_end_k_tbl1 = disp_string_state = 0;
    serifu_state = msg_wait = serifu_wait = serifu_voice = 0;
    xa_m_mode = 2;
    voice_level = 2;
    cd_state = 2;
    k_no = k_mode = mouse_btn_mode = 0;
    now_step = 0x50;
    now_sub_step = now_sub_sub_step = step_mode = 0;
    memset(disp_string_tbl0, 0, sizeof disp_string_tbl0);
    quick_text_buttons(0);                  /* every button up */
    quick_text_frame_end();
}

/* a serifu_main page: name, bracket, three lines (glyph counts) */
static void serifu_page(int shown, int voice)
{
    static const Rec r[] = { { -120, 50, 0, 3, 0, 1 }, { -86, 50, 0, 1, 0, 1 },
                             { -72, 50, 0, 10, 0, 1 }, { -72, 66, 0, 10, 0, 1 },
                             { -72, 82, 0, 6, 0, 1 } };
    memcpy(disp_string_tbl0, r, sizeof r);
    disp_string_state = 5;
    check_end_k_tbl0 = 30;
    check_end_k_tbl1 = (s16)shown;
    serifu_state = 1;
    serifu_voice = voice;
    cd_state = voice ? 4 : 2;
}

/* one VBlank poll then the frame's end, as the port runs them */
static u32 frame(u32 player)
{
    u32 b = quick_text_buttons(player);
    quick_text_frame_end();
    return b;
}

static void case_print_completes(void)
{
    const char *n = "print_completes";
    u32 b;
    reset();
    serifu_page(8, 1);
    b = quick_text_buttons(PC_PAD_CIRCLE);
    CHECK(n, !(b & PC_PAD_CIRCLE));                  /* held back from the game */
    CHECK(n, check_end_k_tbl1 == 8);                 /* not written at the poll ... */
    quick_text_frame_end();
    CHECK(n, check_end_k_tbl1 == 30);                /* ... but at the frame boundary */
    CHECK(n, !(frame(PC_PAD_CIRCLE) & PC_PAD_CIRCLE));   /* still held: still held back */
    CHECK(n, frame(0) == 0);                         /* released */
    /* the next press, voice still playing: the game's cross */
    b = frame(PC_PAD_CIRCLE);
    CHECK(n, b == PC_PAD_CROSS);
    CHECK(n, frame(PC_PAD_CIRCLE) == PC_PAD_CROSS);  /* for as long as it is held */
    CHECK(n, frame(0) == 0);
    CHECK(n, quick_text_completed() >= 1 && quick_text_cuts() >= 1);
    printf("ok %s\n", n);
}

static void case_after_voice_passes(void)
{
    const char *n = "after_voice_passes";
    reset();
    serifu_page(30, 1);
    cd_state = 2;                                    /* the line ended: the arrow's wait */
    CHECK(n, frame(PC_PAD_CIRCLE) == PC_PAD_CIRCLE);
    frame(0);
    serifu_page(30, 0);                              /* a page without a voice */
    CHECK(n, frame(PC_PAD_CIRCLE) == PC_PAD_CIRCLE);
    printf("ok %s\n", n);
}

static void case_voice_level(void)
{
    const char *n = "voice_level";
    reset();
    serifu_page(30, 1);
    voice_level = 1;                                 /* "always on": the game has no cut */
    CHECK(n, frame(PC_PAD_CIRCLE) == PC_PAD_CIRCLE);
    frame(0);
    voice_level = 0;
    CHECK(n, frame(PC_PAD_CIRCLE) == PC_PAD_CIRCLE);
    printf("ok %s\n", n);
}

static void case_not_dialogue(void)
{
    const char *n = "not_dialogue";
    reset();
    serifu_page(8, 1);
    serifu_state = 0;                                /* a menu printing its labels */
    CHECK(n, frame(PC_PAD_CIRCLE) == PC_PAD_CIRCLE);
    CHECK(n, check_end_k_tbl1 == 8);
    frame(0);
    now_step = 0x45;                                 /* the TEL screen, no speech layout */
    disp_string_tbl0[1].x = -40;
    CHECK(n, frame(PC_PAD_CIRCLE) == PC_PAD_CIRCLE);
    CHECK(n, check_end_k_tbl1 == 8);
    printf("ok %s\n", n);
}

static void case_held_from_before(void)
{
    const char *n = "held_from_before";
    reset();
    frame(PC_PAD_CIRCLE);                            /* held before the page */
    serifu_page(8, 1);
    CHECK(n, frame(PC_PAD_CIRCLE) == PC_PAD_CIRCLE); /* no new press: the game's own hold */
    CHECK(n, check_end_k_tbl1 == 8);
    printf("ok %s\n", n);
}

static void case_tel_scroll(void)
{
    const char *n = "tel_scroll";
    reset();
    serifu_page(8, 0);
    serifu_state = 0;
    now_step = 0x45;                                 /* TEL: speech layout on the page */
    disp_string_tbl0[5] = disp_string_tbl0[4];       /* a fourth and fifth line */
    disp_string_tbl0[6] = disp_string_tbl0[4];
    disp_string_state = 7;
    check_end_k_tbl0 = 42;
    k_mode = 0x80;                                   /* scrolling, top line = record 2 */
    k_no = 2;
    frame(PC_PAD_CIRCLE);
    CHECK(n, check_end_k_tbl1 == 30);                /* records 0..4: the visible window */
    frame(0);
    k_mode = 0;                                      /* not scrolling: the page's end */
    frame(PC_PAD_CIRCLE);
    CHECK(n, check_end_k_tbl1 == 42);
    frame(0);
    xa_m_mode = 1;                                   /* the TEL voice still plays */
    CHECK(n, frame(PC_PAD_CIRCLE) == PC_PAD_CROSS);
    frame(0);
    xa_m_mode = 2;
    CHECK(n, frame(PC_PAD_CIRCLE) == PC_PAD_CIRCLE);
    printf("ok %s\n", n);
}

static void case_page_changed(void)
{
    const char *n = "page_changed";
    reset();
    serifu_page(8, 1);
    quick_text_buttons(PC_PAD_CIRCLE);
    check_end_k_tbl0 = 12;                           /* a new page before the frame's end */
    check_end_k_tbl1 = 5;
    quick_text_frame_end();
    CHECK(n, check_end_k_tbl1 == 5);
    frame(0);
    printf("ok %s\n", n);
}

/* the mouse: the button the game reads as circle is held back / sent as
 * the other one, through the port model's record byte 3 (active low:
 * 0x08 left, 0x04 right) */
static void case_mouse(void)
{
    const char *n = "mouse";
    pad_device_config cfg = { 1.0, 0, PADDEV_MOUSE, 1 };
    pad_device_init(&cfg);
    reset();
    serifu_page(8, 1);
    pad_device_mouse_button(0, 1);                   /* left down */
    quick_text_buttons(0);
    pad_device_tick();
    CHECK(n, pad1_buf[1] == 0x12 && (pad1_buf[3] & 0x08));   /* not reported */
    quick_text_frame_end();
    CHECK(n, check_end_k_tbl1 == 30);
    pad_device_mouse_button(0, 0);
    quick_text_buttons(0);
    pad_device_tick();
    CHECK(n, (pad1_buf[3] & 0x0C) == 0x0C);
    pad_device_mouse_button(0, 1);                   /* voice playing: sent as the right */
    quick_text_buttons(0);
    pad_device_tick();
    CHECK(n, (pad1_buf[3] & 0x08) && !(pad1_buf[3] & 0x04));
    pad_device_mouse_button(0, 0);
    quick_text_buttons(0);
    pad_device_tick();
    CHECK(n, (pad1_buf[3] & 0x0C) == 0x0C);
    /* swapped buttons (mouse_btn_mode): the right one is circle */
    mouse_btn_mode = 1;
    serifu_page(8, 1);
    pad_device_mouse_button(1, 1);
    quick_text_buttons(0);
    pad_device_tick();
    CHECK(n, pad1_buf[3] & 0x04);
    quick_text_frame_end();
    CHECK(n, check_end_k_tbl1 == 30);
    pad_device_mouse_button(1, 0);
    quick_text_buttons(0);
    printf("ok %s\n", n);
}

/* TOKI_QUICK_TEXT=0: nothing held back, translated or written */
static void case_off(void)
{
    const char *n = "off";
    reset();
    serifu_page(8, 1);
    CHECK(n, frame(PC_PAD_CIRCLE) == PC_PAD_CIRCLE);
    CHECK(n, check_end_k_tbl1 == 8);
    frame(0);
    serifu_page(30, 1);
    CHECK(n, frame(PC_PAD_CIRCLE) == PC_PAD_CIRCLE);
    CHECK(n, quick_text_completed() == 0 && quick_text_cuts() == 0);
    printf("ok %s\n", n);
}

int main(int argc, char **argv)
{
    pad_device_init(NULL);
    if (argc > 1 && !strcmp(argv[1], "off")) {
        case_off();
        return g_fail;
    }
    case_print_completes();
    case_after_voice_passes();
    case_voice_level();
    case_not_dialogue();
    case_held_from_before();
    case_tel_scroll();
    case_page_changed();
    case_mouse();
    return g_fail;
}
