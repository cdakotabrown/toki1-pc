/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pad_map.c -- the default mapping for a joystick SDL does not know
 * (pc/include/pad_map.h).
 *
 * SDL turns a joystick into a "game controller" only when its mapping
 * database has a line for the device's GUID (its own built-in list, a
 * gamecontrollerdb.txt, SDL_GAMECONTROLLERCONFIG).  A generic USB / DirectInput
 * pad, an arcade-stick encoder or a cheap clone often has none and would
 * be ignored.  pc/hal/pad.c gives such a device this line instead.
 *
 * The button order is the one most generic pads share, the PlayStation-
 * style clones' (DragonRise and the "USB Joystick" encoders), as printed on
 * them:
 *
 *   button 1 triangle   2 circle   3 cross   4 square
 *          5 L1   6 R1   7 L2   8 R2   9 select   10 start
 *          11 L3  12 R3  13 home
 *
 * The other common numbering (Logitech's DirectInput mode: 1 X, 2 A, 3 B,
 * 4 Y) gives button 2 -- its A, the bottom one -- circle as well, and 3 cross,
 * so confirm and cancel land on the two main buttons either way; only
 * triangle and square trade places.  The left stick is axes 0/1 and the
 * d-pad is hat 0 (both drive the PS1 d-pad, [controller] defaults);
 * rightx/righty are axes 2/3 (unbound by default).  A player whose device
 * differs writes the line pc/hal/pad.c prints into gamecontrollerdb.txt
 * with the numbers changed (docs/pc-port/PLAYER-GUIDE.md, "Controllers").
 */
#include "pad_map.h"

#include <stdio.h>
#include <string.h>

/* GameController button name -> joystick button (0-based); SDL mapping
 * names, so "lefttrigger" here is the trigger read as a button. */
static const struct { const char *name; int button; } k_buttons[] = {
    { "y", 0 }, { "b", 1 }, { "a", 2 }, { "x", 3 },
    { "leftshoulder", 4 }, { "rightshoulder", 5 },
    { "lefttrigger", 6 }, { "righttrigger", 7 },
    { "back", 8 }, { "start", 9 },
    { "leftstick", 10 }, { "rightstick", 11 },
    { "guide", 12 },
};
#define NBUTTONS ((int)(sizeof k_buttons / sizeof k_buttons[0]))

int pad_fallback_button(const char *sdl_name) {
    int i;
    if (!sdl_name) return -1;
    for (i = 0; i < NBUTTONS; i++)
        if (!strcmp(k_buttons[i].name, sdl_name)) return k_buttons[i].button;
    return -1;
}

int pad_fallback_mapping(char *out, size_t n, const char *guid, const char *name,
                         int nbuttons, int naxes, int nhats) {
    size_t len = 0;
    int i, w;
    if (!out || !n || !guid || !*guid || nbuttons <= 0) return -1;
#define PUT(...) do { \
        w = snprintf(out + len, n - len, __VA_ARGS__); \
        if (w < 0 || (size_t)w >= n - len) return -1; \
        len += (size_t)w; \
    } while (0)
    PUT("%s,", guid);
    /* the name is the mapping's second field: no commas, never empty */
    {
        const char *p = name && *name ? name : "Generic joystick";
        for (; *p && len + 1 < n; p++) out[len++] = *p == ',' ? ' ' : *p;
        if (len + 1 >= n) return -1;
        out[len] = 0;
    }
    for (i = 0; i < NBUTTONS; i++)
        if (k_buttons[i].button < nbuttons) PUT(",%s:b%d", k_buttons[i].name, k_buttons[i].button);
    if (naxes >= 2) PUT(",leftx:a0,lefty:a1");
    if (naxes >= 4) PUT(",rightx:a2,righty:a3");
    if (nhats >= 1) PUT(",dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8");
    PUT(",");
#undef PUT
    return (int)len;
}
