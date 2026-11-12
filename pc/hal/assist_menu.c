/* SPDX-License-Identifier: GPL-3.0-or-later */
/* assist_menu.c -- the settings menu's entries for the enhanced profile's
 * time and skip assists (pc/include/pc_menu.h):
 *
 *   REWIND            [enhanced] rewind        pc/hal/rewind.c
 *   SKIP INTRO        [enhanced] skip_intro    pc/hal/enhance.c
 *   SKIP READ TEXT    [enhanced] skip_seen     pc/hal/seen_text.c
 *   QUICK TEXT        [enhanced] quick_text    pc/hal/quick_text.c
 *
 * Each is a toggle of its settings.ini key (saved when the menu closes,
 * never in an automated run).  Rewind, the read-text skip and quick text
 * follow the setting at once; the intro skip is read at power-on, so it takes effect
 * at the next launch.  Registered by the presenter (pc/hal/present.c).
 * Session state only (the menu's), kept out of save states with menu.c.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pc_menu.h"
#include "pc_settings.h"
#include "profile.h"

extern unsigned long seen_text_count(void) __attribute__((weak));

enum { A_REWIND, A_INTRO, A_SEEN, A_QUICK, A_COUNT };

static int *field(int which)
{
    PcSettings *s = pc_settings_mut();
    return which == A_REWIND ? &s->rewind : which == A_INTRO ? &s->skip_intro :
           which == A_SEEN ? &s->skip_seen : &s->quick_text;
}

static int get_on(void *p) { return *field((int)(long)p); }
static void set_on(void *p, int v) { *field((int)(long)p) = v != 0; }

static const char *note(void *p)
{
    static char t[32];
    int which = (int)(long)p;
    static const char *const env[] = { "TOKI_REWIND", "TOKI_SKIP_INTRO", "TOKI_SKIP_SEEN",
                                       "TOKI_QUICK_TEXT" };
    const char *e = getenv(env[which]);
    if (strcmp(hal_profile_name(), "enhanced")) return "Enhanced only";
    if (e && *e) return "Set by env var";
    if (which == A_REWIND) return "Hold Delete";
    if (which == A_INTRO) return "Next launch";
    if (which == A_QUICK) return "Confirm";
    snprintf(t, sizeof t, "Hold FF, %lu read", seen_text_count ? seen_text_count() : 0ul);
    return t;
}

void assist_menu_register(void)
{
    static int done;
    static const struct { const char *label, *key; } k[] = {
        { "Rewind", "enhanced.rewind" },
        { "Skip intro", "enhanced.skip_intro" },
        { "Skip read text", "enhanced.skip_seen" },
        { "Quick text", "enhanced.quick_text" },
    };
    PcMenuItem it;
    int i;
    if (done) return;
    done = 1;
    memset(&it, 0, sizeof it);
    it.type = PC_MENU_HEADER;
    it.label = "Play assists";
    pc_menu_add(&it);
    for (i = 0; i < A_COUNT; i++) {
        memset(&it, 0, sizeof it);
        it.type = PC_MENU_TOGGLE;
        it.label = k[i].label;
        it.key = k[i].key;
        it.get = get_on;
        it.set = set_on;
        it.note = note;
        it.ctx = (void *)(long)i;
        pc_menu_add(&it);
    }
}
