/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_pad_map.c -- the default mapping pc/hal/pad.c gives a joystick SDL
 * has no mapping for (pc/hal/pad_map.c): the line's fields for a full
 * PlayStation-style clone, a small pad, an arcade stick without a hat, a
 * name with commas, and the refusals.  No SDL (pc/tools/
 * test_controller_input.py feeds the line to SDL with virtual joysticks). */
#include <stdio.h>
#include <string.h>

#include "pad_map.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "  %s:%d: %s\n", __FILE__, __LINE__, #c); return __LINE__; } } while (0)

static const char *GUID = "03000000790000000600000000000000";

int test_pad_map(void) {
    char line[1024];
    int n;

    /* the PS-style clone: 12 buttons, 4 axes, one hat */
    n = pad_fallback_mapping(line, sizeof line, GUID, "Generic   USB  Joystick  ", 12, 4, 1);
    CHECK(n > 0 && n == (int)strlen(line));
    CHECK(!strncmp(line, "03000000790000000600000000000000,Generic   USB  Joystick  ,", 59));
    CHECK(strstr(line, ",y:b0,b:b1,a:b2,x:b3,"));
    CHECK(strstr(line, ",leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,righttrigger:b7,"));
    CHECK(strstr(line, ",back:b8,start:b9,leftstick:b10,rightstick:b11,"));
    CHECK(!strstr(line, "guide"));                          /* button 13 is not there */
    CHECK(strstr(line, ",leftx:a0,lefty:a1,rightx:a2,righty:a3,"));
    CHECK(strstr(line, ",dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,"));
    CHECK(line[n - 1] == ',');

    /* 13 buttons: home is the guide (the settings-menu button) */
    n = pad_fallback_mapping(line, sizeof line, GUID, "Pad", 13, 2, 0);
    CHECK(n > 0 && strstr(line, ",guide:b12,"));
    CHECK(!strstr(line, "rightx") && !strstr(line, "dpup"));

    /* an arcade stick: 8 buttons, the lever on axes 0/1, no hat */
    n = pad_fallback_mapping(line, sizeof line, GUID, "Arcade, Stick", 8, 2, 0);
    CHECK(n > 0);
    CHECK(strstr(line, ",Arcade  Stick,"));                 /* the comma would end the field */
    CHECK(strstr(line, ",righttrigger:b7,") && !strstr(line, "back:") && !strstr(line, "start:"));
    CHECK(strstr(line, ",leftx:a0,lefty:a1,"));

    /* two buttons, nothing else; no name */
    n = pad_fallback_mapping(line, sizeof line, GUID, NULL, 2, 0, 0);
    CHECK(n > 0 && !strcmp(line, "03000000790000000600000000000000,Generic joystick,y:b0,b:b1,"));

    /* refusals: no button, no GUID, no room */
    CHECK(pad_fallback_mapping(line, sizeof line, GUID, "x", 0, 2, 1) == -1);
    CHECK(pad_fallback_mapping(line, sizeof line, "", "x", 4, 2, 1) == -1);
    CHECK(pad_fallback_mapping(line, 40, GUID, "x", 12, 4, 1) == -1);
    CHECK(pad_fallback_mapping(line, 36, GUID, "a long name", 2, 0, 0) == -1);

    /* the numbers the generic labels use */
    CHECK(pad_fallback_button("y") == 0 && pad_fallback_button("b") == 1);
    CHECK(pad_fallback_button("a") == 2 && pad_fallback_button("x") == 3);
    CHECK(pad_fallback_button("start") == 9 && pad_fallback_button("guide") == 12);
    CHECK(pad_fallback_button("dpup") == -1 && pad_fallback_button(NULL) == -1);
    return 0;
}
