/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_controls_legend.c -- the controls legend's text (pc/hal/
 * controls_text.c) from keymaps resolved exactly as pc/hal/pad.c resolves
 * them (pc_keymap_build_keys / _pad over the settings), with name tables
 * standing in for SDL's: the defaults, controller_layout = confirm_south,
 * remaps, unbinding, unknown names, a binding listed twice.  And the
 * [hotkeys] controls default (Ctrl+H) clashes with no other key.  No SDL. */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "pc_settings.h"
#include "pc_controls.h"
#include "pad_map.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "  %s:%d: %s\n", __FILE__, __LINE__, #c); return __LINE__; } } while (0)

static const char *const k_keys[] = {
    "Up", "Down", "Left", "Right", "Return", "Space", "Keypad Enter", "Backspace", "Escape",
    "Q", "E", "A", "D", "Z", "C", "F1", "S", "Tab", "J", "K", "F2", NULL };
static const char *const k_buttons[] = {
    "a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick", "leftshoulder",
    "rightshoulder", "dpup", "dpdown", "dpleft", "dpright", NULL };   /* SDL's order */
static const char *const k_axes[] = {
    "leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger", NULL };

static int find(const char *const *t, const char *n) {
    int i;
    for (i = 0; t[i]; i++)
        if (!strcasecmp(t[i], n)) return i;
    return -1;
}
static int key_lookup(const char *n) { return find(k_keys, n); }
static int button_lookup(const char *n) { return find(k_buttons, n); }
static int axis_lookup(const char *n) { return find(k_axes, n); }
static const char *key_name(int c) { return c >= 0 && c < 21 ? k_keys[c] : NULL; }
static const char *button_name(int c) { return c >= 0 && c < 15 ? k_buttons[c] : NULL; }
static const char *axis_name(int c) { return c >= 0 && c < 6 ? k_axes[c] : NULL; }

static const ControlsRow *row(const ControlsLegend *l, int button) {
    int i;
    for (i = 0; i < l->n; i++)
        if (l->row[i].button == button) return &l->row[i];
    return NULL;
}

static void legend_of(const PcSettings *s, ControlsLegend *l) {
    static PcKeymap keys, pad;
    pc_keymap_build_keys(&keys, s->key_bind, key_lookup);
    pc_keymap_build_pad(&pad, s->pad_bind, button_lookup, axis_lookup);
    controls_legend_build(l, &keys, &pad, key_name, button_name, axis_name);
}

int test_controls_legend(void) {
    static PcSettings s;
    static ControlsLegend l;
    static char text[8192];
    char lab[64];
    int i, seen = 0;

    /* the defaults: every PS1 button, each once, face buttons first */
    pc_settings_defaults(&s);
    legend_of(&s, &l);
    CHECK(l.n == PC_BTN_COUNT);
    for (i = 0; i < l.n; i++) seen |= 1 << l.row[i].button;
    CHECK(seen == (1 << PC_BTN_COUNT) - 1);
    CHECK(l.row[0].button == PC_BTN_CIRCLE && l.row[1].button == PC_BTN_CROSS);
    CHECK(!strcmp(row(&l, PC_BTN_CIRCLE)->keys, "Enter, Space, Keypad Enter"));
    CHECK(!strcmp(row(&l, PC_BTN_CIRCLE)->pad, "B (right)"));
    CHECK(!strcmp(row(&l, PC_BTN_CROSS)->keys, "Backspace, Esc"));
    CHECK(!strcmp(row(&l, PC_BTN_CROSS)->pad, "A (bottom)"));
    CHECK(!strcmp(row(&l, PC_BTN_UP)->keys, "Up arrow"));
    CHECK(!strcmp(row(&l, PC_BTN_UP)->pad, "D-pad up, Left stick up"));
    CHECK(!strcmp(row(&l, PC_BTN_RIGHT)->pad, "D-pad right, Left stick right"));
    CHECK(!strcmp(row(&l, PC_BTN_L2)->pad, "LT"));
    CHECK(!strcmp(row(&l, PC_BTN_R1)->pad, "RB"));
    CHECK(!strcmp(row(&l, PC_BTN_START)->keys, "F1, S"));
    CHECK(!strcmp(row(&l, PC_BTN_SELECT)->keys, "Tab"));
    CHECK(!strcmp(row(&l, PC_BTN_SELECT)->pad, "Back"));
    CHECK(!strcmp(row(&l, PC_BTN_SQUARE)->pad, "X (left)"));
    CHECK(!strcmp(row(&l, PC_BTN_TRIANGLE)->pad, "Y (top)"));
    controls_legend_text(&l, text, sizeof text);
    CHECK(strstr(text, "Circle (confirm) | Enter, Space, Keypad Enter | B (right)\n") == text);
    CHECK(strstr(text, "SELECT | Tab | Back\n"));

    /* confirm_south swaps the controller's face defaults, not the keys */
    pc_settings_defaults(&s);
    CHECK(pc_settings_parse(&s, "[input]\ncontroller_layout = confirm_south\n", "t") == 0);
    legend_of(&s, &l);
    CHECK(!strcmp(row(&l, PC_BTN_CIRCLE)->pad, "A (bottom)"));
    CHECK(!strcmp(row(&l, PC_BTN_CROSS)->pad, "B (right)"));
    CHECK(!strcmp(row(&l, PC_BTN_CIRCLE)->keys, "Enter, Space, Keypad Enter"));
    /* ... and an explicit [controller] line wins over the layout */
    pc_settings_defaults(&s);
    CHECK(pc_settings_parse(&s, "[input]\ncontroller_layout = confirm_south\n"
                                "[controller]\ncircle = y\n", "t") == 0);
    legend_of(&s, &l);
    CHECK(!strcmp(row(&l, PC_BTN_CIRCLE)->pad, "Y (top)"));
    CHECK(!strcmp(row(&l, PC_BTN_CROSS)->pad, "B (right)"));

    /* remaps, unbinding, an unknown name (never reaches the keymap, so not
     * listed), the same key twice (listed once), a key on two buttons */
    pc_settings_defaults(&s);
    CHECK(pc_settings_parse(&s, "[keyboard]\ncircle = J, NoSuchKey, J\nsquare = none\n"
                                "triangle = K, Q\n"
                                "[controller]\nl2 = none\nr2 = +righttrigger, rightstick\n",
                            "t") == 0);
    legend_of(&s, &l);
    CHECK(!strcmp(row(&l, PC_BTN_CIRCLE)->keys, "J"));
    CHECK(!strcmp(row(&l, PC_BTN_SQUARE)->keys, "-"));
    CHECK(!strcmp(row(&l, PC_BTN_TRIANGLE)->keys, "K, Q"));
    CHECK(!strcmp(row(&l, PC_BTN_L2)->pad, "-"));
    CHECK(!strcmp(row(&l, PC_BTN_R2)->pad, "RT, Right stick press"));

    /* no keymaps (controllers off): "-" */
    controls_legend_build(&l, NULL, NULL, key_name, button_name, axis_name);
    CHECK(!strcmp(row(&l, PC_BTN_CIRCLE)->keys, "-") && !strcmp(row(&l, PC_BTN_CIRCLE)->pad, "-"));

    /* labels */
    CHECK(!strcmp(controls_key_label("Return"), "Enter"));
    CHECK(!strcmp(controls_key_label("Left Shift"), "Left Shift"));
    controls_pad_label("lefty", 1, lab, sizeof lab);
    CHECK(!strcmp(lab, "Left stick down"));
    controls_pad_label("weird", -1, lab, sizeof lab);
    CHECK(!strcmp(lab, "-weird"));

    /* per family: the same POSITIONS, named as printed on that pad */
    pc_settings_defaults(&s);
    {
        static PcKeymap keys, pad;
        pc_keymap_build_keys(&keys, s.key_bind, key_lookup);
        pc_keymap_build_pad(&pad, s.pad_bind, button_lookup, axis_lookup);
        controls_legend_build_family(&l, &keys, &pad, key_name, button_name, axis_name,
                                     PAD_FAMILY_PLAYSTATION);
        CHECK(!strcmp(row(&l, PC_BTN_CIRCLE)->pad, "Circle"));
        CHECK(!strcmp(row(&l, PC_BTN_CROSS)->pad, "Cross"));
        CHECK(!strcmp(row(&l, PC_BTN_SQUARE)->pad, "Square"));
        CHECK(!strcmp(row(&l, PC_BTN_TRIANGLE)->pad, "Triangle"));
        CHECK(!strcmp(row(&l, PC_BTN_L1)->pad, "L1"));
        CHECK(!strcmp(row(&l, PC_BTN_L2)->pad, "L2"));
        CHECK(!strcmp(row(&l, PC_BTN_START)->pad, "Options"));
        CHECK(!strcmp(row(&l, PC_BTN_SELECT)->pad, "Share"));
        CHECK(!strcmp(row(&l, PC_BTN_UP)->pad, "D-pad up, Left stick up"));
        controls_legend_build_family(&l, &keys, &pad, key_name, button_name, axis_name,
                                     PAD_FAMILY_NINTENDO);
        /* by position: confirm (circle) is the right button, Nintendo's A */
        CHECK(!strcmp(row(&l, PC_BTN_CIRCLE)->pad, "A (right)"));
        CHECK(!strcmp(row(&l, PC_BTN_CROSS)->pad, "B (bottom)"));
        CHECK(!strcmp(row(&l, PC_BTN_SQUARE)->pad, "Y (left)"));
        CHECK(!strcmp(row(&l, PC_BTN_TRIANGLE)->pad, "X (top)"));
        CHECK(!strcmp(row(&l, PC_BTN_R1)->pad, "R"));
        CHECK(!strcmp(row(&l, PC_BTN_R2)->pad, "ZR"));
        CHECK(!strcmp(row(&l, PC_BTN_START)->pad, "+"));
        CHECK(!strcmp(row(&l, PC_BTN_SELECT)->pad, "-"));
        controls_legend_build_family(&l, &keys, &pad, key_name, button_name, axis_name,
                                     PAD_FAMILY_GENERIC);
        /* the default joystick mapping's numbers, 1-based as printed */
        CHECK(!strcmp(row(&l, PC_BTN_TRIANGLE)->pad, "Button 1"));
        CHECK(!strcmp(row(&l, PC_BTN_CIRCLE)->pad, "Button 2"));
        CHECK(!strcmp(row(&l, PC_BTN_CROSS)->pad, "Button 3"));
        CHECK(!strcmp(row(&l, PC_BTN_SQUARE)->pad, "Button 4"));
        CHECK(!strcmp(row(&l, PC_BTN_L2)->pad, "Button 7"));
        CHECK(!strcmp(row(&l, PC_BTN_SELECT)->pad, "Button 9"));
        CHECK(!strcmp(row(&l, PC_BTN_START)->pad, "Button 10"));
        CHECK(!strcmp(row(&l, PC_BTN_LEFT)->pad, "D-pad left, Stick left"));
        /* Xbox is the plain builder's family */
        controls_legend_build_family(&l, &keys, &pad, key_name, button_name, axis_name,
                                     PAD_FAMILY_XBOX);
        CHECK(!strcmp(row(&l, PC_BTN_CIRCLE)->pad, "B (right)"));
        CHECK(!strcmp(row(&l, PC_BTN_R2)->pad, "RT"));
    }
    controls_pad_label_family("lefttrigger", -1, PAD_FAMILY_PLAYSTATION, lab, sizeof lab);
    CHECK(!strcmp(lab, "L2 (released)"));
    controls_pad_label_family("guide", 0, PAD_FAMILY_PLAYSTATION, lab, sizeof lab);
    CHECK(!strcmp(lab, "PS button"));
    controls_pad_label_family("touchpad", 0, PAD_FAMILY_NINTENDO, lab, sizeof lab);
    CHECK(!strcmp(lab, "Touchpad"));            /* not on the family's table: Xbox's */
    controls_pad_label_family("paddle1", 0, PAD_FAMILY_GENERIC, lab, sizeof lab);
    CHECK(!strcmp(lab, "paddle1"));
    CHECK(!strcmp(controls_family_name(PAD_FAMILY_NINTENDO), "Nintendo"));

    /* the hotkey: Ctrl+H by default, settable, and no default key clashes
     * with it (every hotkey token and every keyboard binding, case-blind) */
    pc_settings_defaults(&s);
    CHECK(!strcmp(pc_hotkey_names[PC_HK_CONTROLS], "controls"));
    CHECK(!strcmp(s.hotkey[PC_HK_CONTROLS], "Ctrl+H"));
    {
        char tok[8][64], all[256][64];
        int n = 0, j, k, m;
        for (i = 0; i < PC_HK_COUNT + PC_BTN_COUNT; i++) {
            m = pc_bind_tokens(i < PC_HK_COUNT ? s.hotkey[i] : s.key_bind[i - PC_HK_COUNT], tok, 8);
            for (j = 0; j < m; j++) {
                for (k = 0; k < n; k++) CHECK(strcasecmp(all[k], tok[j]));
                snprintf(all[n++], sizeof all[0], "%s", tok[j]);
            }
        }
    }
    CHECK(pc_settings_set(&s, "hotkeys.controls", "F2") == 0);
    CHECK(!strcmp(s.hotkey[PC_HK_CONTROLS], "F2"));
    return 0;
}
