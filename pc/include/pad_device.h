/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pad_device.h -- the PS1 controller-port model behind the PC input HAL.
 *
 * SDL-free.  pc/hal/pad.c feeds it host input (relative mouse motion, mouse
 * buttons, the keyboard/gamepad button set) and calls pad_device_tick()
 * once per game tick; the model decides what device port 1 presents and
 * writes the libpad response records the matched game code reads
 * (pc/src/input_globals.c: ps1_pad1_rec / ps1_pad2_rec, i.e. pad1_buf..
 * pad1_ay and pad2_buf..pad2_btn_hi).  The game's own detector
 * (pad_type_check, 0x80042D48), cursor updater (mouse_pad_set, 0x80042F20)
 * and button packer (pad_get, 0x80043280) then do all the work.
 *
 * pc/tests/test_pad_mouse.c drives this model against those matched
 * functions headlessly.
 */
#ifndef PAD_DEVICE_H
#define PAD_DEVICE_H

#include "types.h"

/* What is plugged into port 1. */
#define PADDEV_NONE   0     /* empty port: status byte 0xFF */
#define PADDEV_PAD    1     /* digital pad, id 0x41 */
#define PADDEV_MOUSE  2     /* PS1 mouse SCPH-1030, id 0x12 */
#define PADDEV_AUTO  (-1)   /* policy picks from host activity */

typedef struct {
    double sensitivity;     /* host motion units per mouse count (default 1.0) */
    int    swap_gap;        /* empty-port polls between two devices (default 1) */
    int    force;           /* PADDEV_AUTO, or a PADDEV_* to pin port 1 */
    int    mouse_enabled;   /* 0: never present a mouse */
} pad_device_config;

/* Reset the model and apply a configuration (NULL = defaults). */
void pad_device_init(const pad_device_config *cfg);

/* Host input, any time between ticks. */
void pad_device_mouse_motion(double dx, double dy);      /* host units, pre-sensitivity */
void pad_device_mouse_button(int right, int down);       /* right=0 left button, 1 right */
void pad_device_pad_buttons(u32 psx_buttons);            /* this tick's pressed set, libpad u16 bits */
void pad_device_release(void);                           /* host lost focus: drop held state */
/* Port 2: a second digital pad ([input] port2_controller, pc/hal/pad.c),
 * or nothing (connected = 0, the default).  Taken at the next tick. */
void pad_device_port2(int connected, u32 psx_buttons);

/* Quick text (pc/hal/quick_text.c, enhanced): whether a mouse button is
 * held (right=0 left, 1 right), and a filter the next ticks apply to the
 * reported buttons: `hide` drops, `add` reports (bit 0 left, bit 1 right).
 * (0, 0) -- the default -- reports the buttons as they are. */
int pad_device_mouse_held(int right);
void pad_device_mouse_filter(int hide, int add);

/* The absolute mouse (pc/hal/pad.c, [input] mouse_mode = absolute): the
 * host decides each poll's counts itself from where the OS pointer is.
 *   pad_device_mouse_activity()  the pointer moved: the player is using the
 *                                mouse (hot-switch policy), no motion added
 *   pad_device_mouse_counts()    the next poll reports exactly (ax, ay)
 *                                counts, replacing anything accumulated
 *                                (no sensitivity, no carry) */
void pad_device_mouse_activity(void);
void pad_device_mouse_counts(int ax, int ay);

/* The s8 mouse count c whose cursor step in mouse_pad_set (0x80042F20:
 * `step = c * speed / 3`, C division) comes closest to `delta` game units;
 * ties go to the smaller |c|.  speed is mouse_pad_st.speed (csr_speed). */
int  pad_mouse_counts_for(int delta, int speed);

/* One axis of the absolute mouse: the counts for this poll that take the
 * game's cursor accumulator (mouse_pad_st.x / .y) from `cur` to `target`.
 * It also watches what the previous poll's counts did: mouse_pad_set reads
 * the record once per game frame, and a frame that skips its VSync reads
 * the same record twice (the console does this too).  If the cursor moved
 * by a whole multiple k >= 2 of the step asked for, later corrections are
 * divided by k until a single step is seen again, so a repeated read can
 * never make the cursor oscillate around the target. */
typedef struct { int last_pos, last_step, div; } pad_steer_axis;
void pad_steer_reset(pad_steer_axis *a);
int  pad_steer_counts(pad_steer_axis *a, int cur, int target, int speed);

/* Hold-off after the PC settings menu closes (pc/hal/pad.c): the keys and
 * buttons that drove it (Escape is cross, Enter circle) are still down.
 * pad_hold_off_arm() starts it; pad_hold_off(held) then returns 0 until a
 * poll sees nothing held, and `held` unchanged from that poll on. */
void pad_hold_off_arm(void);
u32  pad_hold_off(u32 held);

/* One BIOS poll: pick the device, write both response records. */
void pad_device_tick(void);

/* Introspection (after a tick). */
int  pad_device_presented(void);    /* PADDEV_* on port 1 now */
int  pad_device_wanted(void);       /* what the activity policy asks for */
unsigned pad_device_ticks(void);    /* ticks since init */

#endif /* PAD_DEVICE_H */
