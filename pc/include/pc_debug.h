/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pc_debug.h — Centralized debug logging for PC port
 *
 * Usage:
 *   #include "pc_debug.h"
 *   PC_DEBUG("format %d", value);      // General debug
 *   PC_DEBUG_FMV("FMV state %d", s);   // FMV-specific
 *   PC_DEBUG_GPU("GPU op %s", name);   // GPU-specific
 *   PC_DEBUG_SCENE("scene %d", n);     // Scene transitions
 *
 * Environment variables:
 *   PC_DEBUG=1        Enable all debug output
 *   PC_DEBUG=fmv,gpu  Enable specific categories (comma-separated)
 *   PC_DEBUG=0        Disable all (default)
 *
 * Categories: fmv, gpu, scene, render, sound, mem, all
 */
#ifndef PC_DEBUG_H
#define PC_DEBUG_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Debug category flags */
#define PC_DBG_FMV    (1 << 0)
#define PC_DBG_GPU    (1 << 1)
#define PC_DBG_SCENE  (1 << 2)
#define PC_DBG_RENDER (1 << 3)
#define PC_DBG_SOUND  (1 << 4)
#define PC_DBG_MEM    (1 << 5)
#define PC_DBG_PAD    (1 << 6)
#define PC_DBG_ALL    0xFFFFFFFF

/* Global debug flags - initialized once at startup */
extern unsigned int g_pc_debug_flags;
extern int g_pc_debug_initialized;

/* Initialize debug flags from PC_DEBUG env var */
static inline void pc_debug_init(void) {
    const char *env;
    if (g_pc_debug_initialized) return;
    g_pc_debug_initialized = 1;
    g_pc_debug_flags = 0;

    env = getenv("PC_DEBUG");
    if (!env || !*env || strcmp(env, "0") == 0) return;

    if (strcmp(env, "1") == 0 || strcmp(env, "all") == 0) {
        g_pc_debug_flags = PC_DBG_ALL;
        return;
    }

    /* Parse comma-separated categories */
    if (strstr(env, "fmv"))    g_pc_debug_flags |= PC_DBG_FMV;
    if (strstr(env, "gpu"))    g_pc_debug_flags |= PC_DBG_GPU;
    if (strstr(env, "scene"))  g_pc_debug_flags |= PC_DBG_SCENE;
    if (strstr(env, "render")) g_pc_debug_flags |= PC_DBG_RENDER;
    if (strstr(env, "sound"))  g_pc_debug_flags |= PC_DBG_SOUND;
    if (strstr(env, "mem"))    g_pc_debug_flags |= PC_DBG_MEM;
    if (strstr(env, "pad"))    g_pc_debug_flags |= PC_DBG_PAD;
}

/* Check if a category is enabled */
#define PC_DBG_ENABLED(cat) (g_pc_debug_initialized ? (g_pc_debug_flags & (cat)) : (pc_debug_init(), g_pc_debug_flags & (cat)))

/* Category-specific debug macros */
#define PC_DEBUG_CAT(cat, tag, fmt, ...) \
    do { if (PC_DBG_ENABLED(cat)) { fprintf(stderr, "[" tag "] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } } while(0)

#define PC_DEBUG_FMV(fmt, ...)    PC_DEBUG_CAT(PC_DBG_FMV,    "FMV",    fmt, ##__VA_ARGS__)
#define PC_DEBUG_GPU(fmt, ...)    PC_DEBUG_CAT(PC_DBG_GPU,    "GPU",    fmt, ##__VA_ARGS__)
#define PC_DEBUG_SCENE(fmt, ...)  PC_DEBUG_CAT(PC_DBG_SCENE,  "SCENE",  fmt, ##__VA_ARGS__)
#define PC_DEBUG_RENDER(fmt, ...) PC_DEBUG_CAT(PC_DBG_RENDER, "RENDER", fmt, ##__VA_ARGS__)
#define PC_DEBUG_SOUND(fmt, ...)  PC_DEBUG_CAT(PC_DBG_SOUND,  "SOUND",  fmt, ##__VA_ARGS__)
#define PC_DEBUG_MEM(fmt, ...)    PC_DEBUG_CAT(PC_DBG_MEM,    "MEM",    fmt, ##__VA_ARGS__)
#define PC_DEBUG_PAD(fmt, ...)    PC_DEBUG_CAT(PC_DBG_PAD,    "PAD",    fmt, ##__VA_ARGS__)

/* General debug (always prints if any debug is enabled) */
#define PC_DEBUG(fmt, ...) \
    do { if (g_pc_debug_flags) { fprintf(stderr, "[DEBUG] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } } while(0)

#endif /* PC_DEBUG_H */
