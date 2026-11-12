/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_settings.c -- the player settings file, bindings and presenter math
 * (pc/hal/settings.c, pc/hal/present_layout.c).  No SDL, no assets. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pc_settings.h"
#include "pc_present.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "  %s:%d: %s\n", __FILE__, __LINE__, #c); return __LINE__; } } while (0)

/* ---- a fake environment ---- */
static const char *g_env[32][2];
static int g_nenv;
static void env_clear(void) { g_nenv = 0; }
static void env_set(const char *k, const char *v) { g_env[g_nenv][0] = k; g_env[g_nenv][1] = v; g_nenv++; }
static const char *fake_getenv(const char *k) {
    int i;
    for (i = 0; i < g_nenv; i++) if (!strcmp(g_env[i][0], k)) return g_env[i][1];
    return NULL;
}

static char g_tmp[256];
static void tmp_cleanup(void) {
    char cmd[300];
    if (strncmp(g_tmp, "/tmp/toki_settings_test_", 24)) return;
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_tmp);
    if (system(cmd)) { }
    g_tmp[0] = 0;
}
static void tmpdir(void) {
    tmp_cleanup();
    snprintf(g_tmp, sizeof(g_tmp), "/tmp/toki_settings_test_XXXXXX");
    if (!mkdtemp(g_tmp)) { perror("mkdtemp"); exit(1); }
}
static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    fputs(text, f);
    fclose(f);
}
static int file_exists(const char *p) { FILE *f = fopen(p, "r"); if (f) fclose(f); return f != NULL; }

int test_settings_defaults(void) {
    PcSettings s;
    pc_settings_defaults(&s);
    CHECK(!strcmp(s.profile, "enhanced"));   /* 2026-11-04: the player default */
    CHECK(s.fullscreen == 0 && s.window_width == 0 && s.remember_window == 1);
    CHECK(s.scale_mode == PC_SCALE_ASPECT && s.filter == PC_FILTER_NEAREST);
    CHECK(s.scanlines == 0 && s.vsync == 0);
    CHECK(s.vol_master == 100 && s.vol_music == 100 && s.vol_sfx == 100 && s.vol_voice == 100);
    /* the input defaults reproduce the pre-settings port exactly */
    CHECK(s.mouse_enabled == 1 && s.mouse_sensitivity == 1.0 && s.mouse_grab == 1);
    CHECK(s.mouse_swap_gap == 1 && s.input_device == PC_DEVICE_AUTO);
    CHECK(s.mouse_mode == PC_MOUSE_ABSOLUTE);   /* 2026-11-01: the hand follows the pointer */
    {
        PcSettings t = s;
        CHECK(pc_settings_set(&t, "input.mouse_mode", "relative") == 0 && t.mouse_mode == PC_MOUSE_RELATIVE);
        CHECK(pc_settings_set(&t, "input.mouse_mode", "sideways") != 0 && t.mouse_mode == PC_MOUSE_RELATIVE);
    }
    /* [graphics] resolution_scale: 1 (off) by default, 1..4 (pc/hal/gpu_hires.h) */
    CHECK(s.resolution_scale == 1);
    {
        PcSettings t = s;
        CHECK(pc_settings_set(&t, "graphics.resolution_scale", "3") == 0 && t.resolution_scale == 3);
        CHECK(pc_settings_set(&t, "graphics.resolution_scale", "8") != 0 && t.resolution_scale == 3);
        CHECK(pc_settings_set(&t, "graphics.resolution_scale", "0") != 0 && t.resolution_scale == 3);
        CHECK(pc_settings_parse(&t, "[graphics]\nresolution_scale = 2\n", "t") == 0 &&
              t.resolution_scale == 2);
    }
    /* [graphics] subpixel: on by default (pc/hal/gpu_pgxp.h; it only acts
     * under the enhanced profile with resolution_scale > 1) */
    CHECK(s.subpixel == 1);
    {
        PcSettings t = s;
        CHECK(pc_settings_set(&t, "graphics.subpixel", "false") == 0 && t.subpixel == 0);
        CHECK(pc_settings_parse(&t, "[graphics]\nsubpixel = true\n", "t") == 0 && t.subpixel == 1);
    }
    /* [audio] music_pack / music_dump (pc/hal/music_pack.c): none, off */
    CHECK(s.music_pack[0] == 0 && s.music_dump == 0);
    {
        PcSettings t = s;
        char v[64];
        CHECK(pc_settings_parse(&t, "[audio]\nmusic_pack = \"My OST\"\nmusic_dump = true\n", "t") == 0 &&
              !strcmp(t.music_pack, "My OST") && t.music_dump == 1);
        CHECK(pc_settings_format(&t, "audio.music_pack", v, sizeof v) == 0 && !strcmp(v, "\"My OST\""));
        /* a name longer than the field is refused and the old one kept */
        CHECK(pc_settings_set(&t, "audio.music_pack",
                              "0123456789012345678901234567890123456789012345678901234567890123") != 0 &&
              !strcmp(t.music_pack, "My OST"));
    }
    CHECK(s.stick_threshold == 16000 && s.trigger_threshold == 8000);
    CHECK(!strcmp(s.pad_bind[PC_BTN_CROSS], "a") && !strcmp(s.pad_bind[PC_BTN_CIRCLE], "b"));
    CHECK(!strcmp(s.pad_bind[PC_BTN_L2], "+lefttrigger"));
    /* keyboard: the game confirms with circle */
    CHECK(strstr(s.key_bind[PC_BTN_CIRCLE], "Return") != NULL);
    CHECK(strstr(s.key_bind[PC_BTN_CROSS], "Escape") != NULL);
    CHECK(!strcmp(s.hotkey[PC_HK_FULLSCREEN], "F11, Alt+Return"));
    CHECK(s.pause_on_focus_loss == 0 && s.toasts == 0 && s.screenshot_scale == 2);
    return 0;
}

int test_settings_parse(void) {
    PcSettings s;
    int w;
    pc_settings_defaults(&s);
    w = pc_settings_parse(&s,
        "# comment\n"
        "profile = \"parity\"\n"
        "[display]\n"
        "fullscreen = yes\n"
        "scale = Integer   # trailing comment\n"
        "filter = 'linear'\n"
        "scanlines = 40\n"
        "  window_width = 1280\n"
        "window_height=960\n"
        "[audio]\n"
        "music = 30\n"
        "voice = 250\n"            /* out of range: warns, kept */
        "bogus = 1\n"              /* unknown: warns */
        "[input]\n"
        "mouse_sensitivity = 0.5\n"
        "device = pad\n"
        "[keyboard]\n"
        "circle = \"K, L\"\n"
        "[system]\n"
        "screenshot_dir = \"/tmp/shots; with semicolon\"\n"
        "save_dir = \"~/toki saves\"\n",
        "test");
    CHECK(w == 2);
    CHECK(!strcmp(s.profile, "parity"));
    CHECK(s.fullscreen == 1 && s.scale_mode == PC_SCALE_INTEGER && s.filter == PC_FILTER_LINEAR);
    CHECK(s.scanlines == 40 && s.window_width == 1280 && s.window_height == 960);
    CHECK(s.vol_music == 30 && s.vol_voice == 100);
    CHECK(s.mouse_sensitivity == 0.5 && s.input_device == PC_DEVICE_PAD);
    CHECK(!strcmp(s.key_bind[PC_BTN_CIRCLE], "K, L"));
    CHECK(!strcmp(s.screenshot_dir, "/tmp/shots; with semicolon"));
    CHECK(!strcmp(s.save_dir, "~/toki saves"));
    return 0;
}

int test_settings_overrides_and_layout(void) {
    PcSettings s;
    pc_settings_defaults(&s);
    pc_settings_parse(&s, "[audio]\nmaster = 50\n[input]\ncontroller_layout = confirm_south\n", "t");
    CHECK(s.vol_master == 50);
    /* the layout swaps only the cross/circle DEFAULTS */
    CHECK(!strcmp(s.pad_bind[PC_BTN_CIRCLE], "a") && !strcmp(s.pad_bind[PC_BTN_CROSS], "b"));
    CHECK(pc_settings_apply_overrides(&s, "audio.master=70; display.fullscreen=true\nprofile=parity") == 0);
    CHECK(s.vol_master == 70 && s.fullscreen == 1 && !strcmp(s.profile, "parity"));
    CHECK(pc_settings_apply_overrides(&s, "nokey;display.scale=round") == 2);
    CHECK(s.scale_mode == PC_SCALE_ASPECT);

    pc_settings_defaults(&s);
    pc_settings_parse(&s, "[controller]\ncircle = y\n[input]\ncontroller_layout = confirm_south\n", "t");
    CHECK(!strcmp(s.pad_bind[PC_BTN_CIRCLE], "y"));    /* explicit wins */
    CHECK(!strcmp(s.pad_bind[PC_BTN_CROSS], "b"));
    return 0;
}

int test_settings_load_create_and_harness(void) {
    PcSettings s, t;
    char path[512], cfg[512];
    tmpdir();
    snprintf(cfg, sizeof(cfg), "%s/cfg", g_tmp);
    snprintf(path, sizeof(path), "%s/settings.ini", cfg);

    /* first interactive run: the file is created with the defaults */
    env_clear();
    env_set("TOKI_CONFIG_DIR", cfg);
    pc_settings_load(&s, fake_getenv, 1);
    CHECK(!s.harness && s.created && file_exists(path));
    CHECK(!strcmp(s.file_path, path));

    /* the written file parses back to the same values, with no warnings */
    pc_settings_defaults(&t);
    {
        FILE *f = fopen(path, "r");
        static char buf[65536];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = 0;
        CHECK(pc_settings_parse(&t, buf, path) == 0);
    }
    CHECK(!memcmp(t.key_bind, s.key_bind, sizeof(s.key_bind)));
    CHECK(!memcmp(t.hotkey, s.hotkey, sizeof(s.hotkey)));
    CHECK(t.mouse_sensitivity == s.mouse_sensitivity && t.scale_mode == s.scale_mode);

    /* a player's file and TOKI_SET apply (TOKI_SET last) */
    write_text(path, "[audio]\nmaster = 20\n[keyboard]\ncross = J\n");
    env_set("TOKI_SET", "audio.master=30");
    pc_settings_load(&s, fake_getenv, 1);
    CHECK(s.vol_master == 30 && !strcmp(s.key_bind[PC_BTN_CROSS], "J") && !s.created);

    /* automated runs ignore the file, TOKI_SETTINGS and TOKI_SET */
    {
        static const char *const harness_vars[] = {
            "PC_TRACE_FRAMES", "PC_INPUT", "PC_TURBO", "PC_TRACE", "TOKI_INPUT_SCRIPT",
            "AUTOPLAY", "SCENE_NAV", "PC_DBG_CMDS", "PC_SHOT_DIR", NULL,
        };
        int i;
        for (i = 0; harness_vars[i]; i++) {
            env_clear();
            env_set("TOKI_CONFIG_DIR", cfg);
            env_set("TOKI_SETTINGS", path);
            env_set("TOKI_SET", "audio.master=5;profile=enhanced");
            env_set(harness_vars[i], "1");
            pc_settings_load(&s, fake_getenv, 1);
            CHECK(s.harness && !strcmp(s.harness_reason, harness_vars[i]));
            CHECK(s.vol_master == 100 && !strcmp(s.profile, "parity"));
            CHECK(!strcmp(s.key_bind[PC_BTN_CROSS], "Backspace, Escape"));
            CHECK(s.file_path[0] == 0);
        }
        /* an automated run has toasts off too; TOKI_TOASTS=1 asks for them */
        env_clear();
        env_set("PC_INPUT", "1");
        pc_settings_load(&s, fake_getenv, 1);
        CHECK(s.harness && s.toasts == 0);
        env_set("TOKI_TOASTS", "1");
        pc_settings_load(&s, fake_getenv, 1);
        CHECK(s.harness && s.toasts == 1);
        env_clear();
        env_set("SDL_VIDEODRIVER", "dummy");
        CHECK(pc_settings_harness_reason(fake_getenv) != NULL);
        env_clear();
        env_set("SDL_VIDEODRIVER", "x11");
        CHECK(pc_settings_harness_reason(fake_getenv) == NULL);
        env_set("TOKI_SETTINGS", "off");
        CHECK(pc_settings_harness_reason(fake_getenv) != NULL);
        env_clear();
        env_set("PC_TRACE_FRAMES", "");        /* empty does not count */
        CHECK(pc_settings_harness_reason(fake_getenv) == NULL);
    }

    /* a harness run never creates a file */
    {
        char other[512], otherfile[600];
        snprintf(other, sizeof(other), "%s/never", g_tmp);
        snprintf(otherfile, sizeof(otherfile), "%s/settings.ini", other);
        env_clear();
        env_set("TOKI_CONFIG_DIR", other);
        env_set("PC_TRACE_FRAMES", "300");
        pc_settings_load(&s, fake_getenv, 1);
        CHECK(!file_exists(otherfile));
    }

    /* XDG paths */
    env_clear();
    env_set("HOME", "/home/p");
    pc_settings_paths(&s, fake_getenv);
    CHECK(!strcmp(s.config_dir, "/home/p/.config/tokimeki-pc"));
    CHECK(!strcmp(s.file_path, "/home/p/.config/tokimeki-pc/settings.ini"));
    CHECK(!strcmp(s.data_dir, "/home/p/.local/share/tokimeki-pc"));
    env_set("XDG_CONFIG_HOME", "/x/cfg");
    pc_settings_paths(&s, fake_getenv);
    CHECK(!strcmp(s.config_dir, "/x/cfg/tokimeki-pc"));
    tmp_cleanup();
    return 0;
}

/* ---- bindings ---- */
static int fake_key(const char *n) {
    static const char *const names[] = { "Return", "Space", "Escape", "J", "K", "Up", NULL };
    int i;
    if (!strcmp(n, "Left Ctrl")) return 224;
    for (i = 0; names[i]; i++) if (!strcmp(names[i], n)) return 10 + i;
    return -1;
}
static int fake_button(const char *n) {
    if (!strcmp(n, "a")) return 0;
    if (!strcmp(n, "b")) return 1;
    if (!strcmp(n, "dpup")) return 11;
    return -1;
}
static int fake_axis(const char *n) {
    if (!strcmp(n, "leftx")) return 0;
    if (!strcmp(n, "lefty")) return 1;
    if (!strcmp(n, "lefttrigger")) return 4;
    return -1;
}

int test_settings_binding_remap(void) {
    PcSettings s;
    PcKeymap km;
    unsigned char state[256];
    char tok[8][64];
    unsigned mods;
    const char *k;

    CHECK(pc_bind_tokens(" Return ,Space,, Keypad Enter ", tok, 8) == 3);
    CHECK(!strcmp(tok[0], "Return") && !strcmp(tok[2], "Keypad Enter"));

    k = pc_hotkey_split("Ctrl+Alt+Return", &mods);
    CHECK(!strcmp(k, "Return") && mods == (PC_MOD_CTRL | PC_MOD_ALT));
    k = pc_hotkey_split("F11", &mods);
    CHECK(!strcmp(k, "F11") && mods == 0);
    k = pc_hotkey_split("Ctrl+-", &mods);
    CHECK(!strcmp(k, "-") && mods == PC_MOD_CTRL);
    k = pc_hotkey_split("Keypad +", &mods);
    CHECK(!strcmp(k, "Keypad +") && mods == 0);
    k = pc_hotkey_split("shift+GUI+P", &mods);
    CHECK(!strcmp(k, "P") && mods == (PC_MOD_SHIFT | PC_MOD_GUI));

    pc_settings_defaults(&s);
    pc_settings_parse(&s, "[keyboard]\ncircle = J, Bogus\ncross = K\nup = Up, Left Ctrl\n"
                          "down = none\n", "t");
    pc_keymap_build_keys(&km, s.key_bind, fake_key);
    CHECK(km.unresolved > 0);              /* Bogus plus the default names the fake lacks */
    memset(state, 0, sizeof(state));
    state[fake_key("J")] = 1;
    CHECK(pc_keymap_keys(&km, state, 256, -1) == PC_PAD_CIRCLE);
    state[fake_key("Return")] = 1;         /* rebound away: no longer circle */
    CHECK(pc_keymap_keys(&km, state, 256, -1) == PC_PAD_CIRCLE);
    state[fake_key("K")] = 1;
    CHECK(pc_keymap_keys(&km, state, 256, -1) == (PC_PAD_CIRCLE | PC_PAD_CROSS));
    /* a Ctrl/Alt chord: only modifier-key bindings still count */
    state[224] = 1;
    CHECK(pc_keymap_keys(&km, state, 256, 224) == PC_PAD_UP);
    CHECK(pc_keymap_keys(&km, state, 256, -1) == (PC_PAD_CIRCLE | PC_PAD_CROSS | PC_PAD_UP));

    pc_keymap_build_pad(&km, s.pad_bind, fake_button, fake_axis);
    {
        int i, saw_trigger = 0, saw_stick = 0, saw_cross = 0;
        for (i = 0; i < km.n; i++) {
            if (km.b[i].mask == PC_PAD_L2 && km.b[i].code == 4 && km.b[i].axis_sign == 1) saw_trigger = 1;
            if (km.b[i].mask == PC_PAD_UP && km.b[i].code == 1 && km.b[i].axis_sign == -1) saw_stick = 1;
            if (km.b[i].mask == PC_PAD_CROSS && km.b[i].code == 0 && !km.b[i].axis_sign) saw_cross = 1;
        }
        CHECK(saw_trigger && saw_stick && saw_cross);
    }
    return 0;
}

/* ---- presenter math ---- */
int test_present_scaling_math(void) {
    PresentRect r;
    int w, h, y, bh, row, prev;
    r = present_layout_rect(640, 480, PC_SCALE_ASPECT);
    CHECK(r.x == 0 && r.y == 0 && r.w == 640 && r.h == 480);
    r = present_layout_rect(1920, 1080, PC_SCALE_ASPECT);     /* pillarbox */
    CHECK(r.h == 1080 && r.w == 1440 && r.x == 240 && r.y == 0);
    r = present_layout_rect(800, 900, PC_SCALE_ASPECT);       /* letterbox */
    CHECK(r.w == 800 && r.h == 600 && r.x == 0 && r.y == 150);
    r = present_layout_rect(1920, 1080, PC_SCALE_INTEGER);
    CHECK(r.w == 1280 && r.h == 960 && r.x == 320 && r.y == 60);
    r = present_layout_rect(1000, 800, PC_SCALE_INTEGER);
    CHECK(r.w == 960 && r.h == 720 && r.x == 20 && r.y == 40);
    r = present_layout_rect(200, 100, PC_SCALE_INTEGER);      /* never below 1x */
    CHECK(r.w == 320 && r.h == 240 && r.x == -60 && r.y == -70);
    r = present_layout_rect(1234, 567, PC_SCALE_STRETCH);
    CHECK(r.x == 0 && r.y == 0 && r.w == 1234 && r.h == 567);

    present_auto_window(1920, 1040, &w, &h);
    CHECK(w == 960 && h == 720);
    present_auto_window(2560, 1400, &w, &h);
    CHECK(w == 1280 && h == 960);
    present_auto_window(1366, 728, &w, &h);
    CHECK(w == 640 && h == 480);
    present_auto_window(300, 200, &w, &h);
    CHECK(w == 320 && h == 240);
    present_auto_window(0, 0, &w, &h);
    CHECK(w == 640 && h == 480);

    /* scanline bands: one per source row, inside the row, monotonic */
    r = present_layout_rect(1440, 1080, PC_SCALE_ASPECT);
    prev = -1;
    for (row = 0; row < PRESENT_SRC_H; row++) {
        present_scanline_band(r, row, &y, &bh);
        CHECK(bh >= 1 && y > prev);
        CHECK(y >= r.y + row * r.h / PRESENT_SRC_H && y + bh <= r.y + (row + 1) * r.h / PRESENT_SRC_H);
        prev = y;
    }
    present_scanline_band(present_layout_rect(960, 720, PC_SCALE_INTEGER), 0, &y, &bh);
    CHECK(y == 2 && bh == 1);       /* 3 px rows: the lower (floor) half */

    /* window -> picture (the absolute mouse) */
    {
        double fx, fy;
        CHECK(present_window_to_frame(0.5, 0.5, 640, 480, 640, 480, PC_SCALE_ASPECT, &fx, &fy) == 1);
        CHECK(fx == 0.25 && fy == 0.25);
        CHECK(present_window_to_frame(320, 240, 640, 480, 640, 480, PC_SCALE_ASPECT, &fx, &fy));
        CHECK(fx == 160 && fy == 120);
        /* 1920x1080 pillarbox: picture x 240..1680 */
        CHECK(present_window_to_frame(240, 0, 1920, 1080, 1920, 1080, PC_SCALE_ASPECT, &fx, &fy) == 1);
        CHECK(fx == 0 && fy == 0);
        CHECK(present_window_to_frame(100, 540, 1920, 1080, 1920, 1080, PC_SCALE_ASPECT, &fx, &fy) == 0);
        CHECK(fx < 0 && fy == 120);                     /* over the left bar */
        CHECK(present_window_to_frame(1679.5, 1079.5, 1920, 1080, 1920, 1080, PC_SCALE_ASPECT, &fx, &fy));
        CHECK(fx > 319.8 && fx < 320 && fy > 239.8 && fy < 240);
        /* integer 3x in 1000x800: picture at (20,40) */
        CHECK(present_window_to_frame(20 + 3 * 100, 40 + 3 * 50, 1000, 800, 1000, 800,
                                      PC_SCALE_INTEGER, &fx, &fy) && fx == 100 && fy == 50);
        /* stretch: independent axes */
        CHECK(present_window_to_frame(500, 150, 1000, 300, 1000, 300, PC_SCALE_STRETCH, &fx, &fy));
        CHECK(fx == 160 && fy == 120);
        /* high-DPI: 640x480 points drawn as 1280x960 pixels */
        CHECK(present_window_to_frame(320, 240, 640, 480, 1280, 960, PC_SCALE_ASPECT, &fx, &fy));
        CHECK(fx == 160 && fy == 120);
    }

    CHECK(present_volume_step(95, 10) == 100 && present_volume_step(5, -10) == 0);
    CHECK(present_volume_step(50, -10) == 40);

    /* a menu row's note: a page row's stops short of its arrow (the arrow
     * is drawn at right - 5, about 6 pixels wide) */
    {
        int nr, room;
        room = present_menu_note_room(272, 100, 0, &nr);
        CHECK(nr == 272 && room == 162);
        room = present_menu_note_room(272, 100, 1, &nr);
        CHECK(nr == 262 && room == 152 && nr <= 272 - 5 - 4);
        CHECK(present_menu_note_room(272, 260, 1, &nr) <= 12);   /* no room: no note */
    }

    /* window-shot names: "%d" numbers them; no "%d" is the old one file */
    {
        char p[32];
        CHECK(present_shot_path("/t/w%d.png", 7, p, sizeof p) == 0 && !strcmp(p, "/t/w7.png"));
        CHECK(present_shot_path("/t/w%d-%d.png", 12, p, sizeof p) == 0 &&
              !strcmp(p, "/t/w12-%d.png"));
        CHECK(present_shot_path("/t/w.png", 7, p, sizeof p) == 0 && !strcmp(p, "/t/w.png"));
        CHECK(present_shot_path("/t/100%.png", 7, p, sizeof p) == 0 && !strcmp(p, "/t/100%.png"));
        CHECK(present_shot_path("/t/a-long-name-%d.png", 123456, p, 16) == -1);
    }
    return 0;
}

int test_present_png_window_state(void) {
    char path[512];
    PcWindowState ws, back;

    /* (the overlays' font: pc/tests/test_ui_font.c) */
    tmpdir();
    snprintf(path, sizeof(path), "%s/shot.png", g_tmp);
    {
        unsigned char rgb[3 * 2 * 3];
        unsigned char buf[256];
        FILE *f;
        size_t n;
        int i;
        for (i = 0; i < (int)sizeof(rgb); i++) rgb[i] = (unsigned char)(i * 13);
        CHECK(present_png_write(path, rgb, 3, 2) == 0);
        f = fopen(path, "rb");
        n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        /* sig 8 + IHDR 25 + IDAT (12 + 2 + 5 + 2*(1+9) + 4) + IEND 12 */
        CHECK(n == 8 + 25 + 12 + 2 + 5 + 20 + 4 + 12);
        CHECK(!memcmp(buf, "\x89PNG\r\n\x1a\n", 8) && !memcmp(buf + 12, "IHDR", 4));
        CHECK(buf[19] == 3 && buf[23] == 2 && buf[24] == 8 && buf[25] == 2);
        CHECK(!memcmp(buf + n - 8, "IEND", 4));
    }

    /* the window state lives in [state] of settings.ini (2026-11-07) */
    snprintf(path, sizeof(path), "%s/sub/settings.ini", g_tmp);
    ws.valid = 1; ws.x = 40; ws.y = -20; ws.w = 960; ws.h = 720; ws.maximized = 1;
    CHECK(pc_window_state_save(path, &ws) == 0);
    CHECK(pc_window_state_load(path, &back) == 0);
    CHECK(back.valid && back.x == 40 && back.y == -20 && back.w == 960 && back.h == 720 && back.maximized);
    write_text(path, "[state]\nwindow_x = 0\nwindow_y = 0\nwindow_width = 5\nwindow_height = 5\n"
                     "window_maximized = 0\n");
    CHECK(pc_window_state_load(path, &back) != 0 && !back.valid);
    /* negative control: no [state] at all */
    write_text(path, "[display]\nscale = aspect\n");
    CHECK(pc_window_state_load(path, &back) != 0);
    tmp_cleanup();
    return 0;
}

static char *slurp_text(const char *path) {
    static char buf[16384];
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f) return NULL;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    return buf;
}

int test_settings_state_section(void) {
    char ini[512], win[512], hints[512], v[64];
    const char *user = "# my own note: keep this\n"
                       "profile = \"enhanced\"  # and this comment\n"
                       "\n[display]\nscale = integer\n";
    const char *k[2] = { "controls_hint", "other" }, *val[2] = { "shown", "x y" };
    PcWindowState ws;
    PcSettings s;
    char *text;

    tmpdir();
    snprintf(ini, sizeof(ini), "%s/settings.ini", g_tmp);
    snprintf(win, sizeof(win), "%s/window.ini", g_tmp);
    snprintf(hints, sizeof(hints), "%s/hints.ini", g_tmp);

    /* set: the player's lines are kept byte for byte; [state] is appended */
    write_text(ini, user);
    CHECK(pc_settings_state_get(ini, "controls_hint", v, sizeof v) != 0);
    CHECK(pc_settings_state_set(ini, k, val, 2) == 0);
    text = slurp_text(ini);
    CHECK(text && !strncmp(text, user, strlen(user)));
    CHECK(strstr(text, "safe\n# to delete. The settings above are never changed by it.\n[state]\n") != NULL);
    CHECK(pc_settings_state_get(ini, "controls_hint", v, sizeof v) == 0 && !strcmp(v, "shown"));
    CHECK(pc_settings_state_get(ini, "other", v, sizeof v) == 0 && !strcmp(v, "x y"));
    {
        char tmp[600];
        snprintf(tmp, sizeof tmp, "%s.tmp", ini);
        CHECK(!file_exists(tmp));       /* written through a temporary */
    }
    /* a second set rewrites in place: one [state], one controls_hint */
    val[0] = "again";
    CHECK(pc_settings_state_set(ini, k, val, 1) == 0);
    text = slurp_text(ini);
    CHECK(strstr(strstr(text, "[state]") + 1, "[state]") == NULL);
    CHECK(strstr(strstr(text, "controls_hint") + 1, "controls_hint") == NULL);
    CHECK(pc_settings_state_get(ini, "controls_hint", v, sizeof v) == 0 && !strcmp(v, "again"));
    /* the parser skips [state]: no warnings, the settings still read */
    pc_settings_defaults(&s);
    CHECK(pc_settings_parse(&s, text, "settings.ini") == 0);
    CHECK(s.scale_mode == PC_SCALE_INTEGER);
    /* a key of the same name outside [state] is not state */
    write_text(ini, "[display]\ncontrols_hint = no\n");
    CHECK(pc_settings_state_get(ini, "controls_hint", v, sizeof v) != 0);

    /* fold: window.ini + hints.ini of the old layout go into [state] and
     * are deleted; the player's settings stay */
    write_text(ini, user);
    write_text(win, "# Written by the game on exit\n[window]\nx = 12\ny = 34\nwidth = 800\n"
                    "height = 600\nmaximized = 0\n");
    write_text(hints, "# One-time hints\ncontrols_hint = shown\n");
    CHECK(pc_settings_fold_legacy(ini, g_tmp) == 2);
    CHECK(!file_exists(win) && !file_exists(hints));
    text = slurp_text(ini);
    CHECK(text && !strncmp(text, user, strlen(user)));
    CHECK(pc_window_state_load(ini, &ws) == 0 && ws.x == 12 && ws.y == 34 && ws.w == 800 && ws.h == 600);
    CHECK(pc_settings_state_get(ini, "controls_hint", v, sizeof v) == 0 && !strcmp(v, "shown"));
    /* empty (0-byte) old files carry nothing: deleted, nothing added */
    write_text(ini, user);
    write_text(win, "");
    write_text(hints, "");
    CHECK(pc_settings_fold_legacy(ini, g_tmp) == 2);
    CHECK(!file_exists(win) && !file_exists(hints));
    CHECK(!strcmp(slurp_text(ini), user));
    /* nothing to fold: nothing changes, no file is made */
    CHECK(pc_settings_fold_legacy(ini, g_tmp) == 0);
    CHECK(!strcmp(slurp_text(ini), user));
    CHECK(!file_exists(win) && !file_exists(hints));
    tmp_cleanup();
    return 0;
}
