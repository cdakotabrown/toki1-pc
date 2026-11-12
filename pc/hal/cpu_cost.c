/* SPDX-License-Identifier: GPL-3.0-or-later */
/* cpu_cost.c -- the CPU time game code takes on the console.
 *
 * The port runs the game's C natively, in no emulated time; only the HAL's
 * SDK/BIOS/device models advance the clock.  On the console the same code
 * runs on a 33.8 MHz CPU, and vblank_function turns its time into frames:
 * after the frame's work it calls VSync(0) only while GsGetVcount() <
 * vcount_lim, so a frame whose work runs past the next VBlank takes two
 * (or more) VBlanks and frame_counter falls behind the VBlank count.
 * Measured on the reference emulator (Interpreter) over the scripted new game
 * (pc/tools/playthrough/new_game.txt): a name-entry frame spends ~8,000
 * ticks in game_main and ~340,000 in vblank_function's sprite/text sort
 * (sprite_slot_disp -> GsSortSprite -> _make_packet/search_tpage8 ...,
 * disp_string); a kana entry adds 270,000..930,000 (glyph expansion and a
 * DrawSync behind the GPU's drawing, pc/hal/gpu.c gpu_prim_ticks) and the
 * frame takes two VBlanks.  With game code free the port never overran
 * there and ran one frame ahead per kana.
 *
 * The model: every function in a game object (matched, semi, archive,
 * overlay and pc-port sources; pc/Makefile COST_OBJS) is compiled with
 * `-pg -mfentry`, which makes its first instruction `call __fentry__`,
 * before the prologue and before any argument register is touched (the
 * matched code relies on MIPS-style argument passthrough: gnsx calls
 * strlen() with no arguments; -finstrument-functions loads the hook's
 * arguments into those registers and broke it).  __fentry__ below saves
 * every argument register, and the port charges hal_cpu_spend(the retail
 * time from that function's entry to the next function entry), the mean
 * over the measured runs in the current scene (now_step) where measured
 * often enough, else over all scenes (pc/tools/measure_cpu_costs.py, table
 * pc/hal/cpu_cost_table.c).  When both sides enter the same functions in
 * the same order, this reproduces retail's time between consecutive
 * entries and where it falls.  Functions the port implements in the HAL
 * (VSync, DrawSync, LoadImage, the CD and card BIOS, ...) are not
 * instrumented: retail's probe on them only closes the previous gap, and
 * their own time is the HAL's measured model, as before.  Overlay
 * functions are not in the table (overlay addresses are shared; their
 * time folds into the main-binary function before them, on both sides).
 * Contexts within a scene are averaged: a function costs its mean.  The
 * HAL's replacements of pure-CPU SDK functions charge by name
 * (hal_cpu_cost_named).  A function never measured costs nothing.
 *
 * Interrupt handlers are not charged: the exception's own time is already a
 * measured constant (pc/hal/system.c VBLANK_EXCEPTION_TICKS, cd.c), and
 * game callbacks run from inside HAL dispatch (cd.c event dispatch, irq.c
 * handler delivery).  GAME_CPU_COST=0 turns the model off (negative
 * control; no PC_ prefix because playthrough.py strips those).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#define PC_CPU_COST_DEFINE   /* strong definitions (cpu_cost.h) */
#include "cpu_cost.h"
#include "profile.h"

extern void hal_cpu_spend(unsigned ticks);
extern int hal_irq_in_handler(void);
extern int hal_cd_dispatching(void);
extern unsigned long long hal_clock_ticks(void);

#define SLOTS (1u << 15)
typedef struct {
    void *site;          /* __fentry__'s return address in the function */
    int first, count;    /* its rows in cpu_cost_table, count 0: none */
    int step[2];         /* the scene ticks[k] was looked up for, -2: none */
    unsigned ticks[2];   /* [1]: entered from overlay code */
    int has_ovl;         /* the table has rows for calls from overlay code */
    int is_wrap;         /* a link-time __wrap_ shim */
    const char *last_caller;  /* ticks[1] was looked up for this caller */
    unsigned calls;      /* CPU_COST_PROFILE */
    int traced;          /* CPU_COST_TRACE */
    int irq_row;         /* its cpu_cost_irq_table row, -1 none, -2 unknown */
    int arg_row;         /* its ARG_ROWS entry, -1 none, -2 unknown */
    int boot_first, boot_count;  /* its cpu_cost_boot_table rows (-2: not looked up) */
    int boot_step;       /* the scene boot_ticks was looked up for, -2: none */
    unsigned boot_ticks;
} Slot;
static Slot g_slots[SLOTS];
static int g_armed;
static int g_charging;
static unsigned long long g_charged;
static pthread_t g_main_thread;   /* audio callbacks run elsewhere */
static const char *g_profile;     /* CPU_COST_PROFILE, see hal_cpu_cost_flush */
static const char *g_trace_names; /* CPU_COST_TRACE, see traced() */
static int g_seq_armed;           /* SEQ_IRQ_COST, see hal_cpu_cost_seq_begin */
static int g_ovl_ctx;             /* CPU_COST_OVL, see ticks_for */
extern u8 now_step;

/* The table rows of `name`: rows are sorted by name, then scene (-1, the
 * all-scene mean, first).  -> the first row, *count rows; -1 if none. */
static int rows_in(const CpuCost *table, unsigned n, const char *name, int *count)
{
    int lo = 0, hi = (int)n - 1, mid, cmp, first;
    *count = 0;
    while (lo <= hi) {
        mid = (lo + hi) / 2;
        cmp = strcmp(name, table[mid].name);
        if (cmp == 0) {
            first = mid;
            while (first > 0 && !strcmp(name, table[first - 1].name)) first--;
            while (mid < (int)n && !strcmp(name, table[mid].name)) mid++;
            *count = mid - first;
            return first;
        }
        if (cmp < 0) hi = mid - 1; else lo = mid + 1;
    }
    return -1;
}
static int rows_of(const char *name, int *count)
{
    return rows_in(cpu_cost_table, cpu_cost_count, name, count);
}

/* The gap to the next entry is the rest of the callee plus the caller's own
 * code up to its next call, so one function costs very different times
 * from different callers (SetSemiTrans: 31 ticks in the name entry's
 * sprite sort, ~325 from its panel's _sprite_set_box_shade; "@ovl" is
 * overlay code).  Most specific first: the scene's row for that caller,
 * the scene's mean, the all-scene row for that caller, the all-scene
 * mean (the scene decides how much data each loop walks). */
static unsigned ticks_in(const CpuCost *table, int first, int count, int step,
                         const char *caller)
{
    int i, best = 0;
    unsigned ticks = 0;
    for (i = first; i < first + count; i++) {
        const CpuCost *c = &table[i];
        int rank;
        if (c->caller && (!caller || strcmp(c->caller, caller))) continue;
        if (c->step == step) rank = c->caller ? 4 : 3;
        else if (c->step < 0) rank = c->caller ? 2 : 1;
        else continue;
        if (rank > best) { best = rank; ticks = c->ticks; }
    }
    return ticks;
}
static unsigned ticks_for(int first, int count, int step, const char *caller)
{
    return ticks_in(cpu_cost_table, first, count, step, caller);
}

static int has_ovl_rows(int first, int count)
{
    int i;
    for (i = first; i < first + count; i++)
        if (cpu_cost_table[i].caller) return 1;
    return 0;
}

/* Whether a return address is in overlay code: the port's overlay
 * functions carry their overlay's name as a prefix (pc/src/overlay,
 * generate_overlay_modules.py), retail's live at 0x80132000 and up
 * (pc/tools/measure_cpu_costs.py called_from_overlay).  Cached per site. */
static const char *const g_overlays[] = {
    "BUNKAKEN", "BUNKASAI", "BUNKA_SD", "DATE", "DATE2", "ENDING", "EN_NICHI",
    "ETC", "EVENT", "GEKO", "GYOZI", "KANGEI", "MASTER", "NAME_ENT", "OLH",
    "OMIMAI", "OPTION", "RENSYU", "RPG_BAT", "SHOUGATU", "SHUGAKU", "TACO",
    "TAIIKU", "TEL", "TT", "VALEN"
};
#define CALLERS (1u << 15)
static struct { void *ret; const char *name; } g_callers[CALLERS];
static const char *const WRAP = "__wrap_";
/* -> the calling function's name, "@ovl" for overlay code, WRAP for a
 * link-time shim, NULL if unnamed; cached per return address. */
static const char *caller_name(void *ret)
{
    /* 64-bit arithmetic on every host: `unsigned long` is 32 bits on
     * Windows, where `h >> 49` then hashed every ret to one chain */
    unsigned long long h = ((unsigned long long)(uintptr_t)ret >> 2) * 0x9E3779B97F4A7C15ull;
    unsigned i = (unsigned)(h >> 49) & (CALLERS - 1), k, probes = 0;
    Dl_info info;
    const char *name = NULL;
    while (g_callers[i].ret && probes < CALLERS) {
        if (g_callers[i].ret == ret) return g_callers[i].name;
        i = (i + 1) & (CALLERS - 1);
        probes++;
    }
    if (dladdr(ret, &info) && info.dli_sname) {
        name = info.dli_sname;
        if (!strncmp(name, WRAP, 7)) name = WRAP;
        for (k = 0; name != WRAP && k < sizeof g_overlays / sizeof g_overlays[0]; k++) {
            size_t n = strlen(g_overlays[k]);
            if (!strncmp(name, g_overlays[k], n) && name[n] == '_') {
                name = "@ovl";
                break;
            }
        }
    }
    if (probes < CALLERS) {
        g_callers[i].ret = ret;
        g_callers[i].name = name;
    }
    return name;
}

/* `site` is __fentry__'s return address: 5 bytes (call rel32) past the
 * function start, 9 when the function opens with endbr64. */
static const char *name_of(void *site)
{
    Dl_info info;
    char *s = (char *)site;
    if (!dladdr(s - 5, &info) || !info.dli_sname ||
        (info.dli_saddr != s - 5 && info.dli_saddr != s - 9))
        return NULL;                    /* static or unnamed: no retail probe */
    return info.dli_sname;
}

/* CPU_COST_TRACE=name,name,... (or all): log `[CPU] name tick charge` at
 * each entry of those functions (where the port's clock stands then). */
static int traced(const char *name)
{
    const char *p;
    size_t n;
    if (!g_trace_names || !name) return 0;
    if (!strcmp(g_trace_names, "all")) return 1;
    n = strlen(name);
    for (p = g_trace_names; (p = strstr(p, name)) != NULL; p += n)
        if ((p == g_trace_names || p[-1] == ',') && (p[n] == ',' || !p[n]))
            return 1;
    return 0;
}

static Slot *lookup(void *site)
{
    /* 64-bit arithmetic on every host: `unsigned long` is 32 bits on
     * Windows, where `h >> 49` then hashed every site to one chain */
    unsigned long long h = ((unsigned long long)(uintptr_t)site >> 2) * 0x9E3779B97F4A7C15ull;
    unsigned i = (unsigned)(h >> 49) & (SLOTS - 1);
    const char *name;
    Slot *s;
    while (g_slots[i].site) {
        if (g_slots[i].site == site) return &g_slots[i];
        i = (i + 1) & (SLOTS - 1);
    }
    s = &g_slots[i];
    s->site = site;
    name = name_of(site);
    s->first = name ? rows_of(name, &s->count) : -1;
    s->step[0] = s->step[1] = -2;
    s->has_ovl = s->count ? has_ovl_rows(s->first, s->count) : 0;
    s->is_wrap = name && !strncmp(name, "__wrap_", 7);
    s->traced = traced(name);
    s->irq_row = -2;
    s->arg_row = -2;
    s->boot_first = -2;
    s->boot_step = -2;
    return s;
}

/* The port's link-time shims (__wrap_SetSemiTrans, ...) call the real
 * function, so its caller is the shim: the shim's own caller counts. */
static void *g_wrap_caller;
static const char *caller_of(void *caller)
{
    const char *c = caller_name(caller);
    if (c == WRAP) c = g_wrap_caller ? caller_name(g_wrap_caller) : NULL;
    return c == WRAP ? NULL : c;
}

/* BOOT_ROWS: the boot (game frame 0: _main's init up to the first
 * main-loop frame) is one fixed path over the same data on every power-on,
 * so the port charges each function the mean of the boot's own entries
 * (pc/tools/cpu_costs_boot.tsv, every call counted; per scene where the
 * boot runs it in two), not its mean over the game, where the same
 * function follows other paths: search_tpage 855 ticks at the boot
 * against 1,684 in game, GetVideoMode 132 against 197, _spu_setInTransfer
 * 39 against 5,674.  A function the boot table lacks keeps the main table.
 * BOOT_ROWS=0 is the negative control. */
extern s32 frame_counter;
static int g_boot_rows = -1;
static int boot_rows_on(void)
{
    if (g_boot_rows < 0) {
        const char *e = getenv("BOOT_ROWS");
        g_boot_rows = !(e && e[0] == '0');
    }
    return g_boot_rows;
}
static int boot_ticks(Slot *s, unsigned *ticks)
{
    if (frame_counter != 0 || !cpu_cost_boot_count || !boot_rows_on()) return 0;
    if (s->boot_first == -2) {
        const char *name = name_of(s->site);
        s->boot_first = name ? rows_in(cpu_cost_boot_table, cpu_cost_boot_count,
                                       name, &s->boot_count) : -1;
        if (s->boot_first < 0) s->boot_count = 0;
    }
    if (!s->boot_count) return 0;
    if (s->boot_step != now_step) {
        s->boot_step = now_step;
        s->boot_ticks = ticks_in(cpu_cost_boot_table, s->boot_first, s->boot_count,
                                 now_step, NULL);
    }
    *ticks = s->boot_ticks;
    return 1;
}

static unsigned slot_ticks(Slot *s, void *caller)
{
    const char *c;
    unsigned bt;
    if (boot_ticks(s, &bt)) return bt;
    if (!s->count) return 0;
    if (!(s->has_ovl && g_ovl_ctx && caller)) {
        if (s->step[0] != now_step) {
            s->step[0] = now_step;
            s->ticks[0] = ticks_for(s->first, s->count, now_step, NULL);
        }
        return s->ticks[0];
    }
    c = caller_of(caller);
    if (s->step[1] != now_step || s->last_caller != c) {
        s->step[1] = now_step;
        s->last_caller = c;
        s->ticks[1] = ticks_for(s->first, s->count, now_step, c);
    }
    return s->ticks[1];
}

void hal_cpu_cost_arm(void)
{
    const char *e;
    g_main_thread = pthread_self();
    g_armed = hal_model_on("GAME_CPU_COST");
    g_profile = getenv("CPU_COST_PROFILE");
    g_trace_names = getenv("CPU_COST_TRACE");
    e = getenv("CPU_COST_OVL");
    g_ovl_ctx = !(e && e[0] == '0');
    e = getenv("SEQ_IRQ_COST");
    g_seq_armed = g_armed && !(e && e[0] == '0') && cpu_cost_irq_count > 0;
}

/* CPU_COST_PROFILE=<path>: count the entries per function (outside
 * interrupts) and write `name calls` lines at exit (pc/hal/trace.c flushes
 * it before _exit), to set the port's call counts beside retail's. */
void hal_cpu_cost_flush(void)
{
    FILE *f;
    unsigned i;
    Dl_info info;
    if (!g_profile || !(f = fopen(g_profile, "w"))) return;
    for (i = 0; i < SLOTS; i++) {
        if (!g_slots[i].site || !g_slots[i].calls) continue;
        if (dladdr((char *)g_slots[i].site - 5, &info) && info.dli_sname)
            fprintf(f, "%s\t%u\n", info.dli_sname, g_slots[i].calls);
    }
    fclose(f);
    g_profile = NULL;
}

static int charging_allowed(void)
{
    if (!g_armed || g_charging) return 0;
    if (!pthread_equal(pthread_self(), g_main_thread)) return 0;
    return !hal_irq_in_handler() && !hal_cd_dispatching();
}

static void charge(unsigned ticks)
{
    g_charging = 1;
    g_charged += ticks;
    hal_cpu_spend(ticks);
    g_charging = 0;
}

/* ---- the sequencer tick inside the VBlank exception ----
 * libsnd's sequencer (SsSeqCalledTbyT -> Snd_play -> SeqPlay -> GetSeqData
 * -> NoteOn -> SpuVmKeyOn ...) runs inside every VBlank exception, from
 * libetc's trapIntrVSync.  On retail it is the part of the exception that
 * varies: 2,700 ticks on a VBlank with no sequence event, up to ~18,000
 * with several notes (the reference emulator, the name entry: exceptions of
 * 24,514..39,730 ticks), and that variation sets where each frame's game
 * work starts.  The port used one constant (24,449, measured with the
 * sequencer idle).  Now the VBlank exception costs the measured rest of
 * the exception (cpu_cost_vblank_base, median) plus the functions the
 * sequencer tick actually enters, each charged its retail mean inside that
 * window (pc/tools/measure_cpu_costs.py analyze-irq / emit-irq,
 * pc/hal/cpu_cost_irq_table.c).  Nothing advances the clock here: the sum
 * is the exception's length (pc/hal/system.c deliver_sdk).
 * SEQ_IRQ_COST=0 (or GAME_CPU_COST=0) is the negative control: the old
 * constant. */
static int g_seq_window;
static unsigned long long g_seq_acc;

static int seq_armed(void) { return g_seq_armed > 0; }

static int irq_row(const char *name)
{
    int lo = 0, hi = (int)cpu_cost_irq_count - 1, mid, cmp;
    while (lo <= hi) {
        mid = (lo + hi) / 2;
        cmp = strcmp(name, cpu_cost_irq_table[mid].name);
        if (!cmp) return mid;
        if (cmp < 0) hi = mid - 1; else lo = mid + 1;
    }
    return -1;
}

void hal_cpu_cost_seq_begin(void)
{
    g_seq_acc = 0;
    g_seq_window = seq_armed() && pthread_equal(pthread_self(), g_main_thread);
}

void hal_cpu_cost_seq_end(void) { g_seq_window = 0; }

void hal_cpu_cost_seq_clear(void) { g_seq_acc = 0; }

/* The VBlank exception's CPU ticks with the sequencer work just run;
 * 0 when the model is off (the caller keeps its constant). */
int hal_cpu_cost_vblank_ticks(unsigned *ticks)
{
    if (!seq_armed()) return 0;
    *ticks = cpu_cost_vblank_base + (unsigned)g_seq_acc;
    g_seq_acc = 0;
    return 1;
}

static void seq_enter(void *site)
{
    Slot *s = lookup(site);
    if (s->irq_row == -2) {
        const char *name = name_of(site);
        s->irq_row = name ? irq_row(name) : -1;
    }
    if (s->irq_row < 0) return;
    g_seq_acc += cpu_cost_irq_table[s->irq_row].ticks;
    if (s->traced)
        fprintf(stderr, "[CPU-IRQ] %s %llu %u\n", name_of(site), hal_clock_ticks(),
                cpu_cost_irq_table[s->irq_row].ticks);
}

/* ---- functions whose time is a size argument's ----
 * A function the playthroughs call rarely, on data of different sizes,
 * costs its all-scene mean (the scene rows need 16 calls), which is the
 * wrong cost everywhere.  block_checksum_check(blocks, buf) sums
 * blocks * 2048 - 4 bytes one pair per iteration and returns: measured on
 * the reference emulator (Interpreter, date_save phase 1, entry probe plus
 * return-address probes, exceptions excluded; 2026-10-31 timing round 6),
 * entry to the next main-binary entry:
 *   41 blocks (week_day_load, lesson, x10) 969,118..969,173, body 969,150
 *   45 blocks (bustup_check_init, date)    1,062,250,      body 1,062,227
 *   96 blocks (scene 2, x3)  2,261,113..2,268,106 (+0.5% on the fit: those
 *              calls span four VBlanks, with DMA stalls the fit leaves out)
 * -> 14,921 + 23,273.75 per block (the 41/45 pair; the per-block step of
 * the bodies alone is 23,269.25).  The all-scene mean the port charged
 * for every call was 1,399,801: +430,646 ticks on each 41-block lesson
 * check, a whole VBlank every 80 frames in scene 64.
 * CPU_COST_ARGS=0 is the negative control (the table's mean again). */
typedef struct {
    const char *name;
    unsigned base;           /* ticks */
    unsigned per_unit_x4;    /* ticks per unit, times 4 */
    unsigned long mask;      /* the argument's width (a u8 parameter) */
} ArgCost;
static const ArgCost ARG_ROWS[] = {
    {"block_checksum_check", 14921u, 93095u, 0xFFul},
};
static int g_arg_rows = -1;

static int arg_row_of(Slot *s, void *site)
{
    unsigned k;
    const char *name;
    if (s->arg_row != -2) return s->arg_row;
    s->arg_row = -1;
    if (g_arg_rows < 0) {
        const char *e = getenv("CPU_COST_ARGS");
        g_arg_rows = !(e && e[0] == '0');
    }
    if (!g_arg_rows) return -1;
    name = name_of(site);
    for (k = 0; name && k < sizeof ARG_ROWS / sizeof ARG_ROWS[0]; k++)
        if (!strcmp(name, ARG_ROWS[k].name)) s->arg_row = (int)k;
    return s->arg_row;
}

void hal_cpu_cost_enter(void *site, void *caller, unsigned long arg0)
{
    Slot *s;
    unsigned ticks;
    int a;
    if (g_seq_window && !g_charging && pthread_equal(pthread_self(), g_main_thread)) {
        seq_enter(site);
        return;
    }
    if (!charging_allowed()) return;
    s = lookup(site);
    s->calls++;
    if (s->is_wrap) g_wrap_caller = caller;
    ticks = slot_ticks(s, caller);
    a = frame_counter != 0 ? arg_row_of(s, site) : -1;
    if (a >= 0)
        ticks = ARG_ROWS[a].base +
                (unsigned)(((unsigned long long)(arg0 & ARG_ROWS[a].mask) *
                            ARG_ROWS[a].per_unit_x4) / 4u);
    if (s->traced)
        fprintf(stderr, "[CPU] %s %llu %u\n", name_of(site), hal_clock_ticks(), ticks);
    if (ticks) charge(ticks);
}

/* A HAL replacement of a pure-CPU SDK function (GetTPage, GetClut,
 * SetDrawMode, the BIOS TestEvent) charges retail's time for it by name,
 * including the SDK helpers retail calls from it that the replacement
 * does not (measure_cpu_costs.py ABSORB).  `cache` is the caller's
 * static: 0 on first use, then 1 + its first table row (or -1: none). */
/* The boot's own row of a HAL replacement charged by name (BOOT_ROWS; looked
 * up at each call: the boot is short).  -> 1 if the boot table has one. */
static int boot_named(const char *name)
{
    int count, first;
    unsigned t;
    if (frame_counter != 0 || !cpu_cost_boot_count || !boot_rows_on())
        return 0;
    first = rows_in(cpu_cost_boot_table, cpu_cost_boot_count, name, &count);
    if (first < 0) return 0;
    t = ticks_in(cpu_cost_boot_table, first, count, now_step, NULL);
    if (t && charging_allowed()) charge(t);
    return 1;
}

/* HAL_BOOT_COST: a HAL replacement whose time is measured at the boot only
 * (DecDCToutCallback with the DMACallback / setIntrDMA it calls on retail):
 * the boot's row at game frame 0, nothing elsewhere, as before. */
void hal_cpu_cost_boot_named(const char *name)
{
    (void)boot_named(name);
}

void hal_cpu_cost_named(const char *name, unsigned *cache)
{
    int count, first;
    if (boot_named(name)) return;
    if (!*cache) {
        first = rows_of(name, &count);
        *cache = first < 0 ? (unsigned)-1 : (unsigned)first + 1u;
    }
    if (*cache == (unsigned)-1 || !charging_allowed()) return;
    first = (int)*cache - 1;
    for (count = 0; first + count < (int)cpu_cost_count &&
         !strcmp(cpu_cost_table[first + count].name, name); count++) {}
    count = (int)ticks_for(first, count, now_step, NULL);
    if (count) charge((unsigned)count);
}

/* CALL_CONTINUATION: a BIOS call the port times with its own model of the
 * call alone (bzero 317 + 132 a byte, memcpy; src/main/pc-port/
 * bios_trampolines.c) returns into its caller's code, which runs on to the
 * caller's next call: on retail that time is in the gap after the call,
 * and no port charge covered it (girl_rec_init's code after its bzero,
 * 1,191 ticks; work_clear's after its three, 1,392).  Measured per calling
 * function on the boot (measure_cpu_costs.py run-boot --ret-from,
 * analyze-ret: the call's return address probed), boot table rows
 * "NAME@ret" with that caller; charged at the call's return, game frame 0.
 * CALL_CONTINUATION=0 (or BOOT_ROWS=0) is the negative control. */
static int g_continuation = -1;
static int continuation_on(void)
{
    if (g_continuation < 0) {
        const char *e = getenv("CALL_CONTINUATION");
        g_continuation = !(e && e[0] == '0');
    }
    return g_continuation && frame_counter == 0 && cpu_cost_boot_count && boot_rows_on();
}
/* -> the boot row `row` for the function `ret` returns into; 0 if none */
static int caller_row(const char *row, void *ret, unsigned *ticks)
{
    int first, count, i, best = 0;
    const char *c;
    first = rows_in(cpu_cost_boot_table, cpu_cost_boot_count, row, &count);
    if (first < 0 || !(c = caller_of(ret))) return 0;
    for (i = first; i < first + count; i++) {
        const CpuCost *r = &cpu_cost_boot_table[i];
        int rank;
        if (!r->caller || strcmp(r->caller, c)) continue;
        rank = r->step == now_step ? 2 : r->step < 0 ? 1 : 0;
        if (rank > best) { best = rank; *ticks = r->ticks; }
    }
    return best > 0;
}
void hal_cpu_cost_return(const char *row, void *ret)
{
    unsigned t = 0;
    if (!continuation_on() || !charging_allowed()) return;
    if (caller_row(row, ret, &t) && t) charge(t);
}

/* The same for a HAL model whose last stage is a tail to its caller's next
 * call, measured in vblank_function (PutDrawEnv's 285, PutDispEnv's 224):
 * at the boot, the tail measured for the actual caller
 * (measure_cpu_costs.py analyze-tail; draw_and_disp_env_init's PutDrawEnv
 * 552, PutDispEnv 422).  Not a charge: the HAL spends what this returns. */
unsigned hal_cpu_cost_tail(const char *row, void *ret, unsigned dflt)
{
    unsigned t = dflt;
    if (!continuation_on() || !g_armed) return dflt;
    return caller_row(row, ret, &t) ? t : dflt;
}

/* HAL_SDK_COST: hal_cpu_cost_named for the HAL's pure-CPU SDK
 * replacements that had charged nothing (libgte's matrix functions: on
 * retail ~38,000 ticks of every name-entry frame went to MulMatrix2,
 * MulMatrix, ApplyMatrixLV, RotMatrix, ScaleMatrix, Push/PopMatrix and
 * the Set*Matrix loads, entry to the caller's next call).  Each charges
 * its row like any game function; a HAL function's calls to another
 * charged HAL function are not charged (pc/hal/gte.c GTE_INNER).
 * HAL_SDK_CPU=0 is the negative control. */
static int g_sdk_armed = -1;
void hal_cpu_cost_sdk(const char *name, unsigned *cache)
{
    if (g_sdk_armed < 0) {
        g_sdk_armed = hal_model_on("HAL_SDK_CPU");
    }
    if (g_sdk_armed) hal_cpu_cost_named(name, cache);
}

/* The PS1 C library's abs (game objects: -fno-builtin-abs, linked with
 * --wrap=abs; pc/Makefile).  INT_MIN stays INT_MIN, as on the R3000. */
int __wrap_abs(int i)
{
    static unsigned cache;
    hal_cpu_cost_sdk("abs", &cache);
    return i < 0 ? (int)(0u - (unsigned)i) : i;
}

/* CPU time a HAL model of an SDK function charges itself (gpu.c LoadImage):
 * spent on the system clock, with the game-code CPU model's own switch. */
void hal_cpu_charge(unsigned ticks)
{
    if (charging_allowed() && ticks) charge(ticks);
}

/* Host recovery (pc/src/main.c longjmp) can abandon a charge mid-way. */
void hal_cpu_cost_abort(void) { g_charging = 0; }

unsigned long long hal_cpu_cost_charged(void) { return g_charged; }

/* A HAL model measured as retail time between two points (PutDrawEnv's
 * entry to set_alarm, PutDispEnv's gaps between display writes) that calls
 * instrumented game/SDK code in between (the matched SetDrawEnv and its
 * get_cs .. get_tw, GetVideoMode): those functions already charged their
 * rows, which are part of the measured span.  -> `ticks` less what was
 * charged since `mark` (hal_cpu_cost_charged() taken at the span's start).
 * HAL_NET_SPANS=0 is the negative control (both charged, as before). */
static int g_net_spans = -1;
unsigned hal_cpu_cost_net(unsigned ticks, unsigned long long mark)
{
    unsigned long long done;
    if (g_net_spans < 0) {
        const char *e = getenv("HAL_NET_SPANS");
        g_net_spans = !(e && e[0] == '0');
    }
    if (!g_net_spans) return ticks;
    done = g_charged - mark;
    return done >= ticks ? 0u : ticks - (unsigned)done;
}

/* `call __fentry__` comes before the instrumented function's prologue, so
 * every register may hold one of its arguments (or a value its body reads
 * without declaring it): keep all of them.  On entry rsp is 16-aligned
 * (the function's own entry is 8 mod 16, the call pushes 8).
 * Windows (Microsoft x64 convention, docs/pc-port/WINDOWS-BUILD.md): the
 * volatile set is rax rcx rdx r8-r11 and xmm0-5, the arguments are rcx rdx
 * r8 r9 (the instrumented function's first one is the saved rcx), and
 * hal_cpu_cost_enter is owed 32 bytes of shadow space.  Frame: shadow
 * 0..31, xmm0-5 32..127, pad 128..135, r11..rax 136..184, the site (this
 * call's return address) 192, the instrumented function's return 200. */
#ifdef _WIN32
__asm__(
    ".text\n"
    ".globl __fentry__\n"
    ".def __fentry__; .scl 2; .type 32; .endef\n"
    "__fentry__:\n"
    "    pushq %rax\n    pushq %rcx\n    pushq %rdx\n    pushq %r8\n"
    "    pushq %r9\n    pushq %r10\n    pushq %r11\n"
    "    subq $136, %rsp\n"
    "    movdqu %xmm0, 32(%rsp)\n    movdqu %xmm1, 48(%rsp)\n"
    "    movdqu %xmm2, 64(%rsp)\n    movdqu %xmm3, 80(%rsp)\n"
    "    movdqu %xmm4, 96(%rsp)\n    movdqu %xmm5, 112(%rsp)\n"
    "    movq 192(%rsp), %rcx\n"
    "    movq 200(%rsp), %rdx\n"
    "    movq 176(%rsp), %r8\n"
    "    call hal_cpu_cost_enter\n"
    "    movdqu 32(%rsp), %xmm0\n    movdqu 48(%rsp), %xmm1\n"
    "    movdqu 64(%rsp), %xmm2\n    movdqu 80(%rsp), %xmm3\n"
    "    movdqu 96(%rsp), %xmm4\n    movdqu 112(%rsp), %xmm5\n"
    "    addq $136, %rsp\n"
    "    popq %r11\n    popq %r10\n    popq %r9\n    popq %r8\n"
    "    popq %rdx\n    popq %rcx\n    popq %rax\n"
    "    ret\n");
#else
__asm__(
    ".text\n"
    ".globl __fentry__\n"
    ".type __fentry__, @function\n"
    "__fentry__:\n"
    "    pushq %rax\n    pushq %rdi\n    pushq %rsi\n    pushq %rdx\n"
    "    pushq %rcx\n    pushq %r8\n    pushq %r9\n    pushq %r10\n    pushq %r11\n"
    "    subq $136, %rsp\n"
    "    movdqu %xmm0, 0(%rsp)\n    movdqu %xmm1, 16(%rsp)\n"
    "    movdqu %xmm2, 32(%rsp)\n    movdqu %xmm3, 48(%rsp)\n"
    "    movdqu %xmm4, 64(%rsp)\n    movdqu %xmm5, 80(%rsp)\n"
    "    movdqu %xmm6, 96(%rsp)\n    movdqu %xmm7, 112(%rsp)\n"
    "    movq 208(%rsp), %rdi\n"
    "    movq 216(%rsp), %rsi\n"
    "    movq 192(%rsp), %rdx\n"
    "    call hal_cpu_cost_enter\n"
    "    movdqu 0(%rsp), %xmm0\n    movdqu 16(%rsp), %xmm1\n"
    "    movdqu 32(%rsp), %xmm2\n    movdqu 48(%rsp), %xmm3\n"
    "    movdqu 64(%rsp), %xmm4\n    movdqu 80(%rsp), %xmm5\n"
    "    movdqu 96(%rsp), %xmm6\n    movdqu 112(%rsp), %xmm7\n"
    "    addq $136, %rsp\n"
    "    popq %r11\n    popq %r10\n    popq %r9\n    popq %r8\n"
    "    popq %rcx\n    popq %rdx\n    popq %rsi\n    popq %rdi\n    popq %rax\n"
    "    ret\n"
    ".size __fentry__, .-__fentry__\n");
#endif
