/* SPDX-License-Identifier: GPL-3.0-or-later */
/* text_audit.c -- the drawn-text log of the English text audit
 * (tools/patch/audit_drawn_text.py).
 *
 * TOKI_TEXTLOG=<file> with language = en: every distinct string the patch
 * engine's set_kanji_string draws is appended to <file> as one JSON line
 * (frame, scene, step, sub-step, x, y, the string's address, its bytes).
 * It is the PC twin of the PS1 drawlog profile (config/patch/
 * english-drawlog.json, src/patch/drawlog/drawlog.c): the same hook word,
 * patch_draw_hook in src/patch/main/text_vwf.c, which the PS1 build pokes
 * with pt_draw_log and this file sets to pc_text_audit_draw.
 *
 * The address says where the string came from: a PS1 RAM address
 * (0x80000000-0x80200000: the executable, an overlay, a container buffer,
 * a formatter's buffer) or a HOST address -- a host copy of retail data
 * that the English executable never reaches (the class of the 2026-11-02
 * join_club_message bug); for those the host symbol is logged too (dladdr,
 * Linux builds; the executable links -rdynamic).
 *
 * Off (no variable, or Japanese): nothing is set, nothing is written; the
 * hook word keeps its "off" value 1 and the game is unchanged.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#if !defined(_WIN32)
#include <dlfcn.h>
#endif

#include "lang.h"

#define W __attribute__((weak))
extern unsigned int patch_draw_hook W;   /* src/patch/main/text_vwf.c */

#define RAM_U8(a)  (*(volatile unsigned char *)(uintptr_t)(a))
#define RAM_U32(a) (*(volatile unsigned int *)(uintptr_t)(a))

#define TA_MAX_TEXT 400
#define TA_HASH     (1u << 15)

static FILE *g_ta;
static unsigned long long g_seen[TA_HASH];   /* open addressing, 0 = empty */
static unsigned g_seen_n;

static unsigned long long ta_key(const unsigned char *s, size_t n) {
    unsigned long long h = 1469598103934665603ull ^ (unsigned long long)(uintptr_t)s;
    size_t i;
    for (i = 0; i < n; i++) h = (h ^ s[i]) * 1099511628211ull;
    return h ? h : 1;
}

/* 1 the first time (address, bytes) is seen */
static int ta_first(unsigned long long k) {
    unsigned i = (unsigned)(k & (TA_HASH - 1));
    if (g_seen_n >= TA_HASH - TA_HASH / 8) return 0;   /* full: stop logging */
    while (g_seen[i]) {
        if (g_seen[i] == k) return 0;
        i = (i + 1) & (TA_HASH - 1);
    }
    g_seen[i] = k;
    g_seen_n++;
    return 1;
}

void pc_text_audit_draw(short x, short y, unsigned char *str) {
    size_t n, i;
    unsigned long long k;
    uintptr_t a = (uintptr_t)str;
    const char *sym = "";
    if (!g_ta || !str) return;
    for (n = 0; n < TA_MAX_TEXT && str[n]; n++) {}
    k = ta_key(str, n);
    if (!ta_first(k)) return;
#if !defined(_WIN32)
    if (a < 0x80000000u || a >= 0x80200000u) {
        Dl_info di;
        if (dladdr(str, &di) && di.dli_sname) sym = di.dli_sname;
    }
#endif
    fprintf(g_ta, "{\"f\":%u,\"scene\":%u,\"step\":%u,\"sub\":%u,\"x\":%d,\"y\":%d,"
                  "\"addr\":\"0x%llx\",\"host\":%d,\"sym\":\"%s\",\"hex\":\"",
            RAM_U32(0x800EF858u), RAM_U8(0x800EF86Eu), RAM_U8(0x800EF868u),
            RAM_U8(0x800EF869u), (int)x, (int)y, (unsigned long long)a,
            (a < 0x80000000u || a >= 0x80200000u) ? 1 : 0, sym);
    for (i = 0; i < n; i++) fprintf(g_ta, "%02x", str[i]);
    fputs("\"}\n", g_ta);
    fflush(g_ta);
}

/* Called by hal_lang_boot_fixup once the engine is set up (English only). */
void pc_text_audit_init(void) {
    const char *path = getenv("TOKI_TEXTLOG");
    uintptr_t fn = (uintptr_t)&pc_text_audit_draw;
    if (!path || !*path || !lang_en() || !&patch_draw_hook) return;
    /* the hook word is 32 bits (a PS1 .data word in the patch C): the
     * native function must have a 32-bit address (non-PIE Linux link) */
    if (fn > 0xFFFFFFFFu || (fn & 3)) {
        printf("[TEXTLOG] hook address 0x%llx does not fit the 32-bit hook word; off\n",
               (unsigned long long)fn);
        return;
    }
    g_ta = fopen(path, "a");
    if (!g_ta) { perror("[TEXTLOG] open"); return; }
    patch_draw_hook = (unsigned int)fn;
    printf("[TEXTLOG] drawn text -> %s\n", path);
    fflush(stdout);
}
