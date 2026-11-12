/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc/tests/test_pad_mouse.c -- the PS1 mouse emulation, end to end and
 * headless: pc/hal/pad_device.c writes the libpad records, then the MATCHED
 * game code runs on them exactly as it does in the game loop
 * (pad_update_buffers(); pad_main();):
 *
 *   pad_main        0x80042C80  calls the detector, picks mouse or pad path
 *   pad_type_check  0x80042D48  device detection + the init arms
 *   mouse_pad_set   0x80042F20  cursor, clamps, click bits, edge words
 *   pad_get         0x80043280  the digital pad's packed buttons
 *   InitMouse / SenseMouse / SetMouse / RangeMouse / MouseState /
 *   MouseRead / mouse_pos_clamp -- the libetc helper family
 *
 * Only the BIOS pad calls (InitPAD / StartPAD / ChangeClearPAD) and the
 * pad path's pad_scan are stubbed; everything else is the real code over
 * the retail-offset globals of pc/src/input_globals.c.
 */
#include <stdio.h>
#include <string.h>
#include "types.h"
#include "pad_device.h"

/* ---- game code under test ---- */
extern void pad_main(void);
extern s32  MouseState(s32 idx, s8 *out);
extern void MouseRead(s32 idx, s32 *out);

/* ---- retail-offset globals (pc/src/input_globals.c) ---- */
extern u8  ps1_input_state[0x2C];
extern u8  ps1_pad1_rec[8];
extern u8  ps1_pad2_rec[8];
extern u8  pad_type;            /* 0x800EF6D4 */
extern u8  csr_speed;           /* 0x800EF6CF */
extern u8  click_mode;          /* 0x800EF6D0 */
extern u8  click_speed;         /* 0x800EF6D3 */
extern u8  mouse_btn_mode;      /* 0x800EF6D5 */
extern s16 mouse_x;          /* mouse_pad_st.x */
extern s16 mouse_y;          /* mouse_pad_st.y */
extern u32 pad_data;            /* 0x800EF6E0 */
extern u32 pad_prev;
extern u32 pad_trig;
extern u32 pad_rel;
extern u32 mouse_rec_tbl[];
extern s32 mouse_x_acc[], mouse_y_acc[];
extern s32 mouse_x_min, mouse_x_max, mouse_y_min, mouse_y_max;
extern u32 pad_get(void);

/* ---- the rest of the game state these functions touch ---- */
u8  menu_mode = 1;              /* 0x800EF7F2 */
u8  card_busy = 0;              /* 0x800EE795 */
u8  pad_main_tbl0 = 0;          /* 0x800BA190 */
s16 cursor_cw_x = 0;            /* 0x801271D6 */
s16 cursor_cw_y = 0;            /* 0x801271DA */
s16 libapi_mouse_sense_x = 0;             /* SenseMouse X factor */
s16 libapi_mouse_sense_y = 0;             /* SenseMouse Y factor */

/* ---- BIOS / pad-path stubs ---- */
static int n_initpad, n_startpad, n_clearpad, n_padscan;
void InitPAD(void *a, s32 b, void *c, s32 d) { (void)a; (void)b; (void)c; (void)d; n_initpad++; }
void StartPAD(void) { n_startpad++; }
void ChangeClearPAD(s32 v) { (void)v; n_clearpad++; }
void pad_scan(void) { n_padscan++; }

#define CHECK(cond) do { if (!(cond)) { \
    printf("\n    FAIL %s:%d: %s", __FILE__, __LINE__, #cond); return __LINE__; } } while (0)

/* One game tick, exactly as pc/src/main.c orders it. */
static void tick(void) { pad_device_tick(); pad_main(); }

/* The state game_system_init (pad_type = 1, csr_speed = 3, menu_mode = 1)
 * and game_work_init2 (click_mode = 0, click_speed = 32, swap = 0) leave
 * behind, with a configurable port model. */
static void boot(const pad_device_config *cfg) {
    memset(ps1_input_state, 0, sizeof(ps1_input_state));
    pad_type = 1; csr_speed = 3; click_mode = 0; click_speed = 32; mouse_btn_mode = 0;
    menu_mode = 1; card_busy = 0; pad_main_tbl0 = 0;
    cursor_cw_x = cursor_cw_y = 0;
    libapi_mouse_sense_x = libapi_mouse_sense_y = 0;
    mouse_rec_tbl[0] = mouse_rec_tbl[1] = 0;
    mouse_x_acc[0] = mouse_y_acc[0] = mouse_x_acc[1] = mouse_y_acc[1] = 0;
    n_initpad = n_startpad = n_clearpad = n_padscan = 0;
    pad_device_init(cfg);
}

static const pad_device_config CFG_DEFAULT = { 1.0, 1, PADDEV_AUTO, 1 };

/* Bring port 1 to a detected mouse: motion, the empty-port poll, the id
 * poll (init arm), then one quiet poll. */
static int go_mouse(void) {
    pad_device_mouse_motion(5, -3);
    tick();                                   /* unplugged: 0xFF */
    CHECK(pad_device_presented() == PADDEV_NONE);
    CHECK(pad_type == 0xFF);
    tick();                                   /* mouse answers: init arm */
    CHECK(pad_device_presented() == PADDEV_MOUSE);
    CHECK(pad_type == 0);
    tick();                                   /* first normal poll */
    return 0;
}

/* 1. Nothing touched: port 1 is a digital pad and the detector is quiet. */
int test_pad_boot_as_pad(void) {
    boot(&CFG_DEFAULT);
    tick();
    CHECK(pad_device_presented() == PADDEV_PAD);
    CHECK(ps1_pad1_rec[0] == 0x00 && ps1_pad1_rec[1] == 0x41);
    CHECK(ps1_pad1_rec[2] == 0xFF && ps1_pad1_rec[3] == 0xFF);
    CHECK(ps1_pad2_rec[0] == 0xFF);           /* port 2 empty */
    CHECK(pad_type == 1);
    CHECK(n_initpad == 0 && n_padscan == 1);  /* no transition; pad path ran */
    /* pad_get packs the pad's bytes: UP + CROSS */
    pad_device_pad_buttons(0x1000 | 0x0040);
    tick();
    CHECK(ps1_pad1_rec[2] == 0xEF && ps1_pad1_rec[3] == 0xBF);
    CHECK(pad_get() == 0x1040);
    return 0;
}

/* 2. Mouse motion: unplug poll, mouse init arm, then the cursor moves by
 *    delta * speed / 3 (speed 3 => 1:1) and mirrors into cursor_cw. */
int test_pad_mouse_detect_and_move(void) {
    int r;
    boot(&CFG_DEFAULT);
    tick();
    r = go_mouse(); if (r) return r;
    /* the init arm (pad_type_check 0x80042D48, newState == 0) */
    CHECK(n_initpad == 1 && n_startpad == 1 && n_clearpad == 1);
    CHECK(menu_mode == 0);                    /* pointer hit-testing mode */
    CHECK(mouse_rec_tbl[0] == (u32)(unsigned long)ps1_pad1_rec
          && mouse_rec_tbl[1] == (u32)(unsigned long)ps1_pad2_rec);
    CHECK(libapi_mouse_sense_x == 3 && libapi_mouse_sense_y == 4);               /* SenseMouse(3, 4) */
    CHECK(mouse_x_acc[0] == 160 * 3 && mouse_y_acc[0] == 120 * 4);   /* SetMouse(0,160,120) */
    CHECK(mouse_x_min == 0 && mouse_x_max == 320 * 3);      /* RangeMouse(0,320,0,240) */
    CHECK(mouse_y_min == 0 && mouse_y_max == 240 * 4);
    /* the motion that triggered the swap arrived on the transition frame
     * and was ignored, as the console would: cursor untouched */
    CHECK(cursor_cw_x == 0 && cursor_cw_y == 0);
    CHECK(n_padscan == 1);                    /* pad path only on the first tick */
    pad_device_mouse_motion(10, -6);
    tick();
    CHECK(ps1_pad1_rec[4] == 10 && (s8)ps1_pad1_rec[5] == -6);
    CHECK(mouse_x == 10 && mouse_y == -6);
    CHECK(cursor_cw_x == 10 && cursor_cw_y == -6);
    tick();                                   /* no motion: stays put */
    CHECK(ps1_pad1_rec[4] == 0 && cursor_cw_x == 10 && cursor_cw_y == -6);
    /* speed option scales: csr_speed 6 => delta * 2 */
    csr_speed = 6;
    pad_device_mouse_motion(4, 4);
    tick();
    CHECK(cursor_cw_x == 18 && cursor_cw_y == 2);
    return 0;
}

/* 3. The game's clamps (x in [-150,140], y in [-100,100]) and the HAL's
 *    s8 clamp with remainder carry. */
int test_pad_mouse_clamp_and_carry(void) {
    int r;
    pad_device_config cfg = CFG_DEFAULT;
    boot(&cfg);
    tick();
    r = go_mouse(); if (r) return r;
    pad_device_mouse_motion(300, 0);          /* one flick: 127 + 127 + 46 */
    tick(); CHECK(ps1_pad1_rec[4] == 127 && cursor_cw_x == 127);
    tick(); CHECK(ps1_pad1_rec[4] == 127 && cursor_cw_x == 140);   /* clamped */
    tick(); CHECK(ps1_pad1_rec[4] == 46  && cursor_cw_x == 140);
    tick(); CHECK(ps1_pad1_rec[4] == 0);
    pad_device_mouse_motion(-400, 0);         /* -128, -128, -128, -16 */
    tick(); CHECK((s8)ps1_pad1_rec[4] == -128 && cursor_cw_x == 12);
    tick(); CHECK(cursor_cw_x == -116);
    tick(); CHECK(cursor_cw_x == -150);       /* clamped */
    tick(); CHECK((s8)ps1_pad1_rec[4] == -16 && cursor_cw_x == -150);
    pad_device_mouse_motion(0, 127);
    tick(); CHECK(cursor_cw_y == 100);        /* 127 -> 100 */
    pad_device_mouse_motion(0, -128);
    tick(); CHECK(cursor_cw_y == -28);
    pad_device_mouse_motion(0, -128);
    tick(); CHECK(cursor_cw_y == -100);
    /* fractional carry at sensitivity 0.5.  go_mouse()'s 5 px became 2.5
     * counts: 2 reported, 0.5 carried.  Then 3 px -> 2.0 -> 2; 3 px -> 1.5
     * -> 1 (+0.5); 1 px -> 1.0 -> 1; 1 px -> 0.5 -> 0 (+0.5). */
    cfg.sensitivity = 0.5;
    boot(&cfg); tick(); r = go_mouse(); if (r) return r;
    pad_device_mouse_motion(3, 0); tick(); CHECK(ps1_pad1_rec[4] == 2);
    pad_device_mouse_motion(3, 0); tick(); CHECK(ps1_pad1_rec[4] == 1);
    pad_device_mouse_motion(1, 0); tick(); CHECK(ps1_pad1_rec[4] == 1);
    pad_device_mouse_motion(1, 0); tick(); CHECK(ps1_pad1_rec[4] == 0);
    pad_device_mouse_motion(1, 0); tick(); CHECK(ps1_pad1_rec[4] == 1);
    return 0;
}

/* 4. Buttons through mouse_pad_set: left = 0x20, right = 0x40, edge words,
 *    the else-if precedence and the swap option. */
int test_pad_mouse_buttons(void) {
    int r;
    boot(&CFG_DEFAULT);
    tick();
    r = go_mouse(); if (r) return r;
    pad_device_mouse_button(0, 1);            /* left down */
    tick();
    CHECK(ps1_pad1_rec[3] == 0xF7);
    CHECK(pad_data == 0x20 && pad_trig == 0x20 && pad_rel == 0);
    tick();
    CHECK(pad_data == 0x20 && pad_trig == 0 && pad_prev == 0x20);
    pad_device_mouse_button(0, 0);            /* left up */
    tick();
    CHECK(pad_data == 0 && pad_trig == 0 && pad_rel == 0x20);
    pad_device_mouse_button(1, 1);            /* right down */
    tick();
    CHECK(ps1_pad1_rec[3] == 0xFB && pad_trig == 0x40);
    pad_device_mouse_button(0, 1);            /* both: left wins the else-if */
    tick();
    CHECK(ps1_pad1_rec[3] == 0xF3 && pad_data == 0x20);
    pad_device_mouse_button(0, 0); pad_device_mouse_button(1, 0);
    tick(); tick();
    CHECK(pad_data == 0);
    mouse_btn_mode = 1;                       /* OPTION "mouse button" swap */
    pad_device_mouse_button(0, 1);
    tick();
    CHECK(pad_data == 0x40);
    pad_device_mouse_button(0, 0);
    pad_device_mouse_button(1, 1);
    tick();
    CHECK(pad_data == 0x20);
    pad_device_mouse_button(1, 0);
    /* focus loss drops a held button */
    pad_device_mouse_button(0, 1); tick(); CHECK(pad_data != 0);
    pad_device_release(); tick(); CHECK(pad_data == 0);
    return 0;
}

/* 5. Hot-switching back to the pad: the card_busy arm keeps the old mode
 *    across the empty poll; without it the detector goes through 0xFF. */
int test_pad_mouse_switch_and_card_busy(void) {
    int r;
    pad_device_config cfg = CFG_DEFAULT;
    boot(&cfg);
    tick();
    r = go_mouse(); if (r) return r;
    pad_device_mouse_motion(20, 0); tick();
    CHECK(cursor_cw_x == 20 && n_initpad == 1);
    card_busy = 1;
    pad_device_pad_buttons(0x0040);           /* CROSS: the player picked up the pad */
    tick();                                   /* empty poll */
    CHECK(pad_device_presented() == PADDEV_NONE && ps1_pad1_rec[0] == 0xFF);
    CHECK(pad_type == 0);                     /* card_busy: mode kept, no transition */
    CHECK(n_initpad == 1 && cursor_cw_x == 20);
    pad_device_pad_buttons(0x0040);
    tick();                                   /* pad answers: pad init arm */
    CHECK(pad_device_presented() == PADDEV_PAD && pad_type == 1);
    CHECK(n_initpad == 2 && cursor_cw_x == 0 && pad_data == 0 && pad_trig == 0);
    CHECK(menu_mode == 0);                    /* the pad arm leaves menu_mode alone */
    pad_device_pad_buttons(0x0040);
    tick();
    CHECK(n_padscan == 2);                    /* pad path running again */
    /* back to the mouse with the card idle: the 0xFF state is visible */
    card_busy = 0;
    pad_device_pad_buttons(0);
    pad_device_mouse_button(0, 1);
    tick();
    CHECK(pad_type == 0xFF && n_initpad == 2);
    tick();
    CHECK(pad_type == 0 && n_initpad == 3 && menu_mode == 0);
    pad_device_mouse_button(0, 0);
    /* swap_gap 0: the id changes in place, still a detector transition */
    cfg.swap_gap = 0;
    boot(&cfg); tick();
    pad_device_mouse_motion(1, 1);
    tick();
    CHECK(pad_device_presented() == PADDEV_MOUSE && pad_type == 0 && n_initpad == 1);
    pad_device_pad_buttons(0x1000);
    tick();
    CHECK(pad_device_presented() == PADDEV_PAD && pad_type == 1 && n_initpad == 2);
    return 0;
}

/* 6. Policy knobs: mouse disabled, device pinned. */
int test_pad_mouse_policy(void) {
    pad_device_config cfg = CFG_DEFAULT;
    cfg.mouse_enabled = 0;
    boot(&cfg); tick();
    pad_device_mouse_motion(50, 50); pad_device_mouse_button(0, 1);
    tick(); tick(); tick();
    CHECK(pad_device_presented() == PADDEV_PAD && pad_type == 1 && cursor_cw_x == 0);
    pad_device_mouse_button(0, 0);
    cfg = CFG_DEFAULT; cfg.force = PADDEV_MOUSE;
    boot(&cfg);
    tick();                                   /* no activity at all */
    CHECK(pad_device_presented() == PADDEV_MOUSE && pad_type == 0 && n_initpad == 1);
    pad_device_pad_buttons(0x0040); tick(); tick();
    CHECK(pad_device_presented() == PADDEV_MOUSE && pad_type == 0);
    cfg = CFG_DEFAULT; cfg.force = PADDEV_NONE;
    boot(&cfg); tick();
    CHECK(pad_device_presented() == PADDEV_NONE && pad_type == 0xFF);
    return 0;
}

/* 7. The libetc helpers over the same record, index contract included. */
int test_pad_mouse_helpers(void) {
    int r;
    s8 st[4]; s32 rd[3];
    boot(&CFG_DEFAULT);
    tick();
    r = go_mouse(); if (r) return r;
    pad_device_mouse_motion(7, -2); pad_device_mouse_button(0, 1);
    tick();
    CHECK(MouseState(0, st) == 0);
    CHECK(st[0] == 7 && st[1] == -2 && st[2] == (s8)0xF7 && st[3] == 0x12);
    /* MouseRead(0): clamp adds the deltas to the SetMouse seed, divides by
     * the sense factors, button word = ~[3] & 0x0C */
    MouseRead(0, rd);
    CHECK(mouse_x_acc[0] == 480 + 7 && mouse_y_acc[0] == 480 - 2);
    CHECK(rd[0] == (480 + 7) / 3 && rd[1] == (480 - 2) / 4 && rd[2] == 0x08);
    /* index 1 is the empty port-2 record: clamp bails, sentinel button word */
    CHECK(MouseState(1, st) == -1 && st[0] == 0 && st[3] == 0);
    mouse_x_acc[1] = 99;
    MouseRead(1, rd);
    CHECK(mouse_x_acc[1] == 99 && rd[0] == 33 && rd[2] == -256);
    /* the helper bounds (0..960 / 0..960) clamp independently of the game's */
    pad_device_mouse_button(0, 0);
    mouse_x_acc[0] = 950;
    pad_device_mouse_motion(100, 0); tick();
    MouseRead(0, rd);
    CHECK(mouse_x_acc[0] == 960 && rd[0] == 320);
    return 0;
}

/* 8. The absolute mouse (pc/hal/pad.c, mouse_mode = absolute): the counts
 * pad_steer_counts asks for land the game's own cursor exactly on the
 * target through mouse_pad_set, at every OPTION cursor speed, and a frame
 * that reads one record twice (a skipped VSync) cannot make it oscillate.
 * Negative control: the same targets fed as relative motion (counts = the
 * distance) overshoot or undershoot whenever speed != 3. */
static int steer_to(pad_steer_axis *sx, pad_steer_axis *sy, int tx, int ty, int polls,
                    int reads_per_poll) {
    int i, k;
    for (i = 0; i < polls; i++) {
        pad_device_mouse_counts(pad_steer_counts(sx, mouse_x, tx, csr_speed),
                                pad_steer_counts(sy, mouse_y, ty, csr_speed));
        pad_device_tick();
        for (k = 0; k < reads_per_poll; k++) pad_main();
    }
    return 0;
}

int test_pad_mouse_absolute(void) {
    static const int speeds[] = { 1, 2, 3, 4, 6 };
    static const int targets[][2] = { { 0, 0 }, { 37, -21 }, { -150, -100 }, { 140, 100 },
                                      { 139, 99 }, { -149, 1 }, { 5, 5 } };
    pad_steer_axis sx, sy;
    int r, si, ti, i, c;
    /* the count chooser alone */
    CHECK(pad_mouse_counts_for(10, 3) == 10 && pad_mouse_counts_for(-10, 3) == -10);
    CHECK(pad_mouse_counts_for(10, 1) == 30 && pad_mouse_counts_for(1, 6) == 0);
    CHECK(pad_mouse_counts_for(2, 6) == 1 && pad_mouse_counts_for(-3, 6) == -1);
    CHECK(pad_mouse_counts_for(300, 3) == 127 && pad_mouse_counts_for(-300, 1) == -128);
    for (c = -128; c <= 127; c++)                      /* exact whenever reachable */
        for (si = 0; si < 5; si++) {
            int d = c * speeds[si] / 3;
            CHECK(pad_mouse_counts_for(d, speeds[si]) * speeds[si] / 3 == d);
        }

    for (si = 0; si < 5; si++) {
        boot(&CFG_DEFAULT);
        csr_speed = (u8)speeds[si];
        tick();
        r = go_mouse(); if (r) return r;
        pad_steer_reset(&sx); pad_steer_reset(&sy);
        for (ti = 0; ti < 7; ti++) {
            int tx = targets[ti][0], ty = targets[ti][1];
            steer_to(&sx, &sy, tx, ty, 8, 1);
            /* speed 6 moves in steps of 2: within one game unit */
            if (speeds[si] == 6) {
                CHECK(cursor_cw_x - tx <= 1 && tx - cursor_cw_x <= 1);
                CHECK(cursor_cw_y - ty <= 1 && ty - cursor_cw_y <= 1);
            } else {
                CHECK(cursor_cw_x == tx && cursor_cw_y == ty);
            }
            /* resting on target: the record reports no motion */
            steer_to(&sx, &sy, tx, ty, 1, 1);
            if (speeds[si] != 6) CHECK(ps1_pad1_rec[4] == 0 && ps1_pad1_rec[5] == 0);
        }
    }

    /* a record read twice per poll: converges, never oscillates */
    boot(&CFG_DEFAULT);
    tick();
    r = go_mouse(); if (r) return r;
    pad_steer_reset(&sx); pad_steer_reset(&sy);
    steer_to(&sx, &sy, 60, -40, 1, 2);                 /* first step lands twice */
    CHECK(cursor_cw_x == 120 && cursor_cw_y == -80);
    for (i = 0; i < 12; i++) steer_to(&sx, &sy, 60, -40, 1, 2);
    CHECK(cursor_cw_x == 60 && cursor_cw_y == -40);
    steer_to(&sx, &sy, 60, -40, 1, 1);                 /* back to single reads */
    steer_to(&sx, &sy, 10, 10, 6, 1);
    CHECK(cursor_cw_x == 10 && cursor_cw_y == 10);

    /* negative control: relative motion of the same distance at speed 4 */
    boot(&CFG_DEFAULT);
    csr_speed = 4;
    tick();
    r = go_mouse(); if (r) return r;
    pad_device_mouse_counts(30, 0); tick();
    CHECK(cursor_cw_x == 40);                          /* not 30: speed multiplies */
    return 0;
}

/* 9. Port 2 ([input] port2_controller, pc/hal/pad.c): a digital pad there
 *    with nothing pressed reads exactly as the empty port; its presses
 *    reach pad_get's high half, and mouse_pad_set's while port 1 is a
 *    mouse; unplugged it is empty again. */
int test_pad_port2(void) {
    int r;
    boot(&CFG_DEFAULT);
    tick();
    CHECK(ps1_pad2_rec[0] == 0xFF && pad_get() == 0);
    pad_device_port2(1, 0);
    tick();
    CHECK(ps1_pad2_rec[0] == 0x00 && ps1_pad2_rec[1] == 0x41);
    CHECK(ps1_pad2_rec[2] == 0xFF && ps1_pad2_rec[3] == 0xFF);
    CHECK(pad_get() == 0);                    /* as the empty port */
    pad_device_port2(1, 0x1000 | 0x0040);     /* UP + CROSS on pad 2 */
    pad_device_pad_buttons(0x0020);           /* CIRCLE on pad 1 */
    tick();
    CHECK(ps1_pad2_rec[2] == 0xEF && ps1_pad2_rec[3] == 0xBF);
    CHECK(pad_get() == (0x10400000u | 0x0020u));
    CHECK(pad_device_presented() == PADDEV_PAD);   /* port 1 is unaffected */
    pad_device_release();                     /* focus lost: nothing held */
    pad_device_pad_buttons(0);
    tick();
    CHECK(ps1_pad2_rec[0] == 0x00 && ps1_pad2_rec[2] == 0xFF && ps1_pad2_rec[3] == 0xFF);
    /* port 1 a mouse: mouse_pad_set folds pad 2's buttons in */
    r = go_mouse(); if (r) return r;
    pad_device_port2(1, 0x0040);
    tick();
    CHECK(pad_type == 0);
    CHECK((pad_data & 0x00400000u) != 0);
    pad_device_port2(0, 0x0040);              /* unplugged */
    tick();
    CHECK(ps1_pad2_rec[0] == 0xFF && ps1_pad2_rec[1] == 0xFF);
    CHECK((pad_data & 0xFFFF0000u) == 0);
    return 0;
}
