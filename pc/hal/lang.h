/* SPDX-License-Identifier: GPL-3.0-or-later */
/* lang.h -- the game-text language layer (docs/pc-port/TRANSLATION-ON-PC.md).
 *
 * [game] language = ja | en (settings.ini) or TOKI_LANG=ja|en.  ja is the
 * retail game, bit for bit: every function here is a no-op while
 * g_hal_lang_en is 0.  en serves the English copies of the changed disc
 * files from data/en/ (a checkout: pc/assets-en/; built from the player's own files by
 * pc/hal/translation.c) and routes the English patch's hooked entry points
 * to the natively compiled patch engine (the src/patch/main sources) through the
 * generated stubs (pc/tools/gen_lang_hooks.py).
 */
#ifndef PC_LANG_H
#define PC_LANG_H

#include <stddef.h>

/* 1 while English is active.  Read by the generated hook stubs. */
extern unsigned char g_hal_lang_en;

/* Decide the language and, for en, prepare <asset_dir>/en (applies the
 * embedded pack when a file is missing or stale).  Falls back to ja when
 * the English files cannot be made (message on stdout).  Call once, before
 * any asset is opened. */
void hal_lang_init(const char *asset_dir);

/* "ja" / "en": the requested language (env > settings), before any
 * fallback; for the settings menu and the tests. */
const char *hal_lang_requested(void);

/* The English root that goes with an asset dir: for the player's data
 * folder "<root>/data" it is "<root>/data/en" (2026-11-07; an existing
 * "<root>/assets-en" that could not be moved in yet is used instead); any
 * other dir gets the "-en" sibling ("pc/assets" -> "pc/assets-en"). */
void hal_lang_en_root(const char *asset_dir, char *out, size_t n);

/* If English is active and `path` names a file under some ".../raw/"
 * tree that has an English copy, return that copy's path (in `buf`);
 * otherwise return `path` unchanged. */
const char *hal_lang_path(const char *path, char *buf, size_t n);

/* After main() has laid out the PS1 RAM windows: re-apply the English
 * executable's bytes over windows that were seeded from elsewhere
 * (0x800C0000-0x800E0000 from the retail data blob), then import the
 * builder-filled patch-module data into the native engine. */
void hal_lang_boot_fixup(void);

/* Backlog (pc/hal/backlog_capture.c): the x of glyph cache cell `cell`
 * of string record `rec` at column `col` (proportional English cells keep
 * their own pixel offset), and the ASCII code of a Latin glyph's bits
 * (0 when it is not one). */
int hal_lang_glyph_x(int rec, int cell, int col, int retail_x);
unsigned short hal_lang_glyph_code(const unsigned char *bits);   /* 18 bytes */

/* The settings menu's GAME > LANGUAGE item (pc/hal/present.c calls it). */
void lang_menu_register(void);

/* ---- for HAL files that unit harnesses link WITHOUT lang.c ----
 * Weak references: absent, the language is Japanese and every call is the
 * identity.  Call sites use these, never the functions above directly. */
/* lang.c defines PC_LANG_DEFINE before including this: on PE a definition
 * in a file that also sees a weak declaration of it becomes a COFF weak
 * external, which the linker does not bind (docs/pc-port/WINDOWS-BUILD.md
 * "Symbol binding", WEAK). */
#ifdef PC_LANG_DEFINE
#define PC_LANG_WEAK
#else
#define PC_LANG_WEAK __attribute__((weak))
#endif
extern unsigned char g_hal_lang_en PC_LANG_WEAK;
extern const char *hal_lang_path(const char *, char *, size_t) PC_LANG_WEAK;
extern int hal_lang_glyph_x(int, int, int, int) PC_LANG_WEAK;
extern unsigned short hal_lang_glyph_code(const unsigned char *) PC_LANG_WEAK;

static inline int lang_en(void) { return &g_hal_lang_en && g_hal_lang_en; }
static inline const char *lang_path(const char *p, char *buf, size_t n) {
    return hal_lang_path ? hal_lang_path(p, buf, n) : p;
}
static inline int lang_glyph_x(int rec, int cell, int col, int retail_x) {
    return hal_lang_glyph_x ? hal_lang_glyph_x(rec, cell, col, retail_x) : retail_x;
}
static inline unsigned short lang_glyph_code(const unsigned char *bits) {
    return hal_lang_glyph_code ? hal_lang_glyph_code(bits) : 0;
}

#endif
