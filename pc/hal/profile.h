/* SPDX-License-Identifier: GPL-3.0-or-later */
/* profile.h -- the PC port's run profile and its switches (pc/hal/profile.c).
 *
 * A profile is a set of DEFAULTS for named switches:
 *
 *   parity    every hardware-timing model on, no enhancement:
 *             exactly the behaviour the playthroughs, the scene matrix and
 *             the retail comparisons check.
 *   enhanced  (the player default) the timing models that only reproduce the console's slowness
 *             off (instant loads, no frame overruns), the PC-side play
 *             features on.  docs/pc-port/ENHANCED-MODE.md.
 *
 * Selection, first match wins:
 *   1. the command line:   --profile=NAME, --profile NAME, --enhanced, --parity
 *   2. the environment:    TOKI_PROFILE=NAME
 *   3. the config file:    `profile = NAME` in $TOKI_CONFIG, else ./toki.cfg
 *   4. the player's settings.ini `profile` (pc/hal/settings.c): "enhanced"
 *      by default since 2026-11-04; automated runs (harness env) read
 *      "parity" there
 *   5. parity
 *
 * A switch's value, first match wins:
 *   1. the environment variable of the same name (`0...` off, anything
 *      else non-empty on) -- the negative controls the timing work uses;
 *   2. `NAME = 0|1` in the config file;
 *   3. the profile's default (hal_profile_switches below).
 *
 * Every switch is read once, lazily, the first time the HAL asks; the
 * profile cannot change while the game runs.  This module is SDL-free and
 * game-free, so a settings front end (or a test) can link it alone and
 * enumerate the table.
 */
#ifndef PC_HAL_PROFILE_H
#define PC_HAL_PROFILE_H

typedef enum {
    HAL_PROFILE_PARITY = 0,
    HAL_PROFILE_ENHANCED = 1
} HalProfile;

typedef struct {
    const char *name;     /* environment / config key */
    unsigned char parity; /* default under parity */
    unsigned char enhanced; /* default under enhanced */
    const char *what;     /* one line, for --list-switches / a settings menu */
} HalSwitch;

extern const HalSwitch hal_profile_switches[];
extern const unsigned hal_profile_switch_count;

/* Register the command line before the first switch is read (pc/src/main.c).
 * Removes the profile options it recognises from argv; returns the new argc. */
int hal_profile_parse_args(int argc, char **argv);

HalProfile hal_profile(void);
const char *hal_profile_name(void);
/* Where the profile came from: "default", "cli", "env", "config". */
const char *hal_profile_source(void);

/* A switch of the table: 1 on, 0 off.  An unknown name is a programming
 * error: reported once on stderr and treated as on (parity).  HAL code
 * calls hal_model_on() below instead, which also works in the harnesses
 * that link a HAL file without this module. */
#ifdef PC_HAL_PROFILE_IMPL
int hal_switch(const char *name);
#else
extern int hal_switch(const char *name) __attribute__((weak));
#endif
/* A numeric setting (env, then config file, then `dflt`). */
long hal_setting_long(const char *name, long dflt);
/* A text setting (env, then config file, then `dflt`); never NULL unless
 * `dflt` is. */
const char *hal_setting_str(const char *name, const char *dflt);

/* Print the profile and every switch with its value and source. */
void hal_profile_report(void *stream);

/* Test hook: forget every cached value (the profile and the switches). */
void hal_profile_reset(void);

#ifndef PC_HAL_PROFILE_IMPL
#include <stdlib.h>
/* A switch as the HAL reads it: the profile's value when this module is
 * linked; otherwise (unit harnesses) the pre-profile rule -- on unless the
 * environment variable starts with '0'. */
static __inline__ int hal_model_on(const char *name)
{
    const char *e;
    if (hal_switch) return hal_switch(name);
    e = getenv(name);
    return !(e && e[0] == '0');
}
/* The same for a feature switch that is off unless asked for. */
static __inline__ int hal_feature_on(const char *name)
{
    const char *e;
    if (hal_switch) return hal_switch(name);
    e = getenv(name);
    return e && *e && e[0] != '0';
}
#endif

#endif
