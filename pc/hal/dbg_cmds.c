/* SPDX-License-Identifier: GPL-3.0-or-later */
/* dbg_cmds.c -- the debug toolkit's frame-boundary dispatcher, commands and
 * command scripts (DEBUG-ONLY, off by default).  The live console that
 * feeds the same commands is pc/hal/dbg_console.c.
 *
 * pc/src/main.c calls dbg_frame_boundary() at the top of every main-loop
 * iteration: no game code is on the stack there, the previous frame's
 * VBlank, CD worker and sound command processor have run.  Every action of
 * this toolkit happens at such a boundary -- requests made elsewhere (a
 * hotkey inside VSync's event poll, a console line) wait for the next one.
 *
 * Environment (all optional; with none set the boundary returns at once):
 *   PC_SAVESTATE=N:PATH[,N:PATH...]  save a state at the first boundary
 *                          with frame_counter >= N (game frames)
 *   PC_LOADSTATE=PATH      load a state at the first boundary
 *   PC_DBG_CMDS=FILE       a command script (below), run in order
 *   PC_DBG_CONSOLE=stdin | PORT | unix:PATH
 *                          live console on stdin, on TCP 127.0.0.1:PORT, or
 *                          on a unix socket; one command per line, one reply
 *                          line per command (`ok ...` / `err ...`)
 *   PC_DBG_SLOT=PATH       file of the F5 (save) / F9 (load) hotkeys
 *                          (default state_snapshot.tks)
 *
 * Command script lines (`#` comments):
 *   CMD ARGS               run at the next boundary
 *   at N CMD ARGS          run at the first boundary with frame_counter >= N
 *   after N CMD ARGS       run N game frames after the previous line ran
 *   when COND... do CMD ARGS
 *                          run at the first boundary where every COND holds;
 *                          COND is NAME[:SIZE]=VALUE or NAME[:SIZE]!=VALUE
 *                          (NAME a symbol, `file.c:static`, or 0xADDR)
 * Lines run strictly in order; a waiting line holds back the ones after it.
 *
 * Commands (console and scripts):
 *   help | info | sym NAME
 *   peek TARGET [SIZE]          TARGET: symbol, file.c:static, 0xADDR, +OFF
 *   poke TARGET[:SIZE] VALUE    VALUE: integer, or hex:AABB.. (raw bytes)
 *   goto N                      goto_step(N), the game's scene change
 *   pause | resume | step [N]   hold the game at the boundary / run N frames
 *   save PATH | load PATH       save states (pc/hal/dbg_state.c)
 *   shot PATH                   the displayed 320x240 image as PPM
 *   vram PATH                   the 1024x512 VRAM, u16le (the -dumpvram layout)
 *   ram PATH                    the 2 MB PS1 RAM
 *   qsave [N] | qload [N]       the player's quicksave slot N (pc/hal/quicksave.c;
 *                               default: the selected slot), as F5 / F9 do it
 *   qslot N | qlist             select a slot / print the slot list
 *   qsave ID | qload ID         any save entry by id (save0003, auto1, resume, ending)
 *   qdel ID                     delete a save entry's files
 *   rewind [N] | rewind_to F    the rewind buffer (pc/hal/rewind.c): back N
 *                               snapshots (0: the newest), or to the newest
 *                               snapshot at or before game frame F
 *   winquit                     quit as the window's close button does (the
 *                               resume state, pc/hal/save_auto.c)
 *   winclose                    the window's close button as SDL delivers it:
 *                               an SDL_QUIT event, taken by whoever drains
 *                               the queue next (winquit calls the request)
 *   keys KEY [KEY..]            queue a press and a release of each key
 *                               (SDL names, Ctrl+/Alt+/Shift+/GUI+), as
 *                               TOKI_INPUT_SCRIPT `key` does (pc/hal/pad.c)
 *   anchor                      trace_anchor(): PC_PARITY_* / PC_TRACE_FRAMES
 *                               offsets count from here
 *   debug CATS                  PC_DEBUG categories (fmv,gpu,scene,...; 0 = off)
 *   gp0 on|off                  GP0_TRACE histogram collection
 *   echo TEXT | quit [CODE]
 *
 * Nothing here changes game logic, timing or rendering: a save copies
 * bytes out, and only the commands that say so (poke, goto, load) write
 * game state -- explicitly, as debugging fixtures.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <dlfcn.h>
#include "types.h"
#include "dbg_toolkit.h"
#include "dbg_platform.h"
#include "gpu_display.h"
#include <SDL2/SDL.h>

extern s32 frame_counter;
extern u8 now_step, now_sub_step, now_sub_sub_step, now_girl;
extern volatile int g_crash_count;
extern u16 g_vram[512][1024];
extern u32 hal_clock_vblanks(void);
extern const char *hal_overlay_loaded(void);
extern void goto_step(s32);
extern void trace_anchor(void);
extern void hal_poll_quit(void);
extern int hal_is_running(void);
extern unsigned int g_pc_debug_flags;
extern int g_pc_debug_initialized;

static int g_active;            /* anything requested at all */
static int g_paused;
static long g_step_left = -1;   /* frames to run before pausing again */
static int g_save_tried_paused;

/* pending requests from hotkeys / the console (acted on at a boundary) */
static char g_req_save[1024], g_req_load[1024];
static char g_slot[1024] = "state_snapshot.tks";

/* PC_SAVESTATE schedule */
typedef struct { long frame; char path[1024]; int done; } SaveAt;
static SaveAt g_saves[16];
static int g_nsaves;
static char g_load_path[1024];

/* ------------------------------------------------------------------ */
/* output                                                              */
/* ------------------------------------------------------------------ */

static int g_out_fd = -1;       /* the console client of the current line */

static void reply(const char *fmt, ...) {
    char buf[8192];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 2) n = (int)sizeof buf - 2;
    buf[n++] = '\n';
    if (g_out_fd >= 0) {
        /* a console client (pc/hal/dbg_console.c) */
        extern void dbg_console_send(int h, const char *buf, unsigned long n);
        dbg_console_send(g_out_fd, buf, (unsigned long)n);
    } else {
        printf("[DBG] %.*s", n, buf);
        fflush(stdout);
    }
}

/* ------------------------------------------------------------------ */
/* targets and values                                                  */
/* ------------------------------------------------------------------ */

/* TARGET -> address; *size gets the symbol size (0 for a raw address) */
static unsigned char *resolve_target(const char *spec, unsigned long *size) {
    char name[256];
    const char *plus = strchr(spec, '+');
    unsigned long off = 0, sz = 0;
    void *base = NULL;
    size_t n = plus ? (size_t)(plus - spec) : strlen(spec);
    if (n >= sizeof name) return NULL;
    memcpy(name, spec, n);
    name[n] = 0;
    if (plus) off = strtoul(plus + 1, NULL, 0);
    if (name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
        base = (void *)strtoul(name, NULL, 16);
    } else {
        base = dbg_sym_lookup(name, &sz);
        if (!base && !strchr(name, ':')) base = dlsym(RTLD_DEFAULT, name);
    }
    if (!base) return NULL;
    if (size) *size = off ? 0 : sz;
    return (unsigned char *)base + off;
}

static int parse_hex_bytes(const char *s, unsigned char *out, unsigned long cap,
                           unsigned long *n) {
    *n = 0;
    while (isxdigit((unsigned char)s[0]) && isxdigit((unsigned char)s[1])) {
        char hx[3];
        if (*n >= cap) return 0;
        hx[0] = s[0]; hx[1] = s[1]; hx[2] = 0;
        out[(*n)++] = (unsigned char)strtoul(hx, NULL, 16);
        s += 2;
    }
    return *s == 0 && *n > 0;
}

/* NAME[:SIZE]=VALUE / != : the condition syntax of PC_INPUT `wait` */
typedef struct { unsigned char *ptr; unsigned long size; unsigned char value[64]; int neg; } Cond;

static int parse_cond(const char *tok, Cond *c) {
    char buf[256], *eq, *colon;
    unsigned long sz = 0, i;
    if (strlen(tok) >= sizeof buf) return 0;
    strcpy(buf, tok);
    eq = strchr(buf, '=');
    if (!eq || eq == buf) return 0;
    c->neg = eq[-1] == '!';
    if (c->neg) eq[-1] = 0;
    *eq = 0;
    colon = strrchr(buf, ':');
    /* `file.c:name` keeps its colon; a trailing `:N` (digits) is a size */
    if (colon && isdigit((unsigned char)colon[1])) {
        *colon = 0;
        c->size = strtoul(colon + 1, NULL, 0);
    } else {
        c->size = 0;
    }
    c->ptr = resolve_target(buf, &sz);
    if (!c->ptr) return 0;
    if (!c->size) c->size = (sz == 1 || sz == 2 || sz == 4) ? sz : 4;
    if (c->size > sizeof c->value || !dbg_addr_ok(c->ptr, c->size)) return 0;
    memset(c->value, 0, sizeof c->value);
    if (c->size <= 8) {
        unsigned long long v = strtoull(eq + 1, NULL, 0);
        if (eq[1] == '-') v = (unsigned long long)strtoll(eq + 1, NULL, 0);
        for (i = 0; i < c->size; i++) c->value[i] = (unsigned char)(v >> (8 * i));
    } else {
        unsigned long n;
        if (!parse_hex_bytes(eq + 1, c->value, sizeof c->value, &n) || n != c->size) return 0;
    }
    return 1;
}

static int cond_holds(const Cond *c) {
    int eq = memcmp(c->ptr, c->value, c->size) == 0;
    return c->neg ? !eq : eq;
}

/* ------------------------------------------------------------------ */
/* commands                                                            */
/* ------------------------------------------------------------------ */

/* files and buffers go around the brk heap (see dbg_state.c) */
extern int dbg_write_file(const char *path, const void *data, unsigned long n);
extern void *dbg_mem_alloc(unsigned long n);
extern void *dbg_mem_realloc(void *p, unsigned long n);

static int write_file(const char *path, const void *data, unsigned long n) {
    return dbg_write_file(path, data, n);
}

static int write_shot(const char *path) {
    static unsigned int pixels[GPU_SCANOUT_PIXELS];
    static unsigned char ppm[32 + GPU_SCANOUT_PIXELS * 3];
    GpuScanoutState d;
    int w, h, x, y, n;
    gpu_get_scanout_state(&d);
    if (!gpu_scanout_pack(&d, &g_vram[0][0], pixels, GPU_SCANOUT_PIXELS, &w, &h)) return 0;
    if (d.width > 0 && d.width < w) w = d.width;
    if (d.height > 0 && d.height < h) h = d.height;
    n = snprintf((char *)ppm, 32, "P6\n%d %d\n255\n", w, h);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            unsigned int p = pixels[y * GPU_SCANOUT_WIDTH + x];
            ppm[n++] = (unsigned char)(p >> 24);
            ppm[n++] = (unsigned char)(p >> 16);
            ppm[n++] = (unsigned char)(p >> 8);
        }
    }
    return write_file(path, ppm, (unsigned long)n);
}

extern void qs_writer_wait(void) __attribute__((weak));
static void quit_now(int code) {
    if (qs_writer_wait) qs_writer_wait();     /* a quicksave still being written */
    {
        extern void card_writer_wait(void) __attribute__((weak));   /* card_writer.c */
        if (card_writer_wait) card_writer_wait();   /* the memory card file */
    }
    {
        /* spu.c: the audio dumps' last bytes and the run's audio-health totals */
        extern void spu_audio_health_final(void) __attribute__((weak));
        if (spu_audio_health_final) spu_audio_health_final();
    }
    printf("[DBG] quit (code %d) at frame %ld\n", code, (long)frame_counter);
    fflush(NULL);
    _exit(code);   /* as PC_TRACE_FRAMES: no static destructors under the audio thread */
}

static void cmd_info(void) {
    const char *ovl = hal_overlay_loaded();
    reply("ok frame=%ld vblanks=%lu step=%u sub=%u subsub=%u girl=%u overlay=%s paused=%d crashes=%d",
          (long)frame_counter, (unsigned long)hal_clock_vblanks(), now_step, now_sub_step,
          now_sub_sub_step, now_girl, ovl ? ovl : "-", g_paused, g_crash_count);
}

static void do_save(const char *path) {
    const char *why = "";
    int r = dbg_state_save(path, &why);
    if (r == 0) reply("ok saved %s: %s", path, why);
    else if (r > 0) { reply("ok deferred %s: %s", path, why); snprintf(g_req_save, sizeof g_req_save, "%s", path); }
    else reply("err save %s: %s", path, why);
}

static void do_load(const char *path) {
    const char *why = "";
    if (dbg_state_load(path, &why) == 0) {
        reply("ok loaded %s: %s", path, why);
    } else {
        reply("err load %s: %s", path, why);
    }
}

/* pc/hal/quicksave.c (weak: harnesses that link this file alone) */
extern void qs_boundary(void) __attribute__((weak));
extern void qs_request_save(int slot) __attribute__((weak));
extern void qs_request_load(int slot) __attribute__((weak));
extern void qs_select_slot(int slot) __attribute__((weak));
extern int qs_slot_list(char *out, size_t cap) __attribute__((weak));
extern void qs_request_save_id(const char *id) __attribute__((weak));
extern void qs_request_load_id(const char *id) __attribute__((weak));
extern int qs_delete_id(const char *id) __attribute__((weak));
extern void qs_writer_wait(void) __attribute__((weak));
extern void hal_request_quit(void);
/* pc/hal/rewind.c (weak: harnesses that link this file alone) */
extern void rw_boundary(void) __attribute__((weak));
extern int rw_wanted(void) __attribute__((weak));
extern int rw_command(int steps, long frame) __attribute__((weak));
/* pc/hal/pad.c (weak: harnesses that link this file alone) */
extern void pad_queue_keys(const char *list) __attribute__((weak));

/* Run one command line.  Returns 1 if the line was understood. */
static int run_command(char *line) {
    char *argv[8];
    int argc = 0;
    char *p = line;
    while (argc < 8) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n' || *p == '\r') break;
        argv[argc++] = p;
        if (argc == 2 && !strcmp(argv[0], "echo")) break;   /* rest of line */
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        if (*p) *p++ = 0;
    }
    if (argc && !strcmp(argv[0], "echo")) {
        char *e = argv[1] ? argv[1] + strlen(argv[1]) : NULL;
        while (e && e > argv[1] && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
    }
    if (!argc) return 1;
    if (!strcmp(argv[0], "help")) {
        reply("ok commands: info sym peek poke goto pause resume step save load qsave qload "
              "qslot qlist qdel rewind rewind_to shot vram ram anchor debug gp0 echo winquit "
              "winclose keys quit");
    } else if (!strcmp(argv[0], "info")) {
        cmd_info();
    } else if (!strcmp(argv[0], "sym") && argc == 2) {
        unsigned long sz = 0;
        unsigned char *a = resolve_target(argv[1], &sz);
        if (a) reply("ok %s 0x%lx size %lu", argv[1], (unsigned long)a, sz);
        else reply("err no symbol %s", argv[1]);
    } else if (!strcmp(argv[0], "peek") && argc >= 2) {
        unsigned long sz = 0, n, i;
        unsigned char *a = resolve_target(argv[1], &sz);
        char hex[2 * 256 + 1];
        n = argc > 2 ? strtoul(argv[2], NULL, 0) : (sz ? sz : 4);
        if (n > 256) n = 256;
        if (!a || !n || !dbg_addr_ok(a, n)) { reply("err cannot read %s", argv[1]); return 1; }
        for (i = 0; i < n; i++) sprintf(hex + 2 * i, "%02x", a[i]);
        if (n <= 8) {
            unsigned long long v = 0;
            long long sv;
            for (i = 0; i < n; i++) v |= (unsigned long long)a[i] << (8 * i);
            sv = (n < 8 && (v >> (8 * n - 1)) & 1) ? (long long)(v - (1ull << (8 * n))) : (long long)v;
            reply("ok 0x%lx %lu %s u=%llu s=%lld", (unsigned long)a, n, hex, v, sv);
        } else {
            reply("ok 0x%lx %lu %s", (unsigned long)a, n, hex);
        }
    } else if (!strcmp(argv[0], "poke") && argc == 3) {
        unsigned long sz = 0, n, i;
        unsigned char bytes[256];
        char tgt[256], *colon;
        unsigned char *a;
        snprintf(tgt, sizeof tgt, "%s", argv[1]);
        colon = strrchr(tgt, ':');
        n = 0;
        if (colon && isdigit((unsigned char)colon[1])) { *colon = 0; n = strtoul(colon + 1, NULL, 0); }
        a = resolve_target(tgt, &sz);
        if (!a) { reply("err no symbol %s", tgt); return 1; }
        if (!strncmp(argv[2], "hex:", 4)) {
            unsigned long got;
            if (!parse_hex_bytes(argv[2] + 4, bytes, sizeof bytes, &got)) { reply("err bad hex"); return 1; }
            n = got;
        } else {
            unsigned long long v = argv[2][0] == '-' ? (unsigned long long)strtoll(argv[2], NULL, 0)
                                                    : strtoull(argv[2], NULL, 0);
            if (!n) n = (sz == 1 || sz == 2 || sz == 4 || sz == 8) ? sz : 4;
            if (n > 8) { reply("err integer poke wider than 8 bytes; use hex:"); return 1; }
            for (i = 0; i < n; i++) bytes[i] = (unsigned char)(v >> (8 * i));
        }
        if (!dbg_addr_ok(a, n)) { reply("err %s is outside PS1 RAM / MMIO / the executable's data", tgt); return 1; }
        memcpy(a, bytes, n);
        reply("ok poked %lu bytes at 0x%lx", n, (unsigned long)a);
    } else if (!strcmp(argv[0], "goto") && argc == 2) {
        int s = (int)strtol(argv[1], NULL, 0);
        goto_step(s);
        reply("ok goto_step(%d) at frame %ld", s, (long)frame_counter);
    } else if (!strcmp(argv[0], "pause")) {
        g_paused = 1;
        g_step_left = -1;
        reply("ok paused at frame %ld", (long)frame_counter);
    } else if (!strcmp(argv[0], "resume") || !strcmp(argv[0], "cont")) {
        g_paused = 0;
        g_step_left = -1;
        reply("ok running");
    } else if (!strcmp(argv[0], "step")) {
        long n = argc > 1 ? strtol(argv[1], NULL, 0) : 1;
        if (n < 1) n = 1;
        g_paused = 0;
        g_step_left = n - 1;   /* counted down at each boundary after this one */
        reply("ok stepping %ld frame(s) from %ld", n, (long)frame_counter);
    } else if (!strcmp(argv[0], "save") && argc == 2) {
        do_save(argv[1]);
    } else if (!strcmp(argv[0], "load") && argc == 2) {
        do_load(argv[1]);
    } else if (!strcmp(argv[0], "shot") && argc == 2) {
        if (write_shot(argv[1])) reply("ok %s", argv[1]); else reply("err cannot write %s", argv[1]);
    } else if (!strcmp(argv[0], "vram") && argc == 2) {
        if (write_file(argv[1], g_vram, sizeof g_vram)) reply("ok %s", argv[1]);
        else reply("err cannot write %s", argv[1]);
    } else if (!strcmp(argv[0], "ram") && argc == 2) {
        if (write_file(argv[1], (const void *)0x80000000ul, 0x200000ul)) reply("ok %s", argv[1]);
        else reply("err cannot write %s", argv[1]);
    } else if ((!strcmp(argv[0], "qsave") || !strcmp(argv[0], "qload")) && qs_request_save) {
        int slot = argc > 1 ? atoi(argv[1]) : -1;
        if (argc > 1 && (argv[1][0] < '0' || argv[1][0] > '9') && qs_request_save_id) {
            /* an entry id: save0003, auto1, resume, ending (pc/hal/quicksave.h) */
            if (argv[0][1] == 's') qs_request_save_id(argv[1]); else qs_request_load_id(argv[1]);
        } else if (argv[0][1] == 's') {
            qs_request_save(slot);
        } else {
            qs_request_load(slot);
        }
        /* acted on at this same boundary (boundary_body runs qs_boundary next) */
        reply("ok %s %s requested at frame %ld", argv[0], argc > 1 ? argv[1] : "(selected)",
              (long)frame_counter);
    } else if (!strcmp(argv[0], "qslot") && argc == 2 && qs_select_slot) {
        qs_select_slot(atoi(argv[1]));
        reply("ok slot %s", argv[1]);
    } else if (!strcmp(argv[0], "qlist") && qs_slot_list) {
        char text[1200], *q;
        qs_slot_list(text, sizeof text);
        for (q = text; *q; q++) if (*q == '\n') *q = '|';
        reply("ok %s", text);
    } else if (!strcmp(argv[0], "qdel") && argc == 2 && qs_delete_id) {
        if (qs_delete_id(argv[1]) == 0) reply("ok deleted %s", argv[1]);
        else reply("err no entry %s", argv[1]);
    } else if ((!strcmp(argv[0], "rewind") || (!strcmp(argv[0], "rewind_to") && argc == 2)) &&
               rw_command) {
        int f = argv[0][6] ? rw_command(0, strtol(argv[1], NULL, 0))
                           : rw_command(argc > 1 ? atoi(argv[1]) : 0, -1);
        if (f >= 0) reply("ok rewound to frame %d", f);
        else reply("err nothing to rewind to");
    } else if (!strcmp(argv[0], "winquit")) {
        /* the window's close button / the quit hotkey (pc/hal/system.c):
         * with [saves] resume the quit waits for the resume state */
        hal_request_quit();
        reply("ok quit requested at frame %ld", (long)frame_counter);
    } else if (!strcmp(argv[0], "winclose")) {
        SDL_Event e;
        memset(&e, 0, sizeof e);
        e.type = SDL_QUIT;
        SDL_PushEvent(&e);
        reply("ok close event queued at frame %ld", (long)frame_counter);
    } else if (!strcmp(argv[0], "keys") && argc >= 2 && pad_queue_keys) {
        int k;
        for (k = 1; k < argc; k++) pad_queue_keys(argv[k]);
        reply("ok %d key(s) queued at frame %ld", argc - 1, (long)frame_counter);
    } else if (!strcmp(argv[0], "anchor")) {
        trace_anchor();
        reply("ok anchored at frame %ld", (long)frame_counter);
    } else if (!strcmp(argv[0], "debug") && argc == 2) {
        dbg_plat_setenv("PC_DEBUG", argv[1]);
        g_pc_debug_initialized = 0;   /* pc_debug_init() re-reads PC_DEBUG */
        g_pc_debug_flags = 0;
        reply("ok PC_DEBUG=%s", argv[1]);
    } else if (!strcmp(argv[0], "gp0") && argc == 2) {
        int *t = (int *)dbg_sym_lookup("gpu.c:g_gp0_trace", NULL);
        if (!t) { reply("err gpu.c:g_gp0_trace not found"); return 1; }
        *t = !strcmp(argv[1], "on");
        reply("ok GP0 trace %s", *t ? "on" : "off");
    } else if (!strcmp(argv[0], "echo")) {
        reply("ok %s", argc > 1 ? argv[1] : "");
    } else if (!strcmp(argv[0], "quit")) {
        reply("ok quit");
        quit_now(argc > 1 ? atoi(argv[1]) : 0);
    } else {
        reply("err unknown command %s (help)", argv[0]);
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* command script                                                      */
/* ------------------------------------------------------------------ */

enum { WHEN_NOW, WHEN_AT, WHEN_AFTER, WHEN_COND };
typedef struct {
    int kind;
    long n;
    Cond cond[8];
    int ncond;
    char cmd[512];
    int lineno;
} Line;
static Line *g_lines;
static int g_nlines, g_pos;
static long g_last_fire = -1;

static int load_script(const char *path) {
    FILE *f = fopen(path, "r");
    char buf[1024];
    int lineno = 0;
    if (!f) { fprintf(stderr, "[DBG] cannot open PC_DBG_CMDS %s\n", path); return 0; }
    while (fgets(buf, sizeof buf, f)) {
        char *p = buf, *hash;
        Line *l;
        lineno++;
        hash = strchr(p, '#');
        if (hash) *hash = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n' || *p == '\r') continue;
        g_lines = dbg_mem_realloc(g_lines, (unsigned long)(g_nlines + 1) * sizeof *g_lines);
        if (!g_lines) { fclose(f); return 0; }
        l = &g_lines[g_nlines];
        memset(l, 0, sizeof *l);
        l->lineno = lineno;
        if (!strncmp(p, "at ", 3) || !strncmp(p, "after ", 6)) {
            l->kind = p[1] == 't' ? WHEN_AT : WHEN_AFTER;
            p += l->kind == WHEN_AT ? 3 : 6;
            l->n = strtol(p, &p, 0);
        } else if (!strncmp(p, "when ", 5)) {
            char *tok;
            l->kind = WHEN_COND;
            p += 5;
            for (;;) {
                while (*p == ' ' || *p == '\t') p++;
                tok = p;
                while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
                if (*p) *p++ = 0;
                if (!*tok) { fprintf(stderr, "[DBG] %s:%d: `when` without `do`\n", path, lineno); fclose(f); return 0; }
                if (!strcmp(tok, "do")) break;
                if (l->ncond == 8 || !parse_cond(tok, &l->cond[l->ncond])) {
                    fprintf(stderr, "[DBG] %s:%d: bad condition %s\n", path, lineno, tok);
                    fclose(f);
                    return 0;
                }
                l->ncond++;
            }
        } else {
            l->kind = WHEN_NOW;
        }
        while (*p == ' ' || *p == '\t') p++;
        snprintf(l->cmd, sizeof l->cmd, "%s", p);
        g_nlines++;
    }
    fclose(f);
    printf("[DBG] PC_DBG_CMDS %s: %d lines\n", path, g_nlines);
    return 1;
}

static void script_step(void) {
    while (g_pos < g_nlines) {
        Line *l = &g_lines[g_pos];
        int i, ready = 0;
        long f = (long)frame_counter;
        switch (l->kind) {
        case WHEN_NOW: ready = 1; break;
        case WHEN_AT: ready = f >= l->n; break;
        case WHEN_AFTER: ready = f >= (g_last_fire < 0 ? 0 : g_last_fire) + l->n; break;
        case WHEN_COND:
            ready = 1;
            for (i = 0; i < l->ncond && ready; i++) ready = cond_holds(&l->cond[i]);
            break;
        }
        if (!ready) return;
        printf("[DBG] cmds line %d at frame %ld: %s", l->lineno, f, l->cmd);
        if (!strchr(l->cmd, '\n')) printf("\n");
        g_pos++;
        g_out_fd = -1;
        run_command(l->cmd);
        /* `after N` counts from here -- after a `load`, the loaded frame */
        g_last_fire = (long)frame_counter;
        /* a pause stops the script here; resume continues it */
        if (g_paused) return;
    }
}

/* ------------------------------------------------------------------ */
/* entry points                                                        */
/* ------------------------------------------------------------------ */

/* the live console (pc/hal/dbg_console.c) runs each line it reads here;
 * replies go to OUT_FD (-1: stdout) */
void dbg_run_line(char *line, int out_fd) {
    g_out_fd = out_fd;
    run_command(line);
    g_out_fd = -1;
}

/* the console takes its next line only when no `step N` is running */
int dbg_console_ready(void) {
    return g_step_left < 0;
}

/* weak: the console transport is a separate module */
extern int dbg_console_open(const char *spec) __attribute__((weak));
extern void dbg_console_poll(void) __attribute__((weak));

void dbg_request_save(const char *path) {
    snprintf(g_req_save, sizeof g_req_save, "%s", path && *path ? path : g_slot);
    g_active = 1;
}
void dbg_request_load(const char *path) {
    snprintf(g_req_load, sizeof g_req_load, "%s", path && *path ? path : g_slot);
    g_active = 1;
}
void dbg_request_pause_toggle(void) {
    g_paused = !g_paused;
    g_step_left = -1;
    g_active = 1;
    printf("[DBG] %s at frame %ld\n", g_paused ? "paused" : "running", (long)frame_counter);
}
void dbg_toolkit_activate(void) {
    g_active = 1;
}
void dbg_request_step(int frames) {
    g_paused = 0;
    g_step_left = frames > 1 ? frames - 1 : 0;
    g_active = 1;
}

void dbg_toolkit_init(void) {
    const char *e;
    if ((e = getenv("PC_DBG_SLOT")) && *e) snprintf(g_slot, sizeof g_slot, "%s", e);
    if ((e = getenv("PC_SAVESTATE")) && *e) {
        const char *p = e;
        while (*p && g_nsaves < 16) {
            SaveAt *s = &g_saves[g_nsaves];
            const char *colon = strchr(p, ':'), *comma;
            size_t n;
            if (!colon) break;
            s->frame = strtol(p, NULL, 10);
            comma = strchr(colon + 1, ',');
            n = comma ? (size_t)(comma - colon - 1) : strlen(colon + 1);
            if (n >= sizeof s->path) n = sizeof s->path - 1;
            memcpy(s->path, colon + 1, n);
            s->path[n] = 0;
            printf("[DBG] PC_SAVESTATE: %s at game frame %ld\n", s->path, s->frame);
            g_nsaves++;
            if (!comma) break;
            p = comma + 1;
        }
        g_active = 1;
    }
    if ((e = getenv("PC_LOADSTATE")) && *e) {
        snprintf(g_load_path, sizeof g_load_path, "%s", e);
        g_active = 1;
    }
    if ((e = getenv("PC_DBG_CMDS")) && *e) {
        if (!load_script(e)) { fflush(NULL); _exit(2); }
        g_active = 1;
    }
    if ((e = getenv("PC_DBG_CONSOLE")) && *e) {
        if (dbg_console_open) dbg_console_open(e);
        else fprintf(stderr, "[DBG] PC_DBG_CONSOLE: no console in this build\n");
        g_active = 1;
    }
    if (rw_wanted && rw_wanted()) g_active = 1;   /* rewind snapshots every few frames */
}

/* The toolkit's own stack.  Its work (file I/O, SDL polling while paused,
 * the console) runs there (dbg_plat_run_on_toolkit_stack: a ucontext on
 * Linux, a fiber on Windows), not below the game's main-loop frame: game
 * code that reads uninitialised locals sees what earlier game frames left
 * on the game stack, and a deep toolkit call chain would change that.  The
 * game stack below the boundary is part of a save state (dbg_state.c). */
unsigned long dbg_boundary_sp;     /* the game stack pointer at the boundary */
static void boundary_body(void);

/* the window of the game stack a state carries (dbg_state.c STACK_SNAP) */
#define DBG_STACK_WINDOW (512ul << 10)

void dbg_frame_boundary(void) {
    volatile char here;
    static int prepared;
    if (!g_active) return;
    dbg_boundary_sp = (unsigned long)&here;
    if (!prepared) {
        /* Windows commits a thread's stack page by page: make the window a
         * state copies (and a load writes) addressable, without changing a
         * byte of it.  Nothing to do on Linux. */
        dbg_plat_stack_prepare(DBG_STACK_WINDOW);
        prepared = 1;
    }
    dbg_plat_run_on_toolkit_stack(boundary_body);
}

static void boundary_body(void) {
    int i;

    if (g_load_path[0]) {
        const char *why = "";
        if (dbg_state_load(g_load_path, &why)) {
            fprintf(stderr, "[DBG] PC_LOADSTATE %s: %s\n", g_load_path, why);
            fflush(NULL);
            _exit(2);
        }
        printf("[DBG] PC_LOADSTATE %s: %s\n", g_load_path, why);
        g_load_path[0] = 0;
    }
    if (g_req_load[0]) {
        char path[1024];
        snprintf(path, sizeof path, "%s", g_req_load);
        g_req_load[0] = 0;
        do_load(path);
    }
    if (g_req_save[0]) {
        char path[1024];
        snprintf(path, sizeof path, "%s", g_req_save);
        g_req_save[0] = 0;
        do_save(path);
    }
    if (qs_boundary) qs_boundary();
    if (rw_boundary) rw_boundary();      /* rewind: a snapshot, or the held key */
    for (i = 0; i < g_nsaves; i++) {
        SaveAt *s = &g_saves[i];
        const char *why = "";
        int r;
        if (s->done || (long)frame_counter < s->frame) continue;
        r = dbg_state_save(s->path, &why);
        if (r > 0) {
            printf("[DBG] PC_SAVESTATE %s deferred at frame %ld: %s\n", s->path,
                   (long)frame_counter, why);
            continue;
        }
        s->done = 1;
        printf("[DBG] PC_SAVESTATE %s: %s%s\n", s->path, r ? "FAILED: " : "", why);
        if (r) { fflush(NULL); _exit(2); }
    }
    fflush(stdout);

    if (g_step_left == 0) { g_paused = 1; g_step_left = -1; reply("ok stepped to frame %ld", (long)frame_counter); }
    if (g_step_left > 0) g_step_left--;

    for (;;) {
        if (dbg_console_poll) dbg_console_poll();
        if (!g_paused) script_step();
        /* a quicksave command just run (script / console) acts here, at
         * the boundary it was read at */
        if (qs_boundary) qs_boundary();
        /* a hotkey pressed while paused acts at once (a deferred save stays
         * pending for the next boundary) */
        if (g_req_load[0]) {
            char p[1024];
            snprintf(p, sizeof p, "%s", g_req_load);
            g_req_load[0] = 0;
            do_load(p);
        }
        if (g_req_save[0] && g_paused && !g_save_tried_paused) {
            char p[1024];
            snprintf(p, sizeof p, "%s", g_req_save);
            g_req_save[0] = 0;
            g_save_tried_paused = 1;
            do_save(p);
        }
        if (!g_paused) break;
        hal_poll_quit();   /* SDL events: the window and the hotkeys stay live */
        if (!hal_is_running()) break;
        SDL_Delay(5);
    }
    g_save_tried_paused = 0;
}
