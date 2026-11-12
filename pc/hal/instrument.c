/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * instrument.c — per-function ENTER/EXIT trace hooks.
 *
 * Active in TRACE=1 builds of pc/Makefile. -finstrument-functions
 * inserts a call to __cyg_profile_func_enter on every function entry and
 * __cyg_profile_func_exit on every return, with the function's address.
 *
 * We resolve the address back to a name via dladdr() and print it. Pair
 * with AddressSanitizer to spot the exact function that triggers an OOB
 * read/write in the overlay code.
 *
 * Reach mode (REACH_LIST_FILE=<path>): log nothing per call; record each
 * distinct function entered and APPEND their names, one per line, to
 * <path> at exit (several processes may share one file; `%p` in the path
 * is the process id) -- the runtime
 * evidence of which compiled functions a run executes
 * (pc/tools/audit_ps1_wide_access.py --reach).  No PC_ prefix:
 * playthrough.py strips PC_* from the environment it hands the port.
 * pc/hal is not
 * instrumented (TRACE's exclude list).
 *
 * Output: each line is `[OVL-EN] <hexaddr> <funcname>` or `[OVL-EX] ...`.
 * The last `[OVL-EN]` before an ASan halt is the culprit.
 *
 * Filter: only print when the resolved name starts with `func_8013`,
 * `overlay_`, or matches a known scene/sub-state caller — otherwise the
 * volume is unusable. Set OVL_TRACE_ALL=1 to disable the filter.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#define __USE_GNU
#include <dlfcn.h>

static int g_filter = 1;       /* 0 = all functions, 1 = overlay/func_ only */
static int g_armed = 0;        /* gate: only trace after main() has called arm */
static FILE *g_log = NULL;
static int g_depth = 0;

/* reach mode: an open-addressing set of entered function addresses */
#define REACH_SLOTS (1u << 16)
static void *g_reach[REACH_SLOTS];
static const char *g_reach_path = NULL;

__attribute__((no_instrument_function))
static void reach_add(void *func) {
    unsigned long long h = ((unsigned long long)(uintptr_t)func >> 4) * 0x9E3779B97F4A7C15ull;
    unsigned i = (unsigned)(h >> 48) & (REACH_SLOTS - 1);
    while (g_reach[i] != NULL) {
        if (g_reach[i] == func) return;
        i = (i + 1) & (REACH_SLOTS - 1);
    }
    g_reach[i] = func;
}

/* Write the reach list (once).  The trace harness ends a run with _exit()
 * (pc/hal/trace.c), which skips atexit, so it calls this first. */
__attribute__((no_instrument_function))
void instrument_reach_flush(void) {
    FILE *f;
    unsigned i;
    Dl_info info;
    if (g_armed != 2 || !g_reach_path || !(f = fopen(g_reach_path, "a"))) return;
    g_armed = 0;
    for (i = 0; i < REACH_SLOTS; i++) {
        if (!g_reach[i]) continue;
        if (dladdr(g_reach[i], &info) && info.dli_sname)
            fprintf(f, "%s\n", info.dli_sname);
        else
            fprintf(f, "%p\n", g_reach[i]);
    }
    fclose(f);
}

/* REACH_LIST_FILE may contain `%p`, replaced by the process id, so that
 * runs sharing one environment (the scene matrix's parallel jobs) write
 * one file each instead of interleaving appends in one file
 * (pc/tools/coverage_report.py).  The id survives the layout re-exec. */
static char g_reach_buf[4096];
static int g_reach_checked = 0;

__attribute__((no_instrument_function))
static const char *reach_path_expand(const char *p) {
    const char *pc = p ? strstr(p, "%p") : NULL;
    if (!pc || strlen(p) + 24 >= sizeof g_reach_buf) return p;
    snprintf(g_reach_buf, sizeof g_reach_buf, "%.*s%ld%s",
             (int)(pc - p), p, (long)getpid(), pc + 2);
    return g_reach_buf;
}

/* Reach mode only stores addresses (no stdio, no dladdr) until the flush,
 * so it arms itself at the first instrumented call: the part of the boot
 * chain main() runs before instrument_arm (graph_init, ...) counts too. */
__attribute__((no_instrument_function))
static void reach_arm(void) {
    if (g_reach_checked) return;
    g_reach_checked = 1;
    g_reach_path = reach_path_expand(getenv("REACH_LIST_FILE"));
    if (g_reach_path) {
        atexit(instrument_reach_flush);
        g_armed = 2;
    }
}

/* Called explicitly from main() once stdio/dlfcn are safe. Until then
 * the hooks are no-ops (reach mode excepted, above). */
__attribute__((no_instrument_function))
void instrument_arm(void) {
    reach_arm();
    if (g_reach_path) return;
    if (getenv("OVL_TRACE_ALL")) g_filter = 0;
    const char *path = getenv("OVL_TRACE_FILE");
    g_log = path ? fopen(path, "w") : stderr;
    if (!g_log) g_log = stderr;
    g_armed = 1;
}

__attribute__((no_instrument_function))
void instrument_disarm(void) { g_armed = 0; }

__attribute__((no_instrument_function))
static int interesting(const char *name) {
    if (!g_filter) return 1;
    if (!name) return 0;
    /* Overlay code lives in func_8013... */
    if (strncmp(name, "func_8013", 9) == 0) return 1;
    if (strncmp(name, "overlay_", 8) == 0) return 1;
    /* Main-binary boot-chain helpers: keep visibility on these too */
    if (strncmp(name, "func_8004", 9) == 0) return 1;
    if (strncmp(name, "func_8005", 9) == 0) return 1;
    if (strncmp(name, "func_8006", 9) == 0) return 1;
    return 0;
}

__attribute__((no_instrument_function))
void __cyg_profile_func_enter(void *func, void *caller) {
    Dl_info info;
    if (!g_armed) {
        reach_arm();
        if (!g_armed) return;
    }
    if (g_armed == 2) { reach_add(func); return; }
    if (!dladdr(func, &info) || !info.dli_sname) return;
    if (!interesting(info.dli_sname)) return;
    fprintf(g_log, "[OVL-EN]%*s %p %s\n",
            g_depth < 20 ? g_depth : 20, "",
            func, info.dli_sname);
    fflush(g_log);
    g_depth++;
}

__attribute__((no_instrument_function))
void __cyg_profile_func_exit(void *func, void *caller) {
    Dl_info info;
    if (!g_armed || g_armed == 2) return;
    if (!dladdr(func, &info) || !info.dli_sname) return;
    if (!interesting(info.dli_sname)) return;
    if (g_depth > 0) g_depth--;
    fprintf(g_log, "[OVL-EX]%*s %p %s\n",
            g_depth < 20 ? g_depth : 20, "",
            func, info.dli_sname);
    fflush(g_log);
}
