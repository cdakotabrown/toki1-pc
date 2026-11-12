/* SPDX-License-Identifier: GPL-3.0-or-later */
/* asset_map.c — Asset manifest loader and lookup
 *
 * Loads pc/assets/asset_manifest.json at startup and provides:
 * - Asset info lookup by ID
 * - VRAM region queries
 * - HD texture path resolution (future)
 */
#include "hal.h"
#include "pc_data_root.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ASSETS 64
#define MAX_ID_LEN 32
#define MAX_PATH_LEN 128

typedef struct {
    char id[MAX_ID_LEN];
    char ps1_path[MAX_PATH_LEN];
    char type[16];
    s32 vram_x, vram_y;
    s32 width, height;
    s32 bpp;
    s32 clut_x, clut_y;
    char purpose[64];
    char hd_path[MAX_PATH_LEN];
    int loaded;
} AssetEntry;

static AssetEntry g_assets[MAX_ASSETS];
static int g_asset_count = 0;
static int g_manifest_loaded = 0;

/* Simple JSON string value extractor */
static int json_get_string(const char *json, const char *key, char *out, int max) {
    char pattern[64];
    const char *p, *start, *end;
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    p = strstr(json, pattern);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    while (*p && (*p == ':' || *p == ' ' || *p == '\t')) p++;
    if (*p == 'n' && strncmp(p, "null", 4) == 0) {
        out[0] = '\0';
        return 1;
    }
    if (*p != '"') return 0;
    start = p + 1;
    end = strchr(start, '"');
    if (!end) return 0;
    int len = (int)(end - start);
    if (len >= max) len = max - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    return 1;
}

/* Simple JSON integer extractor */
static int json_get_int(const char *json, const char *key, s32 *out) {
    char pattern[64];
    const char *p;
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    p = strstr(json, pattern);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    while (*p && (*p == ':' || *p == ' ' || *p == '\t')) p++;
    if (*p == '-' || (*p >= '0' && *p <= '9')) {
        *out = atoi(p);
        return 1;
    }
    return 0;
}

/* Parse a single asset object from JSON */
static int parse_asset(const char *obj_start, const char *obj_end, AssetEntry *entry) {
    char buf[512];
    int len = (int)(obj_end - obj_start);
    if (len >= (int)sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, obj_start, len);
    buf[len] = '\0';

    memset(entry, 0, sizeof(*entry));
    json_get_string(buf, "id", entry->id, MAX_ID_LEN);
    json_get_string(buf, "ps1_path", entry->ps1_path, MAX_PATH_LEN);
    json_get_string(buf, "type", entry->type, 16);
    json_get_int(buf, "vram_x", &entry->vram_x);
    json_get_int(buf, "vram_y", &entry->vram_y);
    json_get_int(buf, "width", &entry->width);
    json_get_int(buf, "height", &entry->height);
    json_get_int(buf, "bpp", &entry->bpp);
    json_get_int(buf, "clut_x", &entry->clut_x);
    json_get_int(buf, "clut_y", &entry->clut_y);
    json_get_string(buf, "purpose", entry->purpose, 64);
    json_get_string(buf, "hd_path", entry->hd_path, MAX_PATH_LEN);

    return entry->id[0] != '\0';
}

/* Load manifest from JSON file */
int asset_map_init(void) {
    FILE *f;
    char *data;
    long size;
    const char *p, *assets_start, *obj_start, *obj_end;
    int brace_depth;

    {
        char mp[1100];
        f = fopen(pc_asset_path(mp, sizeof mp, "asset_manifest.json"), "r");
    }
    if (!f) {
        printf("[ASSET_MAP] Cannot open asset_manifest.json\n");
        return -1;
    }

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);

    data = (char *)malloc(size + 1);
    if (!data) { fclose(f); return -1; }
    fread(data, 1, size, f);
    data[size] = '\0';
    fclose(f);

    /* Find "assets" array */
    assets_start = strstr(data, "\"assets\"");
    if (!assets_start) {
        printf("[ASSET_MAP] No 'assets' array in manifest\n");
        free(data);
        return -1;
    }

    p = strchr(assets_start, '[');
    if (!p) { free(data); return -1; }
    p++;

    /* Parse each object in the array */
    g_asset_count = 0;
    while (*p && g_asset_count < MAX_ASSETS) {
        /* Skip whitespace */
        while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',')) p++;
        if (*p == ']') break;
        if (*p != '{') { p++; continue; }

        obj_start = p;
        brace_depth = 1;
        p++;
        while (*p && brace_depth > 0) {
            if (*p == '{') brace_depth++;
            else if (*p == '}') brace_depth--;
            p++;
        }
        obj_end = p;

        if (parse_asset(obj_start, obj_end, &g_assets[g_asset_count])) {
            g_asset_count++;
        }
    }

    free(data);
    g_manifest_loaded = 1;
    printf("[ASSET_MAP] Loaded %d assets from manifest\n", g_asset_count);
    return 0;
}

/* Lookup asset by ID */
const char *asset_map_get_path(const char *id) {
    int i;
    for (i = 0; i < g_asset_count; i++) {
        if (strcmp(g_assets[i].id, id) == 0) {
            /* Return HD path if available, else PS1 path */
            if (g_assets[i].hd_path[0]) {
                return g_assets[i].hd_path;
            }
            return g_assets[i].ps1_path;
        }
    }
    return NULL;
}

/* Get VRAM location for asset */
int asset_map_get_vram(const char *id, s32 *x, s32 *y, s32 *w, s32 *h) {
    int i;
    for (i = 0; i < g_asset_count; i++) {
        if (strcmp(g_assets[i].id, id) == 0) {
            if (x) *x = g_assets[i].vram_x;
            if (y) *y = g_assets[i].vram_y;
            if (w) *w = g_assets[i].width;
            if (h) *h = g_assets[i].height;
            return 0;
        }
    }
    return -1;
}

/* Get CLUT location for asset */
int asset_map_get_clut(const char *id, s32 *x, s32 *y) {
    int i;
    for (i = 0; i < g_asset_count; i++) {
        if (strcmp(g_assets[i].id, id) == 0) {
            if (x) *x = g_assets[i].clut_x;
            if (y) *y = g_assets[i].clut_y;
            return 0;
        }
    }
    return -1;
}

/* Mark asset as loaded */
void asset_map_mark_loaded(const char *id) {
    int i;
    for (i = 0; i < g_asset_count; i++) {
        if (strcmp(g_assets[i].id, id) == 0) {
            g_assets[i].loaded = 1;
            return;
        }
    }
}

/* Check if asset is loaded */
int asset_map_is_loaded(const char *id) {
    int i;
    for (i = 0; i < g_asset_count; i++) {
        if (strcmp(g_assets[i].id, id) == 0) {
            return g_assets[i].loaded;
        }
    }
    return 0;
}

/* Debug: dump all assets */
void asset_map_dump(void) {
    int i;
    printf("[ASSET_MAP] === Asset Manifest ===\n");
    for (i = 0; i < g_asset_count; i++) {
        printf("  [%s] %s @ VRAM(%d,%d) %dx%d %dbpp CLUT(%d,%d) %s\n",
               g_assets[i].id,
               g_assets[i].ps1_path,
               g_assets[i].vram_x, g_assets[i].vram_y,
               g_assets[i].width, g_assets[i].height,
               g_assets[i].bpp,
               g_assets[i].clut_x, g_assets[i].clut_y,
               g_assets[i].loaded ? "[LOADED]" : "");
    }
}
