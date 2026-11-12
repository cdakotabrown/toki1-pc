/* SPDX-License-Identifier: GPL-3.0-or-later */
/* controls_text.c -- the controls legend's text (pc/include/pc_controls.h).
 *
 * SDL-free.  Reads the resolved keymaps the input HAL polls (pc/hal/pad.c:
 * one PcBind per key / button / axis, with the PS1 mask it presses), so
 * every binding the game can receive is listed and nothing else: an unknown
 * name in the settings file never reached the keymap and is not shown.
 */
#include "pc_controls.h"
#include "pad_map.h"

#include <stdio.h>
#include <string.h>

/* display order: the face buttons first (they are what the game asks for) */
static const struct { int button; const char *label; } k_rows[] = {
    { PC_BTN_CIRCLE, "Circle (confirm)" },
    { PC_BTN_CROSS, "Cross (cancel)" },
    { PC_BTN_TRIANGLE, "Triangle" },
    { PC_BTN_SQUARE, "Square" },
    { PC_BTN_UP, "D-pad up" },
    { PC_BTN_DOWN, "D-pad down" },
    { PC_BTN_LEFT, "D-pad left" },
    { PC_BTN_RIGHT, "D-pad right" },
    { PC_BTN_L1, "L1" },
    { PC_BTN_R1, "R1" },
    { PC_BTN_L2, "L2" },
    { PC_BTN_R2, "R2" },
    { PC_BTN_START, "START" },
    { PC_BTN_SELECT, "SELECT" },
};

const char *controls_key_label(const char *n) {
    static const struct { const char *sdl, *label; } k[] = {
        { "Return", "Enter" },
        { "Up", "Up arrow" },
        { "Down", "Down arrow" },
        { "Left", "Left arrow" },
        { "Right", "Right arrow" },
        { "Escape", "Esc" },
    };
    size_t i;
    if (!n) return "?";
    for (i = 0; i < sizeof k / sizeof k[0]; i++)
        if (!strcmp(n, k[i].sdl)) return k[i].label;
    return n;
}

/* Button names per controller family (pc/include/pad_map.h).  The SDL name
 * is the POSITION (a = bottom, b = right, x = left, y = top on every pad:
 * pc/hal/pad.c sets SDL_GAMECONTROLLER_USE_BUTTON_LABELS=0); the label is
 * what is printed on that button. */
typedef struct { const char *sdl, *label; } NameLabel;
static const NameLabel k_xbox[] = {
    { "a", "A (bottom)" }, { "b", "B (right)" }, { "x", "X (left)" }, { "y", "Y (top)" },
    { "back", "Back" }, { "guide", "Guide" }, { "start", "Start" },
    { "leftstick", "Left stick press" }, { "rightstick", "Right stick press" },
    { "leftshoulder", "LB" }, { "rightshoulder", "RB" },
    { "misc1", "Share" }, { "touchpad", "Touchpad" }, { NULL, NULL },
};
static const NameLabel k_playstation[] = {
    { "a", "Cross" }, { "b", "Circle" }, { "x", "Square" }, { "y", "Triangle" },
    { "back", "Share" }, { "guide", "PS button" }, { "start", "Options" },
    { "leftstick", "L3" }, { "rightstick", "R3" },
    { "leftshoulder", "L1" }, { "rightshoulder", "R1" },
    { "misc1", "Mute" }, { "touchpad", "Touchpad" }, { NULL, NULL },
};
static const NameLabel k_nintendo[] = {
    { "a", "B (bottom)" }, { "b", "A (right)" }, { "x", "Y (left)" }, { "y", "X (top)" },
    { "back", "-" }, { "guide", "Home" }, { "start", "+" },
    { "leftstick", "Left stick press" }, { "rightstick", "Right stick press" },
    { "leftshoulder", "L" }, { "rightshoulder", "R" },
    { "misc1", "Capture" }, { NULL, NULL },
};
static const NameLabel k_dpad[] = {
    { "dpup", "D-pad up" }, { "dpdown", "D-pad down" },
    { "dpleft", "D-pad left" }, { "dpright", "D-pad right" }, { NULL, NULL },
};

const char *controls_family_name(int family) {
    switch (family) {
    case PAD_FAMILY_PLAYSTATION: return "PlayStation";
    case PAD_FAMILY_NINTENDO: return "Nintendo";
    case PAD_FAMILY_GENERIC: return "generic";
    default: return "Xbox";
    }
}

static const char *table_label(const NameLabel *t, const char *n) {
    for (; t->sdl; t++)
        if (!strcmp(n, t->sdl)) return t->label;
    return NULL;
}

void controls_pad_label_family(const char *n, int sign, int family, char *out, size_t cap) {
    static const struct { const char *sdl, *minus, *plus; } a[] = {
        { "leftx", "Left stick left", "Left stick right" },
        { "lefty", "Left stick up", "Left stick down" },
        { "rightx", "Right stick left", "Right stick right" },
        { "righty", "Right stick up", "Right stick down" },
    };
    const char *lab;
    size_t i;
    if (!n) n = "?";
    if (sign) {
        if (!strcmp(n, "lefttrigger") || !strcmp(n, "righttrigger")) {
            int left = n[0] == 'l';
            const char *t;
            int b;
            switch (family) {
            case PAD_FAMILY_PLAYSTATION: t = left ? "L2" : "R2"; break;
            case PAD_FAMILY_NINTENDO: t = left ? "ZL" : "ZR"; break;
            case PAD_FAMILY_GENERIC:
                b = pad_fallback_button(n);
                snprintf(out, cap, "Button %d%s", b + 1, sign > 0 ? "" : " (released)");
                return;
            default: t = left ? "LT" : "RT"; break;
            }
            snprintf(out, cap, "%s%s", t, sign > 0 ? "" : " (released)");
            return;
        }
        for (i = 0; i < sizeof a / sizeof a[0]; i++)
            if (!strcmp(n, a[i].sdl)) {
                /* a generic device's one stick (or arcade lever) is "Stick" */
                if (family == PAD_FAMILY_GENERIC && n[0] == 'l')
                    snprintf(out, cap, "Stick %s", n[4] == 'x' ? (sign > 0 ? "right" : "left")
                                                              : (sign > 0 ? "down" : "up"));
                else
                    snprintf(out, cap, "%s", sign > 0 ? a[i].plus : a[i].minus);
                return;
            }
        snprintf(out, cap, "%c%s", sign > 0 ? '+' : '-', n);
        return;
    }
    if ((lab = table_label(k_dpad, n)) != NULL) {
        snprintf(out, cap, "%s", lab);
        return;
    }
    switch (family) {
    case PAD_FAMILY_PLAYSTATION: lab = table_label(k_playstation, n); break;
    case PAD_FAMILY_NINTENDO: lab = table_label(k_nintendo, n); break;
    case PAD_FAMILY_GENERIC: {
        int b = pad_fallback_button(n);
        if (b >= 0) {
            snprintf(out, cap, "Button %d", b + 1);
            return;
        }
        lab = NULL;
        break;
    }
    default: lab = NULL; break;
    }
    if (!lab) lab = table_label(k_xbox, n);
    snprintf(out, cap, "%s", lab ? lab : n);
}

void controls_pad_label(const char *n, int sign, char *out, size_t cap) {
    controls_pad_label_family(n, sign, PAD_FAMILY_XBOX, out, cap);
}

static void cell_add(char *cell, const char *label) {
    size_t len = strlen(cell);
    /* the same input listed twice (bound twice) once */
    const char *p = cell;
    size_t ln = strlen(label);
    while ((p = strstr(p, label)) != NULL) {
        int start = p == cell || (p >= cell + 2 && p[-2] == ',' && p[-1] == ' ');
        int end = p[ln] == 0 || p[ln] == ',';
        if (start && end) return;
        p += ln;
    }
    if (len) snprintf(cell + len, CONTROLS_CELL - len, ", %s", label);
    else snprintf(cell, CONTROLS_CELL, "%s", label);
}

void controls_legend_build(ControlsLegend *out, const PcKeymap *keys, const PcKeymap *pad,
                           ControlsNameFn key_name, ControlsNameFn button_name,
                           ControlsNameFn axis_name) {
    controls_legend_build_family(out, keys, pad, key_name, button_name, axis_name,
                                 PAD_FAMILY_XBOX);
}

void controls_legend_build_family(ControlsLegend *out, const PcKeymap *keys,
                                  const PcKeymap *pad, ControlsNameFn key_name,
                                  ControlsNameFn button_name, ControlsNameFn axis_name,
                                  int family) {
    size_t r;
    int i;
    memset(out, 0, sizeof *out);
    for (r = 0; r < sizeof k_rows / sizeof k_rows[0]; r++) {
        ControlsRow *row = &out->row[out->n++];
        unsigned mask = pc_button_masks[k_rows[r].button];
        row->button = k_rows[r].button;
        row->ps1 = k_rows[r].label;
        for (i = 0; keys && i < keys->n; i++)
            if (keys->b[i].mask & mask)
                cell_add(row->keys, controls_key_label(key_name ? key_name(keys->b[i].code) : NULL));
        for (i = 0; pad && i < pad->n; i++)
            if (pad->b[i].mask & mask) {
                char lab[64];
                const PcBind *b = &pad->b[i];
                ControlsNameFn fn = b->axis_sign ? axis_name : button_name;
                controls_pad_label_family(fn ? fn(b->code) : NULL, b->axis_sign, family, lab,
                                          sizeof lab);
                cell_add(row->pad, lab);
            }
        if (!row->keys[0]) snprintf(row->keys, sizeof row->keys, "-");
        if (!row->pad[0]) snprintf(row->pad, sizeof row->pad, "-");
    }
}

int controls_legend_text(const ControlsLegend *l, char *out, size_t n) {
    size_t len = 0;
    int i;
    if (!n) return 0;
    out[0] = 0;
    for (i = 0; i < l->n && len < n; i++) {
        int w = snprintf(out + len, n - len, "%s | %s | %s\n", l->row[i].ps1, l->row[i].keys,
                         l->row[i].pad);
        if (w < 0) break;
        len += (size_t)w;
    }
    return (int)(len < n ? len : n - 1);
}
