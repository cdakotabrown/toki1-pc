/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_settings_menu.c -- the in-game settings menu: its model
 * (pc/hal/menu.c), writing changed keys back into settings.ini
 * (pc/hal/settings.c) and the input hold-off after it closes
 * (pc/hal/pad_device.c).  No SDL, no assets.  The drawing and the pause
 * are checked against the real binary by pc/tools/test_settings_menu.py. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pc_settings.h"
#include "pc_menu.h"
#include "pad_device.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "  %s:%d: %s\n", __FILE__, __LINE__, #c); return __LINE__; } } while (0)

static int edit_is(const char *text, const char *sec, const char *key, const char *val,
                   const char *want) {
    char *got = pc_settings_edit_text(text, sec, key, val);
    int ok = got && !strcmp(got, want);
    if (!ok) fprintf(stderr, "  edit [%s] %s=%s\n  --- got ---\n%s\n  --- want ---\n%s\n",
                     sec, key, val, got ? got : "(null)", want);
    free(got);
    return ok;
}

int test_settings_menu_keys(void) {
    PcSettings s;
    pc_settings_defaults(&s);
    CHECK(!strcmp(s.hotkey[PC_HK_MENU], "Ctrl+O"));     /* F1 is START, Escape cross */
    CHECK(!strcmp(pc_hotkey_names[PC_HK_MENU], "menu"));
    CHECK(!strcmp(s.menu_button, "guide, touchpad"));
    CHECK(s.ff_speed == 4);                              /* enhance.c's old default */
    CHECK(pc_settings_set(&s, "enhanced.fast_forward_speed", "8") == 0 && s.ff_speed == 8);
    CHECK(pc_settings_set(&s, "enhanced.fast_forward_speed", "65") != 0 && s.ff_speed == 8);
    CHECK(pc_settings_parse(&s, "[hotkeys]\nmenu = F10\n[input]\nmenu_button = back\n", "t") == 0);
    CHECK(!strcmp(s.hotkey[PC_HK_MENU], "F10") && !strcmp(s.menu_button, "back"));
    {
        char v[64];
        CHECK(pc_settings_format(&s, "display.scale", v, sizeof(v)) == 0 && !strcmp(v, "aspect"));
        CHECK(pc_settings_format(&s, "profile", v, sizeof(v)) == 0 && !strcmp(v, "\"enhanced\""));
        CHECK(pc_settings_format(&s, "display.nope", v, sizeof(v)) != 0);
    }
    return 0;
}

int test_settings_edit_text(void) {
    /* an existing key: value replaced, indentation, spacing before '=' and
     * the trailing comment kept; everything else byte for byte */
    CHECK(edit_is("# mine\n[display]\n  scale   = stretch   # wide\nmystery = 7\n",
                  "display", "scale", "integer",
                  "# mine\n[display]\n  scale   = integer  # wide\nmystery = 7\n"));
    /* quoted value holding a '#', comment after the quote */
    CHECK(edit_is("profile = \"par#ity\" ; c\n[x]\n", "", "profile", "\"enhanced\"",
                  "profile = \"enhanced\"  ; c\n[x]\n"));
    /* case-insensitive section and key, every assignment rewritten */
    CHECK(edit_is("[Audio]\nMaster = 10\nmaster=20\n", "audio", "master", "55",
                  "[Audio]\nMaster = 55\nmaster= 55\n"));
    /* the same key in another section is left alone */
    CHECK(edit_is("[a]\nk = 1\n[b]\nk = 2\n", "b", "k", "9", "[a]\nk = 1\n[b]\nk = 9\n"));
    /* missing key: after the section's last key, before the next section's
     * comments */
    CHECK(edit_is("[audio]\n# vol\nmaster = 100\n\n# input\n[input]\nmouse = true\n",
                  "audio", "voice", "40",
                  "[audio]\n# vol\nmaster = 100\nvoice = 40\n\n# input\n[input]\nmouse = true\n"));
    /* missing key in a section with no keys: right after the header */
    CHECK(edit_is("[enhanced]\n# nothing yet\n", "enhanced", "fast_forward_speed", "8",
                  "[enhanced]\nfast_forward_speed = 8\n# nothing yet\n"));
    /* missing section: appended, unterminated last line fixed */
    CHECK(edit_is("[display]\nscale = aspect", "enhanced", "fast_forward_speed", "2",
                  "[display]\nscale = aspect\n\n[enhanced]\nfast_forward_speed = 2\n"));
    /* missing key as the unterminated last line of its section */
    CHECK(edit_is("[display]\nscale = aspect", "display", "vsync", "true",
                  "[display]\nscale = aspect\nvsync = true\n"));
    /* top level: before the first section, after top-level keys */
    CHECK(edit_is("# header\n\n[display]\nscale = aspect\n", "", "profile", "\"enhanced\"",
                  "# header\n\nprofile = \"enhanced\"\n[display]\nscale = aspect\n"));
    CHECK(edit_is("other = 1\n[display]\n", "", "profile", "\"parity\"",
                  "other = 1\nprofile = \"parity\"\n[display]\n"));
    /* an empty / missing file */
    CHECK(edit_is("", "audio", "master", "30", "[audio]\nmaster = 30\n"));
    CHECK(edit_is("", "", "profile", "\"parity\"", "profile = \"parity\"\n"));
    return 0;
}

int test_settings_save_keys(void) {
    char dir[] = "/tmp/toki_menu_test_XXXXXX", path[256], cmd[300];
    const char *keys[] = { "display.scale", "audio.master", "profile", "enhanced.fast_forward_speed",
                           "graphics.resolution_scale", "graphics.texture_dump",
                           "graphics.texture_pack" };
    PcSettings s, back;
    FILE *f;
    char buf[4096];
    size_t n;
    int r = 0;
    if (!mkdtemp(dir)) return __LINE__;
    snprintf(path, sizeof(path), "%s/sub/settings.ini", dir);
    pc_settings_defaults(&s);
    s.scale_mode = PC_SCALE_INTEGER;
    s.vol_master = 35;
    snprintf(s.profile, sizeof(s.profile), "parity");
    s.ff_speed = 0;
    s.resolution_scale = 3;      /* the menu's 3D RESOLUTION entry */
    s.texture_dump = 1;          /* DUMP TEXTURES */
    snprintf(s.texture_pack, sizeof(s.texture_pack), "hd pack");   /* TEXTURE PACK */
    /* a missing file (and directory) is created with just these keys */
    if (pc_settings_save_keys(&s, path, keys, 7) != 0) { r = __LINE__; goto out; }
    pc_settings_defaults(&back);
    f = fopen(path, "r");
    if (!f) { r = __LINE__; goto out; }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    if (pc_settings_parse(&back, buf, path) != 0 || back.scale_mode != PC_SCALE_INTEGER ||
        back.vol_master != 35 || strcmp(back.profile, "parity") || back.ff_speed != 0 ||
        back.resolution_scale != 3 || back.texture_dump != 1 ||
        strcmp(back.texture_pack, "hd pack")) {
        r = __LINE__; goto out;
    }
    /* an existing file: comments, unknown keys and other values survive */
    f = fopen(path, "w");
    fputs("# my notes\n[display]\nscale = stretch  # keep me\nfilter = linear\n"
          "[mods]\ntexture_pack = \"hd\"\n", f);
    fclose(f);
    if (pc_settings_save_keys(&s, path, keys, 2) != 0) { r = __LINE__; goto out; }
    f = fopen(path, "r");
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    if (strcmp(buf, "# my notes\n[display]\nscale = integer  # keep me\nfilter = linear\n"
                    "[mods]\ntexture_pack = \"hd\"\n\n[audio]\nmaster = 35\n")) {
        fprintf(stderr, "  saved:\n%s\n", buf);
        r = __LINE__; goto out;
    }
    /* no temporary left behind */
    snprintf(cmd, sizeof(cmd), "%s.tmp", path);
    if (access(cmd, F_OK) == 0) { r = __LINE__; goto out; }
    /* a harness run never writes, whatever it is asked */
    s.harness = 1;
    if (pc_settings_save_keys(&s, path, keys, 2) != -1) { r = __LINE__; goto out; }
    s.harness = 0;
    if (pc_settings_save_keys(&s, "", keys, 2) != -1) { r = __LINE__; goto out; }
    f = fopen(path, "r");
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    if (!strstr(buf, "scale = integer  # keep me")) { r = __LINE__; goto out; }
out:
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if (system(cmd)) { }
    return r;
}

/* ---- the menu model with fake entries ---- */
static int g_val[8];
static int g_activated;
static const char *g_saved[16];
static int g_nsaved, g_save_calls;

static int fget(void *c) { return g_val[(int)(long)c]; }
static void fset(void *c, int v) { g_val[(int)(long)c] = v; }
static void fact(void *c) { (void)c; g_activated++; }
static int fsave(const char *const *keys, int n) {
    int i;
    g_save_calls++;
    for (i = 0; i < n && g_nsaved < 16; i++) g_saved[g_nsaved++] = keys[i];
    return 0;
}

static void add(int page, int type, const char *label, const char *key, int slot) {
    static const char *const choices[] = { "A", "B", "C", NULL };
    PcMenuItem it;
    memset(&it, 0, sizeof(it));
    it.page = page;
    it.type = type;
    it.label = label;
    it.key = key;
    it.ctx = (void *)(long)slot;
    it.get = fget;
    it.set = fset;
    it.activate = fact;
    it.choices = choices;
    it.min = 0;
    it.max = 100;
    it.step = 30;
    it.unit = "%";
    pc_menu_add(&it);
}

int test_settings_menu_model(void) {
    PcMenuView v;
    PcMenuItem it;
    int sub;
    pc_menu_reset();
    memset(g_val, 0, sizeof(g_val));
    g_activated = g_nsaved = g_save_calls = 0;
    pc_menu_set_save(fsave);
    add(0, PC_MENU_HEADER, "HEAD", NULL, 0);
    add(0, PC_MENU_TOGGLE, "T", "display.vsync", 1);
    add(0, PC_MENU_CHOICE, "C", "display.scale", 2);
    add(0, PC_MENU_SLIDER, "S", "audio.master", 3);
    sub = pc_menu_page("SAVE SLOTS");
    CHECK(sub > 0);
    memset(&it, 0, sizeof(it));
    it.type = PC_MENU_PAGE;
    it.label = "SLOTS";
    it.target = sub;
    CHECK(pc_menu_add(&it) >= 0);
    add(sub, PC_MENU_ACTION, "LOAD", NULL, 0);
    add(sub, PC_MENU_TOGGLE, "SESSION", NULL, 4);   /* no key: never saved */
    it.target = 99;
    CHECK(pc_menu_add(&it) == -1);                  /* bad page */
    CHECK(pc_menu_count() == 7);

    CHECK(!pc_menu_is_open());
    pc_menu_input(PC_MENU_OK);                      /* closed: ignored */
    CHECK(g_val[1] == 0);
    pc_menu_open();
    pc_menu_view(&v);
    CHECK(!strcmp(v.title, "Settings") && v.nrows == 5);
    CHECK(v.selected == 1);                         /* the header is skipped */
    CHECK(!strcmp(v.rows[1].value, "Off") && !strcmp(v.rows[2].value, "A"));
    CHECK(!strcmp(v.rows[3].value, "0%") && v.rows[3].slider && v.rows[3].max == 100);

    pc_menu_input(PC_MENU_OK);                      /* toggle */
    CHECK(g_val[1] == 1);
    pc_menu_input(PC_MENU_RIGHT);
    CHECK(g_val[1] == 0);
    pc_menu_input(PC_MENU_DOWN);
    pc_menu_input(PC_MENU_LEFT);                    /* choice wraps backwards */
    CHECK(g_val[2] == 2);
    pc_menu_input(PC_MENU_RIGHT);
    CHECK(g_val[2] == 0);
    pc_menu_input(PC_MENU_DOWN);
    pc_menu_input(PC_MENU_LEFT);                    /* slider clamps: no change, not dirty */
    CHECK(g_val[3] == 0);
    pc_menu_input(PC_MENU_RIGHT);
    pc_menu_input(PC_MENU_RIGHT);
    pc_menu_input(PC_MENU_RIGHT);
    pc_menu_input(PC_MENU_RIGHT);
    CHECK(g_val[3] == 100);
    pc_menu_view(&v);
    CHECK(v.selected == 3 && !strcmp(v.rows[3].value, "100%") && v.rows[3].pos == 100);
    CHECK(pc_menu_dirty_count() == 3);              /* each key once */

    pc_menu_input(PC_MENU_DOWN);                    /* SLOTS */
    pc_menu_input(PC_MENU_DOWN);                    /* wraps past the header */
    pc_menu_view(&v);
    CHECK(v.selected == 1);
    pc_menu_input(PC_MENU_UP);                      /* wraps back to SLOTS */
    pc_menu_input(PC_MENU_RIGHT);                   /* left/right never open a page */
    pc_menu_view(&v);
    CHECK(!strcmp(v.title, "Settings") && v.selected == 4);
    pc_menu_input(PC_MENU_OK);
    pc_menu_view(&v);
    CHECK(!strcmp(v.title, "SAVE SLOTS") && v.nrows == 2 && v.selected == 0);
    pc_menu_input(PC_MENU_OK);
    CHECK(g_activated == 1);
    pc_menu_input(PC_MENU_DOWN);
    pc_menu_input(PC_MENU_OK);
    CHECK(g_val[4] == 1 && pc_menu_dirty_count() == 3);
    pc_menu_input(PC_MENU_BACK);                    /* back to the main page, same row */
    pc_menu_view(&v);
    CHECK(!strcmp(v.title, "Settings") && v.selected == 4 && pc_menu_is_open());
    CHECK(g_save_calls == 0);
    pc_menu_input(PC_MENU_BACK);                    /* closes and saves once */
    CHECK(!pc_menu_is_open());
    CHECK(g_save_calls == 1 && g_nsaved == 3);
    CHECK(!strcmp(g_saved[0], "display.vsync") && !strcmp(g_saved[1], "display.scale") &&
          !strcmp(g_saved[2], "audio.master"));
    /* reopened: nothing changed, nothing saved */
    pc_menu_open();
    CHECK(pc_menu_close() == 0 && g_save_calls == 1);
    CHECK(pc_menu_close() == -2);
    pc_menu_reset();
    return 0;
}

int test_settings_menu_notes(void) {
    PcMenuView v;
    PcMenuItem it;
    pc_menu_reset();
    memset(&it, 0, sizeof(it));
    it.type = PC_MENU_CHOICE;
    it.label = "PROFILE";
    it.key = "profile";
    it.flags = PC_MENU_RESTART;
    it.get = fget;
    it.set = fset;
    it.ctx = (void *)5L;
    g_val[5] = 7;                                    /* out of range */
    CHECK(pc_menu_add(&it) == 0);
    pc_menu_open();
    pc_menu_view(&v);
    CHECK(!strcmp(v.rows[0].value, "7"));            /* no choices: the number */
    CHECK(!strcmp(v.rows[0].note, "On restart"));
    pc_menu_close();
    pc_menu_reset();
    return 0;
}

int test_pad_hold_off(void) {
    CHECK(pad_hold_off(0x40) == 0x40);              /* not armed: unchanged */
    pad_hold_off_arm();
    CHECK(pad_hold_off(0x40) == 0);                 /* Escape (cross) still down */
    CHECK(pad_hold_off(0x60) == 0);                 /* another press joins: still held off */
    CHECK(pad_hold_off(0) == 0);                    /* all released: disarmed */
    CHECK(pad_hold_off(0x20) == 0x20);              /* a new press reaches the game */
    return 0;
}
