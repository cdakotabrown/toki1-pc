/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_status_keys.c -- the status page's settings (pc/hal/settings.c):
 * [hotkeys] status = Ctrl+I, [enhanced] status = true, status_hud = false,
 * and no default key bound twice (a hotkey that another hotkey or a game
 * button already uses would never reach the status page).  No SDL. */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "pc_settings.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "  %s:%d: %s\n", __FILE__, __LINE__, #c); return __LINE__; } } while (0)

/* every token of every hotkey and keyboard binding, case-blind: the index
 * of the first token seen twice, or -1 */
static int first_duplicate(char lists[][PC_BIND_MAX], int n) {
    static char seen[256][64];
    char tok[8][64];
    int i, j, k, ns = 0;
    for (i = 0; i < n; i++) {
        int m = pc_bind_tokens(lists[i], tok, 8);
        for (j = 0; j < m; j++) {
            if (!strcasecmp(tok[j], "none")) continue;
            for (k = 0; k < ns; k++)
                if (!strcasecmp(seen[k], tok[j])) return i;
            if (ns < 256) snprintf(seen[ns++], sizeof seen[0], "%s", tok[j]);
        }
    }
    return -1;
}

int test_status_keys(void) {
    static PcSettings s;
    static char lists[PC_HK_COUNT + PC_BTN_COUNT][PC_BIND_MAX];
    int i, n = 0;
    pc_settings_defaults(&s);
    CHECK(!strcmp(s.hotkey[PC_HK_STATUS], "Ctrl+I"));
    CHECK(!strcmp(pc_hotkey_names[PC_HK_STATUS], "status"));
    CHECK(s.status == 1);
    CHECK(s.status_hud == 0);
    CHECK(pc_settings_parse(&s, "[enhanced]\nstatus = false\nstatus_hud = true\n", "t") == 0);
    CHECK(s.status == 0 && s.status_hud == 1);
    CHECK(pc_settings_set(&s, "hotkeys.status", "Ctrl+Shift+I") == 0);
    CHECK(!strcmp(s.hotkey[PC_HK_STATUS], "Ctrl+Shift+I"));

    pc_settings_defaults(&s);
    for (i = 0; i < PC_HK_COUNT; i++) snprintf(lists[n++], PC_BIND_MAX, "%s", s.hotkey[i]);
    for (i = 0; i < PC_BTN_COUNT; i++) snprintf(lists[n++], PC_BIND_MAX, "%s", s.key_bind[i]);
    CHECK(first_duplicate(lists, n) == -1);
    /* negative control: the same check sees a clash (Ctrl+I given to the
     * save browser as well) */
    snprintf(lists[PC_HK_SAVES], PC_BIND_MAX, "Ctrl+S, ctrl+i");
    CHECK(first_duplicate(lists, n) == PC_HK_STATUS);
    return 0;
}
