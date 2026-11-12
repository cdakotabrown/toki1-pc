/* SPDX-License-Identifier: GPL-3.0-or-later */
/* lang.c -- the game-text language layer (see lang.h).
 *
 * Japanese is the retail game: with g_hal_lang_en == 0 every entry point
 * here returns at once, the generated hook stubs jump straight to the
 * retail functions and no file path changes.
 */
#define PC_LANG_DEFINE      /* strong definitions on PE (lang.h) */
#include "lang.h"
#include "translation.h"
#include "pc_settings.h"
#include "pc_menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

unsigned char g_hal_lang_en;

static char g_en_root[512];
static char g_ja_root[512];     /* the asset dir the English root goes with */
static char g_req[4] = "ja";
static int g_lang_failed;      /* English requested, preparation failed */

/* a note (queued, timed while on screen: pc/include/pc_present.h "Notes") */
extern void pc_present_note(const char *text, unsigned ms) __attribute__((weak));

extern const PcSettings *pc_settings_get(void) __attribute__((weak));

const char *hal_lang_requested(void) { return g_req; }

static int is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

void hal_lang_en_root(const char *asset_dir, char *out, size_t n) {
    size_t len = strlen(asset_dir);
    const char *base;
    char legacy[600];
    while (len > 1 && (asset_dir[len - 1] == '/' || asset_dir[len - 1] == '\\')) len--;
    for (base = asset_dir + len; base > asset_dir && base[-1] != '/' && base[-1] != '\\'; base--) { }
    if ((size_t)(asset_dir + len - base) != 4 || strncmp(base, "data", 4)) {
        /* any other asset dir (the developer's pc/assets, a folder given
         * on the command line, an old install's assets/ that could not be
         * moved): the "-en" sibling, as before 2026-11-07 */
        snprintf(out, n, "%.*s-en", (int)len, asset_dir);
        return;
    }
    /* the data folder: data/en; while an old install's assets-en beside
     * it could not be moved in, that one */
    snprintf(out, n, "%.*s/en", (int)len, asset_dir);
    if (is_dir(out)) return;
    snprintf(legacy, sizeof legacy, "%.*sassets-en", (int)(base - asset_dir), asset_dir);
    if (is_dir(legacy)) snprintf(out, n, "%s", legacy);
}

static void decide(void) {
    const char *v = getenv("TOKI_LANG");
    if (v && *v) {
        snprintf(g_req, sizeof g_req, "%s", strcasecmp(v, "en") == 0 ? "en" : "ja");
        return;
    }
    if (pc_settings_get && pc_settings_get()->language == 1) strcpy(g_req, "en");
}

void hal_lang_init(const char *asset_dir) {
    char root[512];
    g_hal_lang_en = 0;
    g_lang_failed = 0;
    decide();
    if (strcmp(g_req, "en") != 0) return;
    hal_lang_en_root(asset_dir, g_en_root, sizeof g_en_root);
    snprintf(root, sizeof root, "%s", asset_dir);
    snprintf(g_ja_root, sizeof g_ja_root, "%s", asset_dir);
    if (translation_prepare(root, g_en_root) != 0) {
        /* No pack in this build and no complete English tree, or the
         * player's files are not the supported disc: stay Japanese. */
        size_t n = 0;
        char probe[600];
        snprintf(probe, sizeof probe, "%s/raw/PSX.EXE", g_en_root);
        if (translation_pack_data(&n) || access(probe, R_OK) != 0) {
            /* never silent: the player asked for English (the line above
             * says why it failed; hal_lang_boot_fixup shows it on screen) */
            g_lang_failed = 1;
            printf("[LANG] ERROR: English was requested but its files could not be "
                   "prepared in %s (see the [TRANSLATION] line); playing in Japanese\n",
                   g_en_root);
            fflush(stdout);
            return;
        }
        printf("[LANG] no translation pack in this build; using the existing %s\n", g_en_root);
    }
    g_hal_lang_en = 1;
    printf("[LANG] English (files from %s)\n", g_en_root);
    fflush(stdout);
}

const char *hal_lang_path(const char *path, char *buf, size_t n) {
    const char *raw = NULL, *p;
    if (!g_hal_lang_en || !path) return path;
    /* the last "raw/" path component: "<any asset dir>/raw/<file>" ->
     * "<the prepared English root>/raw/<file>".  The HAL names the asset
     * tree several ways ("./pc/assets/raw", the launcher's absolute
     * asset dir); there is one tree, and one English root beside it. */
    for (p = path; (p = strstr(p, "raw/")) != NULL; p++)
        if (p == path || p[-1] == '/') raw = p;
    if (!raw || raw == path) return path;
    snprintf(buf, n, "%s/%s", g_en_root, raw);
    if (access(buf, R_OK) == 0) return buf;
    return path;
}

/* ------------------------------------------------------------ boot fixup */
extern void pc_text_audit_init(void) __attribute__((weak));
static unsigned char *read_exe(const char *path, unsigned int *load, unsigned int *size) {
    FILE *f = fopen(path, "rb");
    unsigned char hdr[0x800], *buf;
    if (!f) return NULL;
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr || memcmp(hdr, "PS-X EXE", 8) != 0) {
        fclose(f);
        return NULL;
    }
    *load = (unsigned int)hdr[0x18] | (unsigned int)hdr[0x19] << 8 | (unsigned int)hdr[0x1A] << 16 | (unsigned int)hdr[0x1B] << 24;
    *size = (unsigned int)hdr[0x1C] | (unsigned int)hdr[0x1D] << 8 | (unsigned int)hdr[0x1E] << 16 | (unsigned int)hdr[0x1F] << 24;
    buf = (unsigned char *)malloc(*size);
    if (buf && fread(buf, 1, *size, f) != *size) { free(buf); buf = NULL; }
    fclose(f);
    return buf;
}

/* The engine's builder-filled data (src/patch/main/text_vwf.c). */
#define W __attribute__((weak))
extern unsigned char *patch_dte_table[] W;
extern unsigned char *patch_str_table[] W;
extern unsigned short *patch_kanji_sub_codes W;
extern unsigned char *patch_kanji_sub_glyphs W;
extern int patch_kanji_sub_count W;
extern int patch_kanji_limit W;
#define PT_DTE_COUNT 79
#define PT_STR_COUNT 64

static unsigned int ram_u32(unsigned int addr) {
    return *(volatile unsigned int *)(unsigned long)addr;
}
static void *ram_ptr(unsigned int addr) {
    unsigned int v = ram_u32(addr);
    return v ? (void *)(unsigned long)v : NULL;
}

/* "@module.syms" in the pack: "<name> 0x<ps1 address>" lines. */
static unsigned int module_sym(const char *text, const char *name) {
    const char *p = text;
    size_t n = strlen(name);
    while (p && *p) {
        if (!strncmp(p, name, n) && p[n] == ' ') return (unsigned int)strtoul(p + n + 1, NULL, 16);
        p = strchr(p, '\n');
        if (p) p++;
    }
    return 0;
}

static void import_module_data(void) {
    size_t psz = 0, tsz = 0;
    const unsigned char *pack = translation_pack_data(&psz);
    unsigned char *txt = pack ? translation_apply_file(pack, psz, "@module.syms", NULL, 0, &tsz) : NULL;
    unsigned int a;
    int k;
    if (!patch_dte_table || !patch_str_table || !&patch_kanji_limit || !&patch_kanji_sub_count
        || !&patch_kanji_sub_codes || !&patch_kanji_sub_glyphs) {
        free(txt);
        printf("[LANG] this build has no patch engine\n");
        return;
    }
    if (!txt) {
        printf("[LANG] no module symbol table in the pack: English text cannot be decoded\n");
        return;
    }
    txt = (unsigned char *)realloc(txt, tsz + 1);
    txt[tsz] = 0;
    if ((a = module_sym((char *)txt, "patch_dte_table")))
        for (k = 0; k < PT_DTE_COUNT; k++) patch_dte_table[k] = (unsigned char *)ram_ptr(a + 4 * k);
    if ((a = module_sym((char *)txt, "patch_str_table")))
        for (k = 0; k < PT_STR_COUNT; k++) patch_str_table[k] = (unsigned char *)ram_ptr(a + 4 * k);
    if ((a = module_sym((char *)txt, "patch_kanji_sub_codes")))
        patch_kanji_sub_codes = (unsigned short *)ram_ptr(a);
    if ((a = module_sym((char *)txt, "patch_kanji_sub_glyphs")))
        patch_kanji_sub_glyphs = (unsigned char *)ram_ptr(a);
    if ((a = module_sym((char *)txt, "patch_kanji_sub_count")))
        patch_kanji_sub_count = (int)ram_u32(a);
    if ((a = module_sym((char *)txt, "patch_kanji_limit")))
        patch_kanji_limit = (int)ram_u32(a);
    printf("[LANG] patch module data imported (kanji limit %d, subset %d)\n",
           patch_kanji_limit, patch_kanji_sub_count);
    free(txt);
}

void hal_lang_boot_fixup(void) {
    char en[600], ja[600];
    unsigned char *e, *j;
    unsigned int le, se, lj, sj, a;
    if (g_lang_failed && pc_present_note)
        pc_present_note("English unavailable: playing in Japanese (see the log)", 10000);
    if (!g_hal_lang_en) return;
    snprintf(en, sizeof en, "%s/raw/PSX.EXE", g_en_root);
    /* the Japanese executable main.c loaded */
    snprintf(ja, sizeof ja, "%s/raw/PSX.EXE", g_ja_root);
    e = read_exe(en, &le, &se);
    j = read_exe(ja, &lj, &sj);
    if (e && j && le == lj && se == sj) {
        /* main.c re-seeds 0x800C0000-0x800E0000 from the retail data blob
         * after loading the executable: put the English bytes back there
         * (the patch module and its string pool live at 0x800BF3E8..). */
        unsigned int lo = 0x800C0000u, hi = 0x800E0000u, changed = 0;
        for (a = lo; a < hi; a++) {
            unsigned int off = a - le;
            if (a < le || off >= se) continue;
            if (e[off] != j[off]) {
                *(volatile unsigned char *)(unsigned long)a = e[off];
                changed++;
            }
        }
        printf("[LANG] boot fixup: %u English bytes re-applied in 0x800C0000-0x800E0000\n", changed);
    } else {
        printf("[LANG] boot fixup: cannot read %s / %s\n", en, ja);
    }
    free(e);
    free(j);
    import_module_data();
    /* the drawn-text audit log (TOKI_TEXTLOG; pc/hal/text_audit.c) */
    if (pc_text_audit_init) pc_text_audit_init();
    fflush(stdout);
}

/* ------------------------------------------------------------ backlog */
extern unsigned char patch_entry_adv[] W;
extern unsigned short patch_glyph_x[] W;
extern const unsigned short patch_font_rows[95][12] W;

int hal_lang_glyph_x(int rec, int cell, int col, int retail_x) {
    if (!g_hal_lang_en || !patch_entry_adv || rec < 0 || rec > 52 || cell < 0 || cell >= 384)
        return retail_x;
    if (patch_entry_adv[rec] == 0xFF) return patch_glyph_x[cell] & 0x7FFF;
    if (patch_entry_adv[rec]) return col * patch_entry_adv[rec];
    return retail_x;
}

unsigned short hal_lang_glyph_code(const unsigned char *bits) {
    int c, r, k;
    if (!g_hal_lang_en || !patch_font_rows) return 0;
    for (k = 0; k < 18 && !bits[k]; k++)
        ;
    if (k == 18) return 0x20;                  /* a blank cell: a space */
    for (c = 1; c < 95; c++) {
        unsigned char g[18];
        memset(g, 0, sizeof g);
        for (r = 0; r < 12; r++)
            for (k = 0; k < 12; k++)
                if (patch_font_rows[c][r] & (0x8000 >> k)) {
                    int bit = r * 12 + k;
                    g[bit >> 3] |= (unsigned char)(0x80 >> (bit & 7));
                }
        if (!memcmp(g, bits, 18)) return (unsigned short)(0x20 + c);
    }
    return 0;
}

/* ------------------------------------------------------------ settings menu */
/* GAME > LANGUAGE: [game] language, applied on the next launch (the asset
 * tree and the routing are chosen at start-up). */
extern PcSettings *pc_settings_mut(void) __attribute__((weak));

static int menu_get(void *ctx) {
    (void)ctx;
    return pc_settings_get && pc_settings_get()->language == 1;
}
static void menu_set(void *ctx, int v) {
    (void)ctx;
    if (pc_settings_mut) pc_settings_mut()->language = v ? 1 : 0;
}
static const char *menu_note(void *ctx) {
    (void)ctx;
    if (menu_get(NULL) != (int)g_hal_lang_en) return "On restart";
    return NULL;
}

void lang_menu_register(void) {
    static const char *const names[] = { "Japanese", "English", NULL };
    PcMenuItem it;
    memset(&it, 0, sizeof it);
    it.type = PC_MENU_CHOICE;
    it.label = "Language";
    it.key = "game.language";
    it.choices = names;
    it.flags = PC_MENU_RESTART;
    it.get = menu_get;
    it.set = menu_set;
    it.note = menu_note;
    pc_menu_add(&it);
}
