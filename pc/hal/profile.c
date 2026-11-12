/* SPDX-License-Identifier: GPL-3.0-or-later */
/* profile.c -- the PC port's run profile (parity / enhanced) and the table
 * of switches it sets defaults for.  Interface and precedence: profile.h.
 * User documentation: docs/pc-port/ENHANCED-MODE.md.
 *
 * The table is the single definition of what Enhanced changes.  A timing
 * model listed here is on under parity -- its code path and its negative
 * control (`NAME=0`) are exactly what they were before profiles existed --
 * and off under enhanced.  Fidelity fixes that are not hardware slowness
 * (seek jitter draws, the VBlank line, status bits, ...) are deliberately
 * NOT in the table: both profiles keep them.
 */
#define PC_HAL_PROFILE_IMPL
#include "profile.h"
#include "pc_settings.h"

/* settings.c is linked into the game; profile.c alone (the profile tests)
 * goes without it */
extern const PcSettings *pc_settings_get(void) __attribute__((weak));
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const HalSwitch hal_profile_switches[] = {
    /* ---- hardware-timing models: parity on, enhanced off ---- */
    { "GAME_CPU_COST",      1, 0, "game code takes retail's measured CPU time (frame overruns, "
                                  "the VBlank sequencer tick, the HAL's named SDK charges)" },
    { "BOOT_CPU_COST",      1, 0, "the boot's own CPU time (_main's init, the BIOS VBlank drivers)" },
    { "BIOS_CPU_COST",      1, 0, "CPU time of HAL-replaced BIOS calls (StartRCnt, InitGeom)" },
    { "HAL_SDK_CPU",        1, 0, "CPU time of HAL-replaced pure-CPU SDK functions (libgte, ...)" },
    { "GPU_DRAW_TIME",      1, 0, "the GPU takes time to draw (DrawSync waits for it)" },
    { "GPU_OT_CYCLE_HANG",  1, 0, "a cyclic ordering table keeps the GPU busy until DrawSync "
                                  "times out (~4 s in TACO, as on the console)" },
    { "GPU_LOADIMAGE_CPU",  1, 0, "libgpu LoadImage's CPU work" },
    { "GPU_CLEARIMAGE_CPU", 1, 0, "libgpu ClearImage's CPU work" },
    { "RESETGRAPH0_CPU",    1, 0, "ResetGraph(0)'s CPU work" },
    { "SPU_INIT_CPU",       1, 0, "libspu SpuInit's register-setup CPU work" },
    { "MDEC_VLC_CPU",       1, 0, "the movie decoder's VLC pass CPU time" },
    { "MDEC_VLC_STILL",     1, 0, "the VLC pass of still pictures (backgrounds) -- ~2 frames each" },
    { "CD_DRIVE_TIMING",    1, 0, "CD seek, spin-up, speed change, pause and data-sector times "
                                  "(XA voice and movie streams stay real-time)" },
    { "CARD_IO_TIMING",     1, 0, "memory-card requests wait for their slot turn and serial time" },
    { "CARD_CREATE_IO",     1, 0, "file create / failed open do their card I/O synchronously" },
    { "BU_INIT_TAIL",       1, 0, "_bu_init's tail CPU time" },
    /* ---- PC-side features: parity off ---- */
    { "CD_WAIT_MUSIC_FADE", 0, 1, "with instant CD: a data read waits while the game fades its "
                                  "music out, so the next scene's sound bank never cuts the old "
                                  "song mid-fade (on the console the drive is always slower)" },
    { "TOKI_FAST_FORWARD",  0, 1, "hold ` (backquote) or the right-stick click: run "
                                  "TOKI_FF_SPEED game frames per shown frame" },
    { "GPU_HIRES",          0, 1, "3D polygons at [graphics] resolution_scale (TOKI_RES_SCALE) times "
                                  "the console's resolution; the game's VRAM stays 1x" },
    { "GPU_PGXP",           0, 1, "with GPU_HIRES: 3D vertices at the GTE's sub-pixel screen "
                                  "positions in the high-resolution picture ([graphics] subpixel, "
                                  "TOKI_SUBPIXEL); the game's VRAM is unchanged" },
    { "MUSIC_PACK",         0, 1, "play the songs of the music pack [audio] music_pack "
                                  "(TOKI_MUSIC_PACK) in place of the sequenced music; the game's "
                                  "sequencer still runs, its voices are left out of the mix" },
    { "MUSIC_SOUNDFONT",    0, 1, "play the sequenced music through the SoundFont [audio] soundfont "
                                  "(TOKI_SOUNDFONT) from the game's own note events; the game's "
                                  "sequencer still runs, its music voices are left out of the mix" },
    { "SPU_QUALITY",        0, 1, "the enhanced sound settings [audio] interpolation, reverb and "
                                  "output_rate (nothing changes until one leaves its console value)" },
    { "GPU_TEXTURES",       0, 1, "texture dumping and replacement packs ([graphics] texture_dump, "
                                  "texture_pack); replacements draw only into the high-resolution "
                                  "shadow" },
    { "TOKI_BACKLOG",       0, 1, "dialogue backlog (Ctrl+B / wheel up) and voice replay "
                                  "(Ctrl+R): completed pages and their voice lines, read only" },
    { "TOKI_STATUS",        0, 1, "status at a glance (Ctrl+I): the met girls' affection, bombs "
                                  "and ending scores, the parameters and the calendar, read "
                                  "only from game RAM" },
    { "TOKI_REWIND",        0, 1, "rewind buffer: hold [hotkeys] rewind (Delete) to step back "
                                  "([enhanced] rewind, rewind_seconds, rewind_interval_frames)" },
    { "TOKI_MENU_MODE_KEEP", 0, 1, "the menu mode chosen on the pad (cycle through the options, "
                                  "hand, ...) comes back when the pad takes port 1 back from "
                                  "the mouse; the game sets the hand mode for a mouse and, as "
                                  "on the console, leaves it set" },
    { "TOKI_SKIP_INTRO",    0, 1, "press START through the boot movie and title animation when "
                                  "[enhanced] skip_intro asks (input only)" },
    { "TOKI_SKIP_SEEN",     0, 1, "fast-forward also confirms dialogue pages read before when "
                                  "[enhanced] skip_seen asks (input only)" },
    { "TOKI_QUICK_TEXT",    0, 1, "confirm during a printing dialogue page completes it (one write: "
                                  "the shown-glyph counter); confirm during its voice line sends "
                                  "the game's cross cut ([enhanced] quick_text)" },
    { "TOKI_EXTRAS",        0, 1, "extras: the endings seen, a gallery of the pictures shown and "
                                  "achievements, observed read only, kept in <saves>/extras" },
    { "TOKI_TEXT_AUTO",     0, 0, "press confirm TOKI_TEXT_AUTO_DELAY frames after a dialogue "
                                  "page (and its voice) ends; opt-in under either profile" },
};
const unsigned hal_profile_switch_count =
    sizeof hal_profile_switches / sizeof hal_profile_switches[0];

#define MAX_CFG 64
static struct { char key[64]; char value[128]; } g_cfg[MAX_CFG];
static int g_cfg_n = -1;           /* -1: not read yet */
static char g_cfg_path[512];
static int g_profile = -1;
static const char *g_profile_src = "default";
static const char *g_cli_profile;  /* from hal_profile_parse_args */
static signed char g_value[sizeof hal_profile_switches / sizeof hal_profile_switches[0]];
static const char *g_value_src[sizeof hal_profile_switches / sizeof hal_profile_switches[0]];
static int g_values_init;

static int key_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

static char *trim(char *s)
{
    char *e;
    while (*s && isspace((unsigned char)*s)) s++;
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

/* The config file: `key = value` lines, `#` / `;` comments, `[section]`
 * headers ignored (keys are global).  Missing file: no settings. */
static void cfg_load(void)
{
    const char *path = getenv("TOKI_CONFIG");
    FILE *f;
    char line[512];
    if (g_cfg_n >= 0) return;
    g_cfg_n = 0;
    if (!path || !*path) path = "toki.cfg";
    snprintf(g_cfg_path, sizeof g_cfg_path, "%s", path);
    f = fopen(path, "r");
    if (!f) {
        if (getenv("TOKI_CONFIG") && *getenv("TOKI_CONFIG"))
            fprintf(stderr, "[PROFILE] TOKI_CONFIG %s: cannot open\n", path);
        g_cfg_path[0] = 0;
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char *s = line, *eq, *k, *v, *c;
        if ((c = strpbrk(s, "#;"))) *c = 0;
        s = trim(s);
        if (!*s || *s == '[') continue;
        eq = strchr(s, '=');
        if (!eq) {
            fprintf(stderr, "[PROFILE] %s: ignored line: %s\n", path, s);
            continue;
        }
        *eq = 0;
        k = trim(s);
        v = trim(eq + 1);
        if (!*k || g_cfg_n == MAX_CFG) continue;
        snprintf(g_cfg[g_cfg_n].key, sizeof g_cfg[0].key, "%s", k);
        snprintf(g_cfg[g_cfg_n].value, sizeof g_cfg[0].value, "%s", v);
        g_cfg_n++;
    }
    fclose(f);
}

static const char *cfg_get(const char *key)
{
    int i;
    cfg_load();
    for (i = g_cfg_n - 1; i >= 0; i--)     /* the last assignment wins */
        if (key_eq(g_cfg[i].key, key)) return g_cfg[i].value;
    return NULL;
}

static int parse_profile(const char *v, const char *where)
{
    if (key_eq(v, "parity")) return HAL_PROFILE_PARITY;
    if (key_eq(v, "enhanced")) return HAL_PROFILE_ENHANCED;
    fprintf(stderr, "[PROFILE] unknown profile '%s' (%s): using parity\n", v, where);
    return HAL_PROFILE_PARITY;
}

int hal_profile_parse_args(int argc, char **argv)
{
    int i, o = 1;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strncmp(a, "--profile=", 10)) { g_cli_profile = a + 10; continue; }
        if (!strcmp(a, "--profile") && i + 1 < argc) { g_cli_profile = argv[++i]; continue; }
        if (!strcmp(a, "--enhanced")) { g_cli_profile = "enhanced"; continue; }
        if (!strcmp(a, "--parity")) { g_cli_profile = "parity"; continue; }
        argv[o++] = argv[i];
    }
    if (o < argc) argv[o] = NULL;
    return o;
}

HalProfile hal_profile(void)
{
    const char *v;
    if (g_profile >= 0) return (HalProfile)g_profile;
    if (g_cli_profile && *g_cli_profile) {
        g_profile = parse_profile(g_cli_profile, "command line");
        g_profile_src = "cli";
    } else if ((v = getenv("TOKI_PROFILE")) && *v) {
        g_profile = parse_profile(v, "TOKI_PROFILE");
        g_profile_src = "env";
    } else if ((v = cfg_get("profile")) && *v) {
        g_profile = parse_profile(v, g_cfg_path);
        g_profile_src = "config";
    } else if (pc_settings_get && (v = pc_settings_get()->profile) && *v) {
        /* the player's settings.ini (pc/hal/settings.c); defaults
         * ("parity") under the test harnesses, which ignore user settings */
        g_profile = parse_profile(v, "settings.ini");
        g_profile_src = "settings";
    } else {
        g_profile = HAL_PROFILE_PARITY;
        g_profile_src = "default";
    }
    return (HalProfile)g_profile;
}

const char *hal_profile_name(void)
{
    return hal_profile() == HAL_PROFILE_ENHANCED ? "enhanced" : "parity";
}

const char *hal_profile_source(void)
{
    (void)hal_profile();
    return g_profile_src;
}

static int on_off(const char *v, int *out)
{
    if (!v || !*v) return 0;
    *out = v[0] != '0';
    return 1;
}

static void values_init(void)
{
    unsigned i;
    int enhanced;
    if (g_values_init) return;
    enhanced = hal_profile() == HAL_PROFILE_ENHANCED;
    for (i = 0; i < hal_profile_switch_count; i++) {
        const HalSwitch *s = &hal_profile_switches[i];
        int v;
        if (on_off(getenv(s->name), &v)) {
            g_value_src[i] = "env";
        } else if (on_off(cfg_get(s->name), &v)) {
            g_value_src[i] = "config";
        } else {
            v = enhanced ? s->enhanced : s->parity;
            g_value_src[i] = "profile";
        }
        g_value[i] = (signed char)v;
    }
    g_values_init = 1;
    if (enhanced || g_cfg_path[0])
        hal_profile_report(stdout);
}

int hal_switch(const char *name)
{
    unsigned i;
    values_init();
    for (i = 0; i < hal_profile_switch_count; i++)
        if (!strcmp(hal_profile_switches[i].name, name)) return g_value[i];
    {
        static int warned;
        if (!warned++) fprintf(stderr, "[PROFILE] unknown switch %s (treated as on)\n", name);
    }
    return 1;
}

long hal_setting_long(const char *name, long dflt)
{
    const char *v = getenv(name);
    char *end;
    long n;
    if (!v || !*v) v = cfg_get(name);
    if (!v || !*v) return dflt;
    n = strtol(v, &end, 0);
    if (end == v) {
        fprintf(stderr, "[PROFILE] %s=%s is not a number; using %ld\n", name, v, dflt);
        return dflt;
    }
    return n;
}

const char *hal_setting_str(const char *name, const char *dflt)
{
    const char *v = getenv(name);
    if (!v || !*v) v = cfg_get(name);
    return (v && *v) ? v : dflt;
}

void hal_profile_report(void *stream)
{
    FILE *f = stream ? (FILE *)stream : stdout;
    unsigned i;
    values_init();
    fprintf(f, "[PROFILE] %s (from %s%s%s)\n", hal_profile_name(), hal_profile_source(),
            g_cfg_path[0] ? "; config " : "", g_cfg_path);
    for (i = 0; i < hal_profile_switch_count; i++)
        fprintf(f, "[PROFILE]   %-20s %s  (%s)\n", hal_profile_switches[i].name,
                g_value[i] ? "on " : "off", g_value_src[i]);
    fflush(f);
}

void hal_profile_reset(void)
{
    g_cfg_n = -1;
    g_cfg_path[0] = 0;
    g_profile = -1;
    g_profile_src = "default";
    g_cli_profile = NULL;
    g_values_init = 0;
}
