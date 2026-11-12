/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Runtime provenance logger for NBN archive data.
 *
 * NBN_TRACE=1 enables JSONL output. NBN_TRACE_LOG selects the output path
 * (default: nbn_trace.jsonl). The registry deliberately keeps only recent
 * loads: when game RAM is reused, the newest overlapping mapping wins. */
#include "nbn_trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define NBN_TRACE_MAPPINGS 128
#define NBN_TRACE_PATH_MAX 256

typedef struct {
    const unsigned char *start;
    u32 file_offset;
    u32 length;
    unsigned long generation;
    char path[NBN_TRACE_PATH_MAX];
} NbnTraceMapping;

static NbnTraceMapping g_mappings[NBN_TRACE_MAPPINGS];
static unsigned long g_generation;
static unsigned long g_event_id;
static int g_enabled = -1;

static int nbn_trace_enabled(void) {
    const char *value;
    if (g_enabled < 0) {
        value = getenv("NBN_TRACE");
        g_enabled = (value && *value && *value != '0') ? 1 : 0;
    }
    return g_enabled;
}

static int has_nbn_suffix(const char *path) {
    size_t length;
    if (!path) return 0;
    length = strlen(path);
    if (length < 4) return 0;
    path += length - 4;
    return (path[0] == '.' &&
            (path[1] == 'N' || path[1] == 'n') &&
            (path[2] == 'B' || path[2] == 'b') &&
            (path[3] == 'N' || path[3] == 'n'));
}

static FILE *open_log(void) {
    const char *path;
    FILE *file;
    path = getenv("NBN_TRACE_LOG");
    if (!path || !*path) path = "nbn_trace.jsonl";
    file = fopen(path, "a");
    return file;
}

static void write_json_string(FILE *file, const char *value) {
    const unsigned char *p;
    fputc('"', file);
    for (p = (const unsigned char *)value; p && *p; p++) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', file);
            fputc(*p, file);
        } else if (*p < 0x20) {
            fprintf(file, "\\u%04x", (unsigned int)*p);
        } else {
            fputc(*p, file);
        }
    }
    fputc('"', file);
}

void nbn_trace_register_load(const char *path, const void *dest,
                             u32 file_offset, u32 length) {
    unsigned long slot;
    NbnTraceMapping *mapping;
    FILE *file;
    const char *scene;
    if (!nbn_trace_enabled() || !has_nbn_suffix(path) || !dest || !length) return;
    g_generation++;
    slot = g_generation % NBN_TRACE_MAPPINGS;
    mapping = &g_mappings[slot];
    mapping->start = (const unsigned char *)dest;
    mapping->file_offset = file_offset;
    mapping->length = length;
    mapping->generation = g_generation;
    strncpy(mapping->path, path, sizeof(mapping->path) - 1);
    mapping->path[sizeof(mapping->path) - 1] = 0;

    file = open_log();
    if (!file) return;
    scene = getenv("SCENE");
    fprintf(file, "{\"event\":\"file_load\",\"event_id\":%lu,\"generation\":%lu,\"scene\":",
            ++g_event_id, g_generation);
    write_json_string(file, scene ? scene : "boot");
    fprintf(file, ",\"archive\":");
    write_json_string(file, mapping->path);
    fprintf(file, ",\"file_offset\":%u,\"length\":%u,\"ram_start\":\"0x%lx\"}\n",
            file_offset, length, (unsigned long)(uintptr_t)dest);
    fclose(file);
}

void nbn_trace_load_image(const void *data, u32 length,
                          s32 x, s32 y, s32 w_words, s32 h) {
    const unsigned char *start;
    const NbnTraceMapping *best;
    unsigned long i;
    FILE *file;
    const char *scene;
    if (!nbn_trace_enabled() || !data || !length) return;
    start = (const unsigned char *)data;
    best = NULL;
    for (i = 0; i < NBN_TRACE_MAPPINGS; i++) {
        const NbnTraceMapping *candidate;
        candidate = &g_mappings[i];
        if (!candidate->generation) continue;
        if (start < candidate->start) continue;
        if ((u32)(start - candidate->start) > candidate->length) continue;
        if (length > candidate->length - (u32)(start - candidate->start)) continue;
        if (!best || candidate->generation > best->generation) best = candidate;
    }
    if (!best) return;

    file = open_log();
    if (!file) return;
    scene = getenv("SCENE");
    fprintf(file, "{\"event\":\"load_image\",\"event_id\":%lu,\"generation\":%lu,\"scene\":",
            ++g_event_id, best->generation);
    write_json_string(file, scene ? scene : "boot");
    fprintf(file, ",\"archive\":");
    write_json_string(file, best->path);
    fprintf(file, ",\"source_offset\":%u,\"source_length\":%u,"
                  "\"rect\":{\"x\":%d,\"y\":%d,\"w_words\":%d,\"h\":%d}}\n",
            best->file_offset + (u32)(start - best->start), length,
            x, y, w_words, h);
    fclose(file);
}

void nbn_trace_move_image(s32 sx, s32 sy, s32 w_words, s32 h,
                          s32 dx, s32 dy) {
    FILE *file;
    const char *scene;
    if (!nbn_trace_enabled()) return;
    file = open_log();
    if (!file) return;
    scene = getenv("SCENE");
    fprintf(file, "{\"event\":\"move_image\",\"event_id\":%lu,\"scene\":",
            ++g_event_id);
    write_json_string(file, scene ? scene : "boot");
    fprintf(file, ",\"src\":{\"x\":%d,\"y\":%d,\"w_words\":%d,\"h\":%d},"
                  "\"dst\":{\"x\":%d,\"y\":%d}}\n",
            sx, sy, w_words, h, dx, dy);
    fclose(file);
}
