/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pc_controls.h -- the controls legend (pc/hal/controls_text.c builds the
 * text, pc/hal/controls_view.c shows it).
 *
 * One row per PS1 pad button with the keyboard keys and controller inputs
 * bound to it NOW: the rows are built from the input HAL's own resolved
 * keymaps (pc/hal/pad.c, from [keyboard] / [controller] and
 * controller_layout), so the legend cannot drift from what the game is
 * given.  Reference UI, both profiles; it never touches PS1 state.
 *
 * The builder is SDL-free (unit-tested in pc/tests/test_controls_legend.c):
 * the caller passes the code -> name functions (SDL's in the game).
 */
#ifndef PC_CONTROLS_H
#define PC_CONTROLS_H

#include <stddef.h>
#include "pc_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONTROLS_CELL 160

typedef struct {
    int button;                 /* PC_BTN_* */
    const char *ps1;            /* "Circle (confirm)" */
    char keys[CONTROLS_CELL];   /* "Enter, Space, Keypad Enter", or "-" */
    char pad[CONTROLS_CELL];    /* "B (right), ...", or "-" */
} ControlsRow;

typedef struct {
    ControlsRow row[PC_BTN_COUNT];
    int n;
} ControlsLegend;

/* code -> SDL name ("Return", "a", "lefttrigger"); NULL when unknown */
typedef const char *(*ControlsNameFn)(int code);

/* The legend from resolved keymaps (pc_keymap_build_keys / _pad).  `keys`
 * or `pad` may be NULL (that column then reads "-"). */
void controls_legend_build(ControlsLegend *out, const PcKeymap *keys, const PcKeymap *pad,
                           ControlsNameFn key_name, ControlsNameFn button_name,
                           ControlsNameFn axis_name);

/* The same for a controller family (PAD_FAMILY_*, pc/include/pad_map.h):
 * the inputs are named as printed on that family's pads. */
void controls_legend_build_family(ControlsLegend *out, const PcKeymap *keys,
                                  const PcKeymap *pad, ControlsNameFn key_name,
                                  ControlsNameFn button_name, ControlsNameFn axis_name,
                                  int family);

/* A keyboard key's label ("Return" -> "Enter"); the SDL name otherwise. */
const char *controls_key_label(const char *sdl_name);
/* A controller input's label ("a" -> "A (bottom)", axis "lefty" sign -1 ->
 * "Left stick up"). */
void controls_pad_label(const char *sdl_name, int axis_sign, char *out, size_t n);
/* ... as printed on a PAD_FAMILY_* pad ("b" -> "Circle" on a PlayStation
 * pad, "A (right)" on a Nintendo one, "Button 2" on a generic joystick). */
void controls_pad_label_family(const char *sdl_name, int axis_sign, int family, char *out,
                               size_t n);
/* "Xbox", "PlayStation", "Nintendo", "generic" */
const char *controls_family_name(int family);

/* The legend as text, one "PS1 | keyboard | controller" line per row
 * (logs and tests); returns the length written. */
int controls_legend_text(const ControlsLegend *l, char *out, size_t n);

#ifdef __cplusplus
}
#endif
#endif
