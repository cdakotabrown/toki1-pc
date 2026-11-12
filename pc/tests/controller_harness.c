/* SPDX-License-Identifier: GPL-3.0-or-later */
/* controller_harness.c -- the input HAL's controller side (pc/hal/pad.c)
 * against SDL's virtual joysticks (SDL_JoystickAttachVirtualEx, SDL >=
 * 2.24): no hardware, no window, no game.  pc/tools/test_controller_input.py
 * builds it with the real pad.c / pad_device.c / pad_map.c / settings.c and
 * runs each mode; every case prints "ok NAME" or "FAIL NAME: why".
 *
 *   controller_harness basic       the hints, an unmapped joystick on the
 *                                  default mapping, stick hysteresis, unplug
 *                                  mid-press and plug in again, a mapped
 *                                  PlayStation pad, the stick as menu
 *                                  presses, the hold-off
 *   controller_harness port2       TOKI_PORT2=1: player 1 / player 2, port 2's
 *                                  record, player 1 leaving
 *   controller_harness db          a gamecontrollerdb.txt in the settings
 *                                  folder maps the joystick instead
 *   controller_harness nofallback  TOKI_JOYSTICK_FALLBACK=0: it is ignored
 */
#include "types.h"
#include "pad_device.h"
#include "pad_map.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the game state pad.c reads (PS1 RAM in the game) ---- */
u8 pad_type = 1, menu_mode, now_step, csr_speed = 3, mouse_btn_mode, card_busy;
s16 cursor_cw_x, cursor_cw_y, mouse_x, mouse_y;
u32 pad_data, pad_trig;
u8 pad1_buf[8], pad2_buf[8];

/* ---- HAL neighbours pad.c calls ---- */
SDL_Renderer *hal_get_renderer(void) { return NULL; }
void hal_request_quit(void) {}
int trace_input_active(void) { return 0; }
u32 trace_input_mask_pc(void) { return 0; }
int present_window_to_frame(double x, double y, int ww, int wh, int ow, int oh, int mode,
                            double *fx, double *fy) {
    (void)x; (void)y; (void)ww; (void)wh; (void)ow; (void)oh; (void)mode;
    *fx = *fy = 0;
    return 0;
}

/* ---- pad.c ---- */
void pad_sdl_hints(void);
void pad_handle_event(const SDL_Event *e);
void pad_update_buffers(void);
void pad_suppress_until_release(void);
int pad_stick_menu_button(const SDL_Event *e);
int pad_controller_family(char *name, size_t n);
void pad_live_bindings(const void **keys, const void **pad, int *controllers_on, int *connected);

static int g_fail;
#define CHECK(name, cond) do { if (!(cond)) { \
    printf("FAIL %s: %s (line %d)\n", name, #cond, __LINE__); g_fail = 1; return; } } while (0)

/* PS1 buttons (libpad u16, pressed = 1) */
#define B_L2 0x0001
#define B_R1 0x0008
#define B_TRI 0x0010
#define B_CIRCLE 0x0020
#define B_CROSS 0x0040
#define B_SQUARE 0x0080
#define B_SELECT 0x0100
#define B_START 0x0800
#define B_UP 0x1000
#define B_RIGHT 0x2000
#define B_DOWN 0x4000
#define B_LEFT 0x8000
#define EMPTY 0xFFFFFFFFu   /* nothing on the port */

static void pump(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) pad_handle_event(&e);
}

static unsigned port_buttons(const u8 *r) {
    if (r[0] != 0) return EMPTY;
    return (unsigned)(((u8)~r[2] << 8) | (u8)~r[3]);
}

/* one VBLANK poll; the buttons port 1 (and port 2) report */
static unsigned g_port2;
static unsigned poll1(void) {
    pump();
    pad_update_buffers();
    g_port2 = port_buttons(pad2_buf);
    return port_buttons(pad1_buf);
}

static int connected(void) {
    int n = 0;
    pad_live_bindings(NULL, NULL, NULL, &n);
    return n;
}

/* A joystick SDL has no mapping for: a PlayStation-style USB clone.
 * Returns it opened (the test's own handle, to drive it). */
static SDL_Joystick *attach_unknown(void) {
    SDL_VirtualJoystickDesc d;
    int idx;
    memset(&d, 0, sizeof d);
    d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    d.type = SDL_JOYSTICK_TYPE_UNKNOWN;
    d.naxes = 4;
    d.nbuttons = 12;
    d.nhats = 1;
    d.vendor_id = 0x1234;
    d.product_id = 0x5678;
    d.name = "Cheap, USB Gamepad";
    idx = SDL_JoystickAttachVirtualEx(&d);
    if (idx < 0 || SDL_IsGameController(idx)) return NULL;  /* SDL must not know it */
    return SDL_JoystickOpen(idx);
}

/* A mapped pad: SDL maps a virtual GAMECONTROLLER-type device itself
 * (buttons in SDL_GameControllerButton order, axes in ..Axis order). */
static SDL_Joystick *attach_pad(Uint16 vendor, Uint16 product, const char *name) {
    SDL_VirtualJoystickDesc d;
    int idx;
    memset(&d, 0, sizeof d);
    d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    d.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    d.naxes = SDL_CONTROLLER_AXIS_MAX;
    d.nbuttons = SDL_CONTROLLER_BUTTON_DPAD_RIGHT + 1;
    d.vendor_id = vendor;
    d.product_id = product;
    d.name = name;
    idx = SDL_JoystickAttachVirtualEx(&d);
    if (idx < 0) return NULL;
    return SDL_JoystickOpen(idx);
}

static void detach(SDL_Joystick *j) {
    SDL_JoystickID id = SDL_JoystickInstanceID(j);
    int i, n = SDL_NumJoysticks();
    SDL_JoystickClose(j);
    for (i = 0; i < n; i++)
        if (SDL_JoystickGetDeviceInstanceID(i) == id) SDL_JoystickDetachVirtual(i);
}

static void button(SDL_Joystick *j, int b, int down) {
    SDL_JoystickSetVirtualButton(j, b, down ? SDL_PRESSED : SDL_RELEASED);
}

/* ------------------------------------------------------------------ */

static void case_hints(void) {
    const char *v = SDL_GetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS);
    const char *env = getenv("SDL_GAMECONTROLLER_USE_BUTTON_LABELS");
    CHECK("hints", v && !strcmp(v, env ? env : "0"));   /* the environment wins */
    v = SDL_GetHint(SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS);
    CHECK("hints", v && !strcmp(v, "1"));
    printf("ok hints\n");
}

static void case_fallback(void) {
    SDL_Joystick *j = attach_unknown();
    char name[96];
    CHECK("fallback", j != NULL);
    pump();
    CHECK("fallback", connected() == 1);
    CHECK("fallback", pad_controller_family(name, sizeof name) == PAD_FAMILY_GENERIC);
    CHECK("fallback", !strcmp(name, "Cheap  USB Gamepad"));
    CHECK("fallback", poll1() == 0);
    button(j, 1, 1);                                   /* button 2: circle */
    CHECK("fallback", poll1() == B_CIRCLE);
    button(j, 1, 0);
    button(j, 2, 1);                                   /* button 3: cross */
    CHECK("fallback", poll1() == B_CROSS);
    button(j, 2, 0);
    button(j, 0, 1);                                   /* 1 triangle, 4 square */
    button(j, 3, 1);
    CHECK("fallback", poll1() == (B_TRI | B_SQUARE));
    button(j, 0, 0);
    button(j, 3, 0);
    button(j, 6, 1);                                   /* 7 L2 (a button), 10 start */
    button(j, 9, 1);
    CHECK("fallback", poll1() == (B_L2 | B_START));
    button(j, 6, 0);
    button(j, 9, 0);
    button(j, 8, 1);                                   /* 9 select */
    CHECK("fallback", poll1() == B_SELECT);
    button(j, 8, 0);
    SDL_JoystickSetVirtualHat(j, 0, SDL_HAT_UP);       /* the hat is the d-pad */
    CHECK("fallback", poll1() == B_UP);
    SDL_JoystickSetVirtualHat(j, 0, SDL_HAT_RIGHTDOWN);
    CHECK("fallback", poll1() == (B_RIGHT | B_DOWN));
    SDL_JoystickSetVirtualHat(j, 0, SDL_HAT_CENTERED);
    SDL_JoystickSetVirtualAxis(j, 0, -32768);          /* and so is the stick */
    CHECK("fallback", poll1() == B_LEFT);
    SDL_JoystickSetVirtualAxis(j, 0, 0);
    CHECK("fallback", poll1() == 0);
    printf("ok fallback\n");

    /* the stick presses past 16000 and lets go under 12000 */
    SDL_JoystickSetVirtualAxis(j, 1, 15000);
    CHECK("hysteresis", poll1() == 0);
    SDL_JoystickSetVirtualAxis(j, 1, 16500);
    CHECK("hysteresis", poll1() == B_DOWN);
    SDL_JoystickSetVirtualAxis(j, 1, 13000);
    CHECK("hysteresis", poll1() == B_DOWN);            /* held: no flicker */
    SDL_JoystickSetVirtualAxis(j, 1, 11000);
    CHECK("hysteresis", poll1() == 0);
    SDL_JoystickSetVirtualAxis(j, 1, 13000);
    CHECK("hysteresis", poll1() == 0);                 /* pressed again only past 16000 */
    SDL_JoystickSetVirtualAxis(j, 1, 0);
    printf("ok hysteresis\n");

    /* unplugged with a button down: the button lets go, the slot is free */
    button(j, 1, 1);
    CHECK("unplug", poll1() == B_CIRCLE);
    detach(j);
    CHECK("unplug", poll1() == 0);
    CHECK("unplug", connected() == 0);
    CHECK("unplug", pad_controller_family(NULL, 0) == PAD_FAMILY_XBOX);
    printf("ok unplug\n");

    /* plugged in again mid-game: found by the hot-plug event, still on the
     * default mapping (SDL now has it, from the first time) */
    j = SDL_JoystickOpen(SDL_JoystickAttachVirtualEx(&(SDL_VirtualJoystickDesc){
        .version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION, .type = SDL_JOYSTICK_TYPE_UNKNOWN,
        .naxes = 4, .nbuttons = 12, .nhats = 1, .vendor_id = 0x1234, .product_id = 0x5678,
        .name = "Cheap, USB Gamepad" }));
    CHECK("replug", j != NULL);
    CHECK("replug", poll1() == 0);
    CHECK("replug", connected() == 1);
    CHECK("replug", pad_controller_family(NULL, 0) == PAD_FAMILY_GENERIC);
    button(j, 2, 1);
    CHECK("replug", poll1() == B_CROSS);
    button(j, 2, 0);
    poll1();
    detach(j);
    poll1();
    printf("ok replug\n");
}

/* a device whose axis 1 rests at one end (a wheel's pedal): no stuck d-pad */
static void case_rest_extreme(void) {
    SDL_Joystick *j = SDL_JoystickOpen(SDL_JoystickAttachVirtualEx(&(SDL_VirtualJoystickDesc){
        .version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION, .type = SDL_JOYSTICK_TYPE_UNKNOWN,
        .naxes = 3, .nbuttons = 8, .vendor_id = 0x1234, .product_id = 0x9999,
        .name = "Wheel" }));
    CHECK("rest-extreme", j != NULL);
    SDL_JoystickSetVirtualAxis(j, 1, -32768);          /* before the HAL sees it */
    CHECK("rest-extreme", poll1() == 0);
    CHECK("rest-extreme", connected() == 1);
    CHECK("rest-extreme", poll1() == 0);
    SDL_JoystickSetVirtualAxis(j, 0, 30000);           /* axis 0 rested at the centre */
    CHECK("rest-extreme", poll1() == B_RIGHT);
    SDL_JoystickSetVirtualAxis(j, 0, 0);
    SDL_JoystickSetVirtualAxis(j, 1, 0);               /* through the centre once ... */
    CHECK("rest-extreme", poll1() == 0);
    SDL_JoystickSetVirtualAxis(j, 1, -32768);          /* ... then it counts */
    CHECK("rest-extreme", poll1() == B_UP);
    SDL_JoystickSetVirtualAxis(j, 1, 0);
    poll1();
    detach(j);
    poll1();
    printf("ok rest-extreme\n");
}

static void case_playstation(void) {
    SDL_Joystick *j = attach_pad(0x054C, 0x09CC, "PS4 Controller");   /* DualShock 4 v2 */
    SDL_Event e;
    SDL_JoystickID id;
    CHECK("playstation", j != NULL);
    id = SDL_JoystickInstanceID(j);
    CHECK("playstation", poll1() == 0);
    CHECK("playstation", connected() == 1);
    CHECK("playstation", pad_controller_family(NULL, 0) == PAD_FAMILY_PLAYSTATION);
    button(j, SDL_CONTROLLER_BUTTON_B, 1);             /* right (circle) confirms */
    CHECK("playstation", poll1() == B_CIRCLE);
    button(j, SDL_CONTROLLER_BUTTON_B, 0);
    button(j, SDL_CONTROLLER_BUTTON_A, 1);             /* bottom (cross) */
    CHECK("playstation", poll1() == B_CROSS);
    button(j, SDL_CONTROLLER_BUTTON_A, 0);
    SDL_JoystickSetVirtualAxis(j, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 30000);   /* L2 */
    SDL_JoystickSetVirtualAxis(j, SDL_CONTROLLER_AXIS_LEFTY, -30000);        /* stick up */
    CHECK("playstation", poll1() == (B_L2 | B_UP));
    SDL_JoystickSetVirtualAxis(j, SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
    SDL_JoystickSetVirtualAxis(j, SDL_CONTROLLER_AXIS_LEFTY, 0);
    CHECK("playstation", poll1() == 0);
    printf("ok playstation\n");

    /* the left stick as menu presses: one per push */
    memset(&e, 0, sizeof e);
    e.type = SDL_CONTROLLERAXISMOTION;
    e.caxis.which = id;
    e.caxis.axis = SDL_CONTROLLER_AXIS_LEFTY;
    e.caxis.value = 20000;
    CHECK("menu-stick", pad_stick_menu_button(&e) == SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    e.caxis.value = 30000;
    CHECK("menu-stick", pad_stick_menu_button(&e) == -1);     /* still the same push */
    e.caxis.value = 13000;
    CHECK("menu-stick", pad_stick_menu_button(&e) == -1);
    e.caxis.value = 2000;
    CHECK("menu-stick", pad_stick_menu_button(&e) == -1);     /* back to centre */
    e.caxis.value = -17000;
    CHECK("menu-stick", pad_stick_menu_button(&e) == SDL_CONTROLLER_BUTTON_DPAD_UP);
    e.caxis.value = 0;
    pad_stick_menu_button(&e);
    e.caxis.axis = SDL_CONTROLLER_AXIS_LEFTX;
    e.caxis.value = 32767;
    CHECK("menu-stick", pad_stick_menu_button(&e) == SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    e.caxis.axis = SDL_CONTROLLER_AXIS_RIGHTX;                 /* not the left stick */
    CHECK("menu-stick", pad_stick_menu_button(&e) == -1);
    e.caxis.which = id + 1000;                                 /* not a controller */
    e.caxis.axis = SDL_CONTROLLER_AXIS_LEFTY;
    CHECK("menu-stick", pad_stick_menu_button(&e) == -1);
    printf("ok menu-stick\n");

    /* after the settings menu: what closed it waits for its release */
    button(j, SDL_CONTROLLER_BUTTON_B, 1);
    pad_suppress_until_release();
    CHECK("hold-off", poll1() == 0);
    CHECK("hold-off", poll1() == 0);
    button(j, SDL_CONTROLLER_BUTTON_B, 0);
    CHECK("hold-off", poll1() == 0);
    button(j, SDL_CONTROLLER_BUTTON_B, 1);
    CHECK("hold-off", poll1() == B_CIRCLE);
    button(j, SDL_CONTROLLER_BUTTON_B, 0);
    poll1();
    printf("ok hold-off\n");
    detach(j);
    poll1();
}

static void case_port2(void) {
    SDL_Joystick *a = attach_pad(0x045E, 0x0B12, "Xbox Series X Controller");
    SDL_Joystick *b;
    CHECK("port2", a != NULL);
    CHECK("port2", poll1() == 0);
    CHECK("port2", g_port2 == EMPTY);                  /* one controller: port 2 empty */
    b = attach_pad(0x057E, 0x2009, "Nintendo Switch Pro Controller");
    CHECK("port2", b != NULL);
    CHECK("port2", poll1() == 0);
    CHECK("port2", connected() == 2);
    CHECK("port2", g_port2 == 0);                      /* a pad, nothing pressed */
    CHECK("port2", SDL_JoystickGetPlayerIndex(a) == 0 && SDL_JoystickGetPlayerIndex(b) == 1);
    CHECK("port2", pad_controller_family(NULL, 0) == PAD_FAMILY_XBOX);   /* player 1's */
    button(b, SDL_CONTROLLER_BUTTON_B, 1);
    CHECK("port2", poll1() == 0 && g_port2 == B_CIRCLE);
    button(a, SDL_CONTROLLER_BUTTON_A, 1);
    CHECK("port2", poll1() == B_CROSS && g_port2 == B_CIRCLE);
    button(a, SDL_CONTROLLER_BUTTON_A, 0);
    button(b, SDL_CONTROLLER_BUTTON_B, 0);
    CHECK("port2", poll1() == 0 && g_port2 == 0);
    printf("ok port2\n");

    /* the menu's hold-off covers port 2 too */
    button(b, SDL_CONTROLLER_BUTTON_START, 1);
    pad_suppress_until_release();
    CHECK("port2-hold-off", poll1() == 0 && g_port2 == 0);
    button(b, SDL_CONTROLLER_BUTTON_START, 0);
    CHECK("port2-hold-off", poll1() == 0 && g_port2 == 0);
    button(b, SDL_CONTROLLER_BUTTON_START, 1);
    CHECK("port2-hold-off", poll1() == 0 && g_port2 == B_START);
    button(b, SDL_CONTROLLER_BUTTON_START, 0);
    poll1();
    printf("ok port2-hold-off\n");

    /* player 1 leaves: player 2 becomes player 1, port 2 is empty */
    detach(a);
    CHECK("port2-leave", poll1() == 0 && g_port2 == EMPTY);
    CHECK("port2-leave", SDL_JoystickGetPlayerIndex(b) == 0);
    CHECK("port2-leave", pad_controller_family(NULL, 0) == PAD_FAMILY_NINTENDO);
    button(b, SDL_CONTROLLER_BUTTON_B, 1);
    CHECK("port2-leave", poll1() == B_CIRCLE && g_port2 == EMPTY);
    button(b, SDL_CONTROLLER_BUTTON_B, 0);
    poll1();
    /* the one plugged in again is player 2 now */
    a = attach_pad(0x045E, 0x0B12, "Xbox Series X Controller");
    CHECK("port2-leave", poll1() == 0 && g_port2 == 0);
    button(a, SDL_CONTROLLER_BUTTON_Y, 1);
    CHECK("port2-leave", poll1() == 0 && g_port2 == B_TRI);
    button(a, SDL_CONTROLLER_BUTTON_Y, 0);
    poll1();
    detach(a);
    detach(b);
    poll1();
    printf("ok port2-leave\n");
}

/* gamecontrollerdb.txt names the joystick: its line, not the default */
static void case_db(void) {
    SDL_Joystick *j = NULL;
    SDL_VirtualJoystickDesc d;
    int idx;
    memset(&d, 0, sizeof d);
    d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    d.type = SDL_JOYSTICK_TYPE_UNKNOWN;
    d.naxes = 4;
    d.nbuttons = 12;
    d.nhats = 1;
    d.vendor_id = 0x1234;
    d.product_id = 0x5678;
    d.name = "Cheap, USB Gamepad";
    idx = SDL_JoystickAttachVirtualEx(&d);
    CHECK("db", idx >= 0);
    CHECK("db", SDL_IsGameController(idx));           /* the file's line */
    j = SDL_JoystickOpen(idx);
    CHECK("db", poll1() == 0);
    CHECK("db", connected() == 1);
    CHECK("db", pad_controller_family(NULL, 0) != PAD_FAMILY_GENERIC);
    button(j, 5, 1);                                   /* the file says b:b5 */
    CHECK("db", poll1() == B_CIRCLE);
    button(j, 5, 0);
    button(j, 1, 1);                                   /* ... and rightshoulder:b1 */
    CHECK("db", poll1() == B_R1);
    button(j, 1, 0);
    poll1();
    detach(j);
    poll1();
    printf("ok db\n");
}

static void case_nofallback(void) {
    SDL_Joystick *j = attach_unknown();
    CHECK("nofallback", j != NULL);
    button(j, 1, 1);
    CHECK("nofallback", poll1() == 0);
    CHECK("nofallback", connected() == 0);
    button(j, 1, 0);
    detach(j);
    poll1();
    /* a mapped pad still works */
    j = attach_pad(0x045E, 0x0B12, "Xbox Series X Controller");
    button(j, SDL_CONTROLLER_BUTTON_B, 1);
    CHECK("nofallback", poll1() == B_CIRCLE && connected() == 1);
    detach(j);
    poll1();
    printf("ok nofallback\n");
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "basic";
    pad_sdl_hints();
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) < 0) {
        printf("FAIL init: %s\n", SDL_GetError());
        return 1;
    }
    if (SDL_NumJoysticks() != 0) {
        /* a real device would take player 1 */
        printf("skip: %d joysticks connected\n", SDL_NumJoysticks());
        return 0;
    }
    if (!strcmp(mode, "guid")) {              /* the clone's GUID, for a db line */
        char guid[40];
        int idx = SDL_JoystickAttachVirtualEx(&(SDL_VirtualJoystickDesc){
            .version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION, .type = SDL_JOYSTICK_TYPE_UNKNOWN,
            .naxes = 4, .nbuttons = 12, .nhats = 1, .vendor_id = 0x1234,
            .product_id = 0x5678, .name = "Cheap, USB Gamepad" });
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(idx), guid, sizeof guid);
        printf("guid %s\n", guid);
        SDL_Quit();
        return 0;
    }
    poll1();                                  /* pad.c reads its settings */
    if (!strcmp(mode, "basic")) {
        case_hints();
        case_fallback();
        case_rest_extreme();
        case_playstation();
    } else if (!strcmp(mode, "port2")) {
        case_port2();
    } else if (!strcmp(mode, "db")) {
        case_db();
    } else if (!strcmp(mode, "nofallback")) {
        case_nofallback();
    } else {
        printf("FAIL usage: %s\n", mode);
        return 2;
    }
    SDL_Quit();
    return g_fail;
}
