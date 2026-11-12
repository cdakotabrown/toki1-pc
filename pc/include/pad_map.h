/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pad_map.h -- the default SDL GameController mapping for a joystick SDL
 * has no mapping for (pc/hal/pad_map.c), and the controller families the
 * PC menus name buttons for.
 *
 * SDL-free: pc/hal/pad.c builds the mapping string from the device's GUID,
 * name and counts and hands it to SDL_GameControllerAddMapping, so an
 * unknown pad, arcade stick or clone becomes an ordinary game controller
 * (events, the menus, [controller] bindings, the legend).  Unit-tested in
 * pc/tests/test_pad_map.c.
 */
#ifndef PAD_MAP_H
#define PAD_MAP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Who made the controller: decides the button names the PC menus show
 * (pc/hal/controls_text.c).  Bindings are by POSITION for every family
 * (SDL_GAMECONTROLLER_USE_BUTTON_LABELS=0, pc/hal/pad.c). */
enum {
    PAD_FAMILY_XBOX,          /* also the default when nothing is connected */
    PAD_FAMILY_PLAYSTATION,
    PAD_FAMILY_NINTENDO,
    PAD_FAMILY_GENERIC,       /* a joystick on the default mapping below */
    PAD_FAMILY_COUNT
};

/* The default mapping: a GameController button name -> the joystick
 * button index it reads (0-based), or -1 when it is not on a button. */
int pad_fallback_button(const char *sdl_name);

/* The mapping line for one joystick, SDL_GameControllerDB format:
 * "<guid>,<name>,a:b2,b:b1,...".  Buttons the device does not have are
 * left out; the left stick is axes 0/1, the d-pad hat 0.  Commas in the
 * name are replaced.  Returns the length, or -1 when it does not fit or
 * the device has no button. */
int pad_fallback_mapping(char *out, size_t n, const char *guid, const char *name,
                         int nbuttons, int naxes, int nhats);

#ifdef __cplusplus
}
#endif
#endif
