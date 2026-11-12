/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pad_device.c -- PS1 controller-port model: what the BIOS VBLANK poll
 * would leave in the two libpad response buffers, derived from host input.
 *
 * SDL-free (see pad_device.h); pc/hal/pad.c is the SDL glue and
 * pc/tests/test_pad_mouse.c drives this file against the matched game code.
 *
 * The records are the ones the matched code reads, byte for byte:
 *
 *   port 1 record (pad1_buf, 0x800F10D0), 8 bytes registered by
 *   InitPAD(&pad1_buf, 8, &pad2_buf, 8):
 *     [0] status   0 = a device answered, 0xFF = nothing on the port
 *                  (pad_type_check 0x80042D48 reads exactly these two)
 *     [1] id       0x41 digital pad / 0x12 mouse: pad_type_check derives
 *                  pad_type 0 (mouse) from (id & 0xF0) == 0x10, else 1;
 *                  MouseRead / mouse_pos_clamp insist on 0x12
 *     [2] buttons  active-low SELECT/START/UP/RIGHT/DOWN/LEFT (pad);
 *                  always 0xFF on a mouse
 *     [3] buttons  active-low L2/R2/L1/R1/TRI/CIRCLE/CROSS/SQUARE (pad);
 *                  mouse: bit 3 (0x08) left, bit 2 (0x04) right, rest 1 --
 *                  mouse_pad_set 0x80042F20: pressed = ~[3], 0x08 -> 0x20,
 *                  0x04 -> 0x40 (reversed when mouse_pad_st.swap is set)
 *     [4] pad1_ax  s8 X delta since the last poll (mouse)
 *     [5] pad1_ay  s8 Y delta, positive = down
 *   port 2 record (pad2_buf, 0x800F1110): same layout.  Empty (status
 *   0xFF) unless the player asks for a second pad there ([input]
 *   port2_controller: the second game controller, pad_device_port2), then
 *   a digital pad (0x41).  mouse_pad_set folds port 2 into bits 16..31
 *   only when [0] == 0 and (id & 0xF0) == 0x40; pad_get 0x80043280
 *   inverts [2]/[3], and 0xFF bytes contribute 0 -- so a port-2 pad with
 *   nothing pressed reads exactly as an empty port.  The game reads port 2
 *   in pad_main (all four directions held), pad_scan (its START+SELECT+
 *   L1+R1 reset), mascot_move and NAME_ENT.
 *
 * Device policy ("hot-switching"), the way the console sees it:
 *   - port 1 carries the device the player last used: a mouse after mouse
 *     motion or a click, a digital pad after any pad/keyboard button;
 *   - a change of device passes through an EMPTY port for `swap_gap` polls
 *     (default 1), as unplugging one controller and plugging in the other
 *     does.  pad_type_check then runs its own transitions: status 0xFF ->
 *     pad_type 0xFF (or the old mode kept while card_busy != 0), then the
 *     new id -> the mouse or pad init arm (InitPAD, InitMouse, SenseMouse,
 *     SetMouse, RangeMouse, cursor and edge words cleared, menu_mode = 0
 *     for a mouse), returning 1 so pad_main skips that frame.  Nothing
 *     here short-cuts those arms; the HAL only answers the poll.
 *   - swap_gap 0 makes the id change in place (also a transition the
 *     detector handles: old != new -> init arm -> return 1).
 *
 * Motion: host relative motion is scaled by `sensitivity`, accumulated in
 * floating point between polls, and drained once per poll as a signed
 * byte.  The truncated fraction AND any excess beyond the s8 range stay in
 * the accumulator for the next poll (a fast flick arrives over several
 * frames instead of being dropped -- a deliberate PC choice; the real
 * SCPH-1030 saturates and forgets).  Motion that arrives while no mouse is
 * presented and none is wanted is discarded, as motion on an unplugged
 * mouse would be; motion during the swap gap is kept for the first mouse
 * poll (the detector's transition frame, where the game ignores it).
 */
#include "pad_device.h"
#include <stdlib.h>
#include <string.h>

/* The two response records, by the names the matched game reads them
 * through (input_events_init / pad_type_check / mouse_pad_set / pad_get).
 * In the game build pc/src/layout_aliases.ld binds both names to their
 * retail offsets inside screen_w (0x800F10D0 / 0x800F1110), overriding the
 * pc/src/input_globals.c views of ps1_pad1_rec / ps1_pad2_rec; the HAL
 * must write the bytes the game reads, so it never names the backing
 * arrays.  (Until 2026-10-30 it wrote ps1_pad1_rec, which the layout
 * aliases had orphaned on 2026-10-15: no pad, mouse or scripted input
 * reached pad_data in the game.) */
extern u8 pad1_buf[8];
extern u8 pad2_buf[8];

static pad_device_config g_cfg;
static unsigned g_tick;             /* polls since init */
static unsigned g_last_mouse_tick;  /* tick of the last mouse motion/click (0 = never) */
static unsigned g_last_pad_tick;    /* tick of the last pad/keyboard press */
static int g_attached;              /* PADDEV_* physically on port 1 */
static int g_gap_left;              /* empty-port polls still owed */
static double g_acc_x, g_acc_y;     /* motion not yet reported, in counts */
static int g_mouse_left, g_mouse_right;
static int g_mouse_hide, g_mouse_add;   /* pad_device_mouse_filter (quick text) */
static u32 g_pad_buttons;           /* this tick's pressed set (libpad u16 bits) */
static int g_port2;                 /* a pad on port 2 (pad_device_port2) */
static u32 g_port2_buttons;

void pad_device_init(const pad_device_config *cfg)
{
    static const pad_device_config defaults = { 1.0, 1, PADDEV_AUTO, 1 };
    g_cfg = cfg ? *cfg : defaults;
    if (g_cfg.sensitivity <= 0.0) g_cfg.sensitivity = 1.0;
    if (g_cfg.swap_gap < 0) g_cfg.swap_gap = 0;
    g_tick = 0;
    g_last_mouse_tick = g_last_pad_tick = 0;
    g_attached = PADDEV_NONE;
    g_gap_left = 0;
    g_acc_x = g_acc_y = 0.0;
    g_mouse_left = g_mouse_right = 0;
    g_mouse_hide = g_mouse_add = 0;
    g_pad_buttons = 0;
    g_port2 = 0;
    g_port2_buttons = 0;
    memset(pad1_buf, 0xFF, 4); memset(pad1_buf + 4, 0, 4);
    memset(pad2_buf, 0xFF, 4); memset(pad2_buf + 4, 0, 4);
}

void pad_device_mouse_motion(double dx, double dy)
{
    if (dx == 0.0 && dy == 0.0) return;
    g_acc_x += dx * g_cfg.sensitivity;
    g_acc_y += dy * g_cfg.sensitivity;
    g_last_mouse_tick = g_tick + 1;     /* lands in the NEXT poll */
}

void pad_device_mouse_button(int right, int down)
{
    if (right) g_mouse_right = down != 0;
    else       g_mouse_left  = down != 0;
    if (down) g_last_mouse_tick = g_tick + 1;
}

int pad_device_mouse_held(int right)
{
    return right ? g_mouse_right : g_mouse_left;
}

void pad_device_mouse_filter(int hide, int add)
{
    g_mouse_hide = hide & 3;
    g_mouse_add = add & 3;
}

void pad_device_mouse_activity(void)
{
    g_last_mouse_tick = g_tick + 1;
}

void pad_device_mouse_counts(int ax, int ay)
{
    g_acc_x = ax < -128 ? -128 : ax > 127 ? 127 : ax;
    g_acc_y = ay < -128 ? -128 : ay > 127 ? 127 : ay;
}

int pad_mouse_counts_for(int delta, int speed)
{
    int c, best = 0, best_err, guess, k;
    if (speed <= 0 || delta == 0) return 0;
    best_err = delta < 0 ? -delta : delta;
    guess = delta * 3 / speed;
    for (k = -2; k <= 2; k++) {
        int err;
        c = guess + k;
        if (c < -128 || c > 127) continue;
        err = c * speed / 3 - delta;
        if (err < 0) err = -err;
        if (err < best_err || (err == best_err && abs(c) < abs(best))) {
            best = c;
            best_err = err;
        }
    }
    if (guess > 127 || guess < -128) best = guess > 0 ? 127 : -128;   /* far: full speed */
    return best;
}

void pad_steer_reset(pad_steer_axis *a)
{
    a->last_pos = 0;
    a->last_step = 0;
    a->div = 1;
}

int pad_steer_counts(pad_steer_axis *a, int cur, int target, int speed)
{
    int d, c;
    if (a->div < 1) a->div = 1;
    if (a->last_step != 0) {
        int moved = cur - a->last_pos;
        if (moved == a->last_step) a->div = 1;
        else if (moved != 0 && moved % a->last_step == 0 && moved / a->last_step >= 2)
            a->div = moved / a->last_step;
    }
    d = target - cur;
    if (a->div > 1) {
        int q = d / a->div;
        d = q ? q : (d > 0) - (d < 0);
    }
    c = pad_mouse_counts_for(d, speed);
    a->last_pos = cur;
    a->last_step = speed > 0 ? c * speed / 3 : 0;
    return c;
}

void pad_device_pad_buttons(u32 psx_buttons)
{
    g_pad_buttons = psx_buttons & 0xFFFFu;
    if (g_pad_buttons) g_last_pad_tick = g_tick + 1;
}

void pad_device_release(void)
{
    g_mouse_left = g_mouse_right = 0;
    g_pad_buttons = 0;
    g_port2_buttons = 0;
    g_acc_x = g_acc_y = 0.0;
}

int pad_device_wanted(void)
{
    int want;
    if (g_cfg.force != PADDEV_AUTO) {
        want = g_cfg.force;
    } else if (g_last_mouse_tick > g_last_pad_tick) {
        want = PADDEV_MOUSE;
    } else {
        want = PADDEV_PAD;              /* ties and "nothing yet": a pad, as before */
    }
    if (want == PADDEV_MOUSE && !g_cfg.mouse_enabled) want = PADDEV_PAD;
    return want;
}

/* Drain one poll's worth of motion as an s8, carrying the remainder. */
static s8 drain(double *acc)
{
    double whole = (double)(long)*acc;      /* truncate toward zero */
    if (whole > 127.0) whole = 127.0;
    if (whole < -128.0) whole = -128.0;
    *acc -= whole;
    return (s8)(int)whole;
}

void pad_device_tick(void)
{
    int want;
    u8 *r = pad1_buf;

    g_tick++;
    want = pad_device_wanted();

    /* A swap between two real devices unplugs the first one for swap_gap
     * polls; the empty port is what pad_type_check's 0xFF arm is for. */
    if (want != g_attached && g_attached != PADDEV_NONE && want != PADDEV_NONE
        && g_cfg.swap_gap > 0 && g_gap_left == 0) {
        g_gap_left = g_cfg.swap_gap;
        g_attached = PADDEV_NONE;
    }
    if (g_gap_left > 0) {
        g_gap_left--;
    } else if (want != g_attached) {
        g_attached = want;
    }

    switch (g_attached) {
    case PADDEV_MOUSE: {
        int left = (g_mouse_left && !(g_mouse_hide & 1)) || (g_mouse_add & 1);
        int right = (g_mouse_right && !(g_mouse_hide & 2)) || (g_mouse_add & 2);
        r[0] = 0x00;
        r[1] = 0x12;
        r[2] = 0xFF;
        r[3] = (u8)(0xFF & ~(left ? 0x08 : 0) & ~(right ? 0x04 : 0));
        r[4] = (u8)drain(&g_acc_x);
        r[5] = (u8)drain(&g_acc_y);
        r[6] = r[7] = 0;
        break;
    }
    case PADDEV_PAD:
        r[0] = 0x00;
        r[1] = 0x41;
        r[2] = (u8)~((g_pad_buttons >> 8) & 0xFFu);   /* SELECT..LEFT, active low */
        r[3] = (u8)~(g_pad_buttons & 0xFFu);          /* L2..SQUARE, active low */
        r[4] = r[5] = r[6] = r[7] = 0;                /* a digital pad sends no more */
        if (want != PADDEV_MOUSE) g_acc_x = g_acc_y = 0.0;   /* no mouse to carry it */
        break;
    default:
        r[0] = r[1] = r[2] = r[3] = 0xFF;             /* nobody answered */
        r[4] = r[5] = r[6] = r[7] = 0;
        if (want != PADDEV_MOUSE) g_acc_x = g_acc_y = 0.0;
        break;
    }

    /* Port 2: empty, or the second pad. */
    if (g_port2) {
        pad2_buf[0] = 0x00;
        pad2_buf[1] = 0x41;
        pad2_buf[2] = (u8)~((g_port2_buttons >> 8) & 0xFFu);
        pad2_buf[3] = (u8)~(g_port2_buttons & 0xFFu);
    } else {
        pad2_buf[0] = pad2_buf[1] = pad2_buf[2] = pad2_buf[3] = 0xFF;
    }
    pad2_buf[4] = pad2_buf[5] = pad2_buf[6] = pad2_buf[7] = 0;
}

void pad_device_port2(int connected, u32 psx_buttons)
{
    g_port2 = connected != 0;
    g_port2_buttons = g_port2 ? psx_buttons & 0xFFFFu : 0;
}

int pad_device_presented(void) { return g_attached; }
unsigned pad_device_ticks(void) { return g_tick; }

/* ---- hold-off after the PC settings menu (pad_device.h) ---- */
static int g_hold_off;

void pad_hold_off_arm(void)
{
    g_hold_off = 1;
}

u32 pad_hold_off(u32 held)
{
    if (!g_hold_off) return held;
    if (held) return 0;
    g_hold_off = 0;
    return 0;
}
