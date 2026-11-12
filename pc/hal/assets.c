/* SPDX-License-Identifier: GPL-3.0-or-later */
/* assets.c — Asset loading system for PC port
 *
 * Maps PS1 disc file paths to extracted PC asset paths.
 * The game code references files by their ISO 9660 paths (e.g., "\\CDROM\\EXEDIR\\OPTION.EXN;1").
 * We resolve these to the extracted directory structure under pc/assets/.
 *
 * Assets are extracted once from the user's original disc via extract_assets.py.
 * They are NOT shipped with the source code (anti-piracy).
 */
#include "hal.h"
#include "pc_data_root.h"
#include "lang.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static char g_asset_base[512] = "pc/assets";

void assets_init(const char *base_path) {
    strncpy(g_asset_base, base_path, sizeof(g_asset_base) - 1);
    /* the one root every reader uses (pc/include/pc_data_root.h) */
    pc_asset_root_set(base_path);
    printf("[Assets] Base path: %s\n", g_asset_base);
}

/* Resolve a PS1 disc path to a local file path.
 * Input: "\\CDROM\\EXEDIR\\OPTION.EXN;1" or "CDROM/EXEDIR/OPTION.EXN"
 * Output: "pc/assets/raw/CDROM/EXEDIR/OPTION.EXN" */
static s32 resolve_path(const char *ps1_path, char *out, s32 max_len);

/* [game] language = en: a resolved file with an English copy is served
 * from <assets>-en (pc/hal/lang.c); a no-op for Japanese. */
s32 assets_resolve_path(const char *ps1_path, char *out, s32 max_len) {
    char en[1024];
    const char *p;
    if (!resolve_path(ps1_path, out, max_len)) return 0;
    p = lang_path(out, en, sizeof en);
    if (p != out) snprintf(out, max_len, "%s", p);
    return 1;
}

static s32 resolve_path(const char *ps1_path, char *out, s32 max_len) {
    char clean[256];
    s32 i, j;

    /* Clean up the path: strip leading backslash, convert \\ to /, strip ;1 version */
    j = 0;
    for (i = 0; ps1_path[i] && j < 255; i++) {
        if (ps1_path[i] == '\\') {
            if (j > 0) clean[j++] = '/';
        } else if (ps1_path[i] == ';') {
            break; /* strip version number */
        } else {
            clean[j++] = ps1_path[i];
        }
    }
    clean[j] = 0;

    /* Try the raw extraction directory first */
    snprintf(out, max_len, "%s/raw/%s", g_asset_base, clean);
    {
        FILE *f = fopen(out, "rb");
        if (f) { fclose(f); return 1; }
    }

    /* Try without the CDROM prefix */
    if (strncmp(clean, "CDROM/", 6) == 0) {
        snprintf(out, max_len, "%s/raw/CDROM/%s", g_asset_base, clean + 6);
        FILE *f = fopen(out, "rb");
        if (f) { fclose(f); return 1; }
    }

    /* Try organized subdirectories */
    {
        const char *ext = strrchr(clean, '.');
        if (ext) {
            if (strcmp(ext, ".EXN") == 0) {
                snprintf(out, max_len, "%s/overlays/%s", g_asset_base, strrchr(clean, '/') ? strrchr(clean, '/') + 1 : clean);
            } else if (strcmp(ext, ".NBN") == 0) {
                snprintf(out, max_len, "%s/nbn/%s", g_asset_base, strrchr(clean, '/') ? strrchr(clean, '/') + 1 : clean);
            } else if (strcmp(ext, ".TIM") == 0) {
                snprintf(out, max_len, "%s/textures/%s", g_asset_base, strrchr(clean, '/') ? strrchr(clean, '/') + 1 : clean);
            } else if (strcmp(ext, ".VH") == 0 || strcmp(ext, ".VB") == 0 || strcmp(ext, ".SEQ") == 0) {
                snprintf(out, max_len, "%s/music/%s", g_asset_base, strrchr(clean, '/') ? strrchr(clean, '/') + 1 : clean);
            } else if (strcmp(ext, ".XA") == 0) {
                snprintf(out, max_len, "%s/audio/%s", g_asset_base, strrchr(clean, '/') ? strrchr(clean, '/') + 1 : clean);
            } else if (strcmp(ext, ".STR") == 0) {
                snprintf(out, max_len, "%s/video/%s", g_asset_base, strrchr(clean, '/') ? strrchr(clean, '/') + 1 : clean);
            }
            FILE *f = fopen(out, "rb");
            if (f) { fclose(f); return 1; }
        }
    }

    return 0; /* not found */
}

/* Load an entire file into a malloc'd buffer. Caller must free(). */
u8 *assets_load_file(const char *ps1_path, s32 *out_size) {
    char path[512];
    FILE *f;
    u8 *buf;
    s32 size;

    if (!assets_resolve_path(ps1_path, path, sizeof(path))) {
        printf("[Assets] Not found: %s\n", ps1_path);
        if (out_size) *out_size = 0;
        return NULL;
    }

    f = fopen(path, "rb");
    if (!f) {
        if (out_size) *out_size = 0;
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);

    buf = (u8 *)malloc(size);
    if (!buf) {
        fclose(f);
        if (out_size) *out_size = 0;
        return NULL;
    }

    fread(buf, 1, size, f);
    fclose(f);

    if (out_size) *out_size = size;
    return buf;
}

/* Load a file into a pre-allocated buffer (for CdRead-style loading) */
s32 assets_load_to_buffer(const char *ps1_path, void *buf, s32 max_size) {
    char path[512];
    FILE *f;
    s32 n;

    if (!assets_resolve_path(ps1_path, path, sizeof(path))) {
        return 0;
    }

    f = fopen(path, "rb");
    if (!f) return 0;

    n = fread(buf, 1, max_size, f);
    fclose(f);
    return n;
}
