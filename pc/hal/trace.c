/* SPDX-License-Identifier: GPL-3.0-or-later */
/* trace.c -- deterministic per-frame state trace, scripted input and turbo
 * for the PC side of pc/tools/trace_compare.py (the PS1 side is
 * the reference emulator's -trace / -tracereg, a local patch).
 *
 * Environment:
 *   PC_TRACE=FILE        one line per VBlank (as the PS1 side: one per emulated frame):
 *                          frame=<VBlank #> pad=<psx mask> <field>=<hex> ...
 *                        <hex> is the variable's bytes, little-endian, exactly
 *                        what a RAM read of the same address on the PS1 gives,
 *                        so the two traces compare byte for byte.
 *   PC_INPUT=FILE        scripted pad input, keyed by the GAME's frame counter
 *                        (frame_counter, 0x800EF858 -- the same clock the
 *                        differ aligns on).  Lines: `<frame>:<hexmask>` (PSX
 *                        button layout: SELECT=0001 START=0008 UP=0010 ...
 *                        CROSS=4000), `hold <n>` (frames a press is held,
 *                        default 2 -- the regtest default), `#` comments.
 *                        While a script is loaded the keyboard and AUTOPLAY
 *                        are ignored: the game sees only the script.
 *                        State-anchored lines (pc/tools/playthrough.py):
 *                          wait LABEL NAME[:SIZE]=VALUE ... [within N]
 *                            blocks until every condition holds at a VBlank
 *                            (NAME is a traced field or any symbol, dlsym;
 *                            VALUE an integer for SIZE <= 4, else hex bytes;
 *                            `!=` negates; NAME[:SIZE]&MASK=VALUE compares
 *                            only MASK's bits, SIZE <= 4; `branch LABEL N
 *                            [within] [mash]` + N `case LABEL INDEX COND...`
 *                            lines: the first case to hold is reached
 *                            (`branch LABEL -> CASE`) and the script goes on
 *                            at item INDEX; `goto INDEX` jumps -- the program
 *                            playthrough.py compiles from branch/case/end-
 *                            branch blocks), prints `[TRACE] PC_INPUT wait
 *                            LABEL reached ...` and anchors the presses that
 *                            follow; not reached within N VBlanks of its
 *                            start: prints `... NOT reached` and exits 3.
 *                            `mash MASK/K`: while the conditions do not
 *                            hold, press MASK every K game frames from the
 *                            wait's start (a player tapping through
 *                            dialogue until the screen comes);
 *                            `mash M1[*H1],M2[*H2],.../K` cycles through up
 *                            to 16 presses, one every K frames, or H + 5
 *                            after a press held H frames when that is
 *                            longer (a hand move between two taps).
 *                          +N:MASK[*H]  press N game frames after the
 *                            previous anchor or press (held H frames).
 *                          end N    exit 0, N game frames after the last
 *                            anchor or press.
 *                          poke N TARGET HEX  SEEDED RUNS ONLY
 *                            (pc/tools/seeded_starts.py): N frames after
 *                            the cursor, write HEX (bytes, up to 256) at
 *                            TARGET, a PS1 RAM address 0x80000000..
 *                            0x801FFFFF or a global by symbol name, before
 *                            that VBlank's trace line -- the order of the
 *                            reference's `-poke FRAME:ADDR:HEX`
 *                            (ApplyPokes before TraceFrame).  Prints
 *                            `[TRACE] PC_INPUT poke ... (SEEDED RUN)`.
 *                            The one anchored item that changes state: a
 *                            playthrough without `poke` lines never has one.
 *                        A wait starts once the previous press is released.
 *                        Anchoring only times the input -- like a player
 *                        waiting for the screen -- it never changes state
 *                        (`poke`, above, is the one exception).
 *   PC_TURBO=1          no frame pacing (hal_frame_end / FMV delays).
 *   PC_TRACE_FRAMES=N    exit cleanly after N budget-clock frames.
 *   PC_TRACE_PHASE=post_irq  legacy default; pre_irq samples before BIOS
 *                        callbacks, matching regtest's frame observer.
 *   PC_TRACE_CLOCK=game  budget clock (default); vblank explicitly counts
 *                        hardware frames for standalone loops such as TT.
 *                        Input and trace field clocks remain unchanged.
 *   PC_MARK_EVERY=N      print `[MARK] game frame F vblank V step S/s/ss`
 *                        (stdout, flushed) whenever frame_counter enters a
 *                        new multiple of N: a time base for the lines other
 *                        code writes to stderr unbuffered (UBSan reports,
 *                        [CRASH], [FOLD]) in a merged log (pc/tools/explorer.py).
 *   PC_POKE=G:FIELD=HEX  test fixture: set a traced field / global at game
 *                        frame G, or vN:FIELD=HEX N VBlanks after the
 *                        SCENE_NAV navigation (below).  Harness only.
 *
 * Apart from PC_POKE and a seeded start's `poke` lines (state-writing
 * fixtures, named as such), none of this changes what the game does: the
 * trace reads globals, the
 * script replaces the keyboard as the pad source (the matched pad chain
 * still runs), turbo only removes host sleeps.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <dlfcn.h>
#include "types.h"
#include "parity.h"
/* weak: the trace harness tests link this file without parity.c */
extern int parity_shot(const char *label) __attribute__((weak));

extern u8 now_step, now_sub_step, now_sub_sub_step, now_girl;
extern s32 frame_counter, step_frame_cnt, vblnk_counter;
extern s32 pad_data, pad_trig, pad_rel;
extern u8 cd_state2, step_mode;
extern s32 activeBuff;
extern volatile int g_crash_count;
extern u8 screen_w[];               /* the game-state block (pc/src/layout_aliases.ld lays it out as on the PS1) */
extern u32 g_psx_rand_seed;         /* the kernel rand() seed (src/main/pc-port/bios_trampolines.c; PS1 0xA0009010) */

/* The field table.  pc/tools/trace_compare.py carries the same list
 * (name, address, size) and derives the regtest -tracereg arguments from
 * it; keep the two in step (the tool checks the names of the first PC line). */
typedef struct { const char *name; u32 addr; u32 size; const void *ptr; } TraceField;
static const TraceField FIELDS[] = {
    { "now_step",         0x800EF868, 1, &now_step },
    { "now_sub_step",     0x800EF869, 1, &now_sub_step },
    { "now_sub_sub_step", 0x800EF86A, 1, &now_sub_sub_step },
    { "now_girl",         0x800EF6BF, 1, &now_girl },
    { "frame_counter",    0x800EF858, 4, &frame_counter },
    { "step_frame_cnt",   0x800EF85C, 4, &step_frame_cnt },
    { "vblnk_counter",    0x800EF854, 4, &vblnk_counter },
    { "pad_data",         0x800EF6E0, 4, &pad_data },
    { "pad_trig",         0x800EF6E8, 4, &pad_trig },
    { "pad_rel",          0x800EF6EC, 4, &pad_rel },
    { "cd_state2",        0x800E8DA2, 1, &cd_state2 },
    { "disp_buf",         0x80127180, 4, &activeBuff },
    { "step_mode",        0x800EF86D, 1, &step_mode },
    /* the kernel's rand() seed: every rand() call moves it, so equal seeds
     * mean the same random numbers were drawn */
    { "rand_seed",        0x80009010, 4, &g_psx_rand_seed },
    /* game state inside screen_w: the calendar (season_no, now_year,
     * now_month, now_day, now_youbi, pass_days) and the player record
     * (player_rec, p_sei, p_mei, p_adana, birthday, the nine para_* stats,
     * basyo flags) */
    { "calendar",         0x800EE79D, 7, screen_w + 0x3D },
    { "player",           0x800EE824, 0x6C, screen_w + 0xC4 },
};
#define NFIELDS (sizeof(FIELDS) / sizeof(FIELDS[0]))
static void load_pokes(const char *spec);
static long g_nav_vblanks = -1;   /* VBlanks since trace_anchor; -1 before */

/* PC_TRACE_SYMS=name[:size],...: extra fields, any global by its symbol
 * name (dlsym; the binary links with -rdynamic), traced after the fixed
 * ones.  trace_compare.py --syms passes the same names to the PS1 side as
 * -tracereg with their config/symbol_names.json addresses. */
typedef struct { char name[64]; u32 size; const u8 *ptr; } ExtraField;
static ExtraField g_extra[32];
static int g_nextra = 0;
static void load_extra(const char *spec) {
    const char *p = spec;
    while (*p && g_nextra < 32) {
        ExtraField *x = &g_extra[g_nextra];
        size_t n = strcspn(p, ",:");
        if (n >= sizeof x->name) n = sizeof x->name - 1;
        memcpy(x->name, p, n); x->name[n] = 0;
        p += n;
        x->size = 4;
        if (*p == ':') { x->size = (u32)strtoul(p + 1, (char **)&p, 0); if (!x->size || x->size > 256) x->size = 4; }
        x->ptr = (const u8 *)dlsym(RTLD_DEFAULT, x->name);
        if (x->ptr) g_nextra++;
        else fprintf(stderr, "[TRACE] PC_TRACE_SYMS: no symbol %s\n", x->name);
        if (*p == ',') p++;
    }
}

/* A traced field by name, else any global by symbol (dlsym). */
static const u8 *field_ptr(const char *name, u32 *size) {
    size_t i;
    for (i = 0; i < NFIELDS; i++)
        if (!strcmp(FIELDS[i].name, name)) { *size = FIELDS[i].size; return FIELDS[i].ptr; }
    *size = 0;
    return (const u8 *)dlsym(RTLD_DEFAULT, name);
}

static FILE *g_trace = NULL;
static int g_turbo = 0;
static long g_max_frames = -1;
static long g_host_frame = 0;
/* g_host_frame - the emulated VBlank count (pc/hal/cd.c), constant through
 * a run: both count every VBlank from power-on */
static long g_clock_offset;
static int g_clock_offset_valid;
/* pc/hal/cd.c; weak: the trace test harnesses link this file alone */
extern u32 hal_clock_vblanks(void) __attribute__((weak));
static int g_input_vblank = 0;   /* PC_INPUT `clock vblank` (seq_step) */
static long g_mark_every = 0, g_mark_last = -1;   /* PC_MARK_EVERY */

/* scripted input */
typedef struct { long frame; u32 mask; long hold; } Press;   /* hold 0: the script's `hold` */
static Press *g_presses = NULL;
static int g_npresses = 0, g_input_loaded = 0;
static long g_hold = 2;

static int press_cmp(const void *a, const void *b) {
    long d = ((const Press *)a)->frame - ((const Press *)b)->frame;
    return d < 0 ? -1 : d > 0;
}

static int g_press_cap = 0;
static int g_press_lo = 0;          /* script_psx_mask's scan floor */
static long g_press_maxhold = 0;    /* the longest explicit hold */
static void add_press(long frame, u32 mask, long hold) {
    if (g_npresses == g_press_cap) {
        g_press_cap = g_press_cap ? g_press_cap * 2 : 64;
        g_presses = realloc(g_presses, g_press_cap * sizeof *g_presses);
        if (!g_presses) { fprintf(stderr, "[TRACE] PC_INPUT: out of memory\n"); _exit(2); }
    }
    g_presses[g_npresses].frame = frame;
    g_presses[g_npresses].mask = mask;
    g_presses[g_npresses].hold = hold;
    g_npresses++;
    if (hold > g_press_maxhold) g_press_maxhold = hold;
}

/* add_press, keeping g_presses sorted by frame (the order qsort gave; the
 * mask ORs the presses, so the order of equal frames does not matter).
 * An anchored script appends its relative presses in order, so this is
 * O(1) per press where a qsort per press was quadratic: a 10,000-press
 * explorer script (pc/tools/explorer.py) spent minutes in one VBlank. */
static void insert_press(long frame, u32 mask, long hold) {
    int i;
    Press p;
    add_press(frame, mask, hold);
    for (i = g_npresses - 1; i > 0 && g_presses[i - 1].frame > frame; i--) {
        p = g_presses[i - 1]; g_presses[i - 1] = g_presses[i]; g_presses[i] = p;
    }
    if (i < g_press_lo) g_press_lo = i;
}

/* State-anchored input (see the header): waits, relative presses and an
 * end, consumed in file order at VBlank by seq_step(). */
typedef struct { const u8 *ptr; u32 size; u8 value[64]; u8 mask[64]; int negate; } WaitCond;
enum { SEQ_WAIT, SEQ_PRESS, SEQ_END, SEQ_SHOT, SEQ_BRANCH, SEQ_CASE, SEQ_GOTO, SEQ_POKE };
typedef struct {
    int kind;
    char label[48];
    WaitCond cond[8];
    int ncond;
    long within;            /* VBlanks; 0 = unbounded */
    long delta;             /* SEQ_PRESS / SEQ_END: game frames after the cursor */
    u32 mask;
    long hold;              /* SEQ_PRESS: frames held (0: the script's hold) */
    u32 mash_mask[16];       /* SEQ_WAIT: a cycle of presses, one every */
    long mash_hold[16];      /*   mash_every frames while the conditions */
    int mash_n, mash_i;     /*   do not hold (holds 0: the script's) */
    long mash_every;        /*   0 = off */
    long mash_gap;          /*   frames to the next press: K, or H + 5 */
    long mash_last;         /*   game frame of the last mash press */
    int ncases;             /* SEQ_BRANCH: the SEQ_CASE items that follow it */
    int target;             /* SEQ_CASE / SEQ_GOTO: the item to go on at */
    u8 *poke_ptr;           /* SEQ_POKE: where, the bytes, how many */
    u8 *poke_bytes;
    u32 poke_n;
    u32 poke_addr;          /*   the PS1 address (0: a symbol) */
} SeqItem;
static SeqItem *g_seq = NULL;
static int g_nseq = 0, g_seq_pos = 0, g_seq_cap = 0;
static long g_seq_cursor = 0;       /* game frame of the last anchor / press */
static long g_seq_ready = 0;        /* a wait is evaluated from this game frame */
static long g_seq_wait_start = -1;  /* VBlank the current wait started at */
static long g_end_frame = -1;

static int parse_cond(char *tok, WaitCond *c) {
    char *eq = strchr(tok, '='), *colon, *amp;
    u32 size = 0, fsize = 0, i, mask = 0xFFFFFFFFu;
    if (!eq || eq == tok) return 0;
    c->negate = eq[-1] == '!';
    if (c->negate) eq[-1] = 0;
    *eq = 0;
    amp = strchr(tok, '&');
    if (amp) { *amp = 0; mask = (u32)strtoul(amp + 1, NULL, 0); }
    colon = strchr(tok, ':');
    if (colon) { *colon = 0; size = (u32)strtoul(colon + 1, NULL, 0); }
    c->ptr = field_ptr(tok, &fsize);
    if (!c->ptr) { fprintf(stderr, "[TRACE] PC_INPUT wait: no field or symbol %s\n", tok); return 0; }
    c->size = size ? size : (fsize ? fsize : 4);
    if (c->size > sizeof c->value) return 0;
    memset(c->value, 0, sizeof c->value);
    memset(c->mask, 0xFF, sizeof c->mask);
    if (c->size <= 4) {
        u32 v = (u32)strtoul(eq + 1, NULL, 0);
        for (i = 0; i < c->size; i++) {
            c->mask[i] = (u8)(mask >> (8 * i));
            c->value[i] = (u8)(v >> (8 * i)) & c->mask[i];
        }
    } else {
        if (amp) return 0;
        const char *h = eq + 1;
        for (i = 0; i < c->size; i++) {
            char hx[3];
            if (!isxdigit((unsigned char)h[0]) || !isxdigit((unsigned char)h[1])) return 0;
            hx[0] = h[0]; hx[1] = h[1]; hx[2] = 0;
            c->value[i] = (u8)strtoul(hx, NULL, 16);
            h += 2;
        }
    }
    return 1;
}

static SeqItem *seq_new(int kind) {
    SeqItem *it;
    if (g_nseq == g_seq_cap) {
        g_seq_cap = g_seq_cap ? g_seq_cap * 2 : 64;
        g_seq = realloc(g_seq, g_seq_cap * sizeof *g_seq);
        if (!g_seq) { fprintf(stderr, "[TRACE] PC_INPUT: out of memory\n"); _exit(2); }
    }
    it = &g_seq[g_nseq++];
    memset(it, 0, sizeof *it);
    it->kind = kind;
    return it;
}

/* Next whitespace-separated token of *pp (terminated in place), or NULL. */
static char *next_token(char **pp) {
    char *p = *pp, *tok;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (!*p) { *pp = p; return NULL; }
    tok = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
    if (*p) *p++ = 0;
    *pp = p;
    return tok;
}

static int parse_wait_kind(char *p, int kind) {
    SeqItem *it = seq_new(kind);
    char *tok = next_token(&p);
    if (!tok) return 0;
    snprintf(it->label, sizeof it->label, "%s", tok);
    if (kind != SEQ_WAIT) {
        /* branch LABEL NCASES ... / case LABEL TARGET COND... */
        tok = next_token(&p);
        if (!tok) return 0;
        if (kind == SEQ_BRANCH) it->ncases = (int)strtol(tok, NULL, 10);
        else it->target = (int)strtol(tok, NULL, 10);
    }
    while ((tok = next_token(&p))) {
        if (!strcmp(tok, "within")) {
            tok = next_token(&p);
            if (!tok) return 0;
            it->within = strtol(tok, NULL, 10);
            continue;
        }
        if (!strcmp(tok, "mash")) {        /* mash MASK[*H][,MASK[*H]...]/K */
            char *slash, *q;
            tok = next_token(&p);
            if (!tok || !(slash = strchr(tok, '/'))) return 0;
            it->mash_every = strtol(slash + 1, NULL, 10);
            it->mash_n = 0;
            for (q = tok; q < slash; ) {
                char *end;
                long h;
                if (it->mash_n == 16) return 0;
                it->mash_mask[it->mash_n] = (u32)strtoul(q, &end, 16) & 0xFFFF;
                it->mash_hold[it->mash_n] = 0;
                if (*end == '*') it->mash_hold[it->mash_n] = strtol(end + 1, &end, 10);
                if (!it->mash_mask[it->mash_n] || it->mash_every <= g_hold) return 0;
                it->mash_n++;
                q = (*end == ',') ? end + 1 : slash;
            }
            if (!it->mash_n) return 0;
            continue;
        }
        if (kind == SEQ_BRANCH) return 0;  /* conditions belong to the cases */
        if (it->ncond == 8 || !parse_cond(tok, &it->cond[it->ncond])) return 0;
        it->ncond++;
    }
    return kind == SEQ_BRANCH ? it->ncases >= 2 : it->ncond > 0;
}
static int parse_wait(char *p) { return parse_wait_kind(p, SEQ_WAIT); }

/* A branch is followed by its ncases SEQ_CASE items, and every jump lands
 * inside the program (its end included). */
static int seq_valid(void) {
    int i, k;
    for (i = 0; i < g_nseq; i++) {
        SeqItem *it = &g_seq[i];
        if (it->kind == SEQ_BRANCH) {
            if (i + it->ncases >= g_nseq) return 0;
            for (k = 1; k <= it->ncases; k++) if (g_seq[i + k].kind != SEQ_CASE) return 0;
        }
        if ((it->kind == SEQ_CASE || it->kind == SEQ_GOTO) &&
                (it->target < 0 || it->target > g_nseq))
            return 0;
    }
    return 1;
}

/* `poke N TARGET HEX` (seeded runs): TARGET is 0xADDR inside PS1 RAM
 * (0x80000000..0x801FFFFF, the port maps it at those addresses,
 * pc/src/ps1_regions.c) or a global's symbol name. */
#define PS1_RAM_BASE 0x80000000u
#define PS1_RAM_SIZE 0x200000u
static int parse_poke(char *p) {
    SeqItem *it = seq_new(SEQ_POKE);
    char *tok = next_token(&p), *hex;
    u32 size = 0, n, i;
    if (!tok) return 0;
    it->delta = strtol(tok, NULL, 10);
    if (it->delta < 0) return 0;
    if (!(tok = next_token(&p)) || !(hex = next_token(&p))) return 0;
    n = (u32)strlen(hex);
    if (n == 0 || n % 2 || n / 2 > 256) return 0;
    n /= 2;
    if (tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) {
        u32 a = (u32)strtoul(tok, NULL, 16);
        size_t f;
        if (a < PS1_RAM_BASE || a - PS1_RAM_BASE > PS1_RAM_SIZE - n) return 0;
        it->poke_addr = a;
        it->poke_ptr = (u8 *)(unsigned long)a;
        /* a traced field the port keeps elsewhere (rand_seed: the kernel's
         * 0x80009010 is g_psx_rand_seed here) is written where the trace
         * reads it, and only whole */
        for (f = 0; f < NFIELDS; f++) {
            if (FIELDS[f].ptr == (const void *)(unsigned long)FIELDS[f].addr) continue;
            if (a < FIELDS[f].addr + FIELDS[f].size && FIELDS[f].addr < a + n) {
                if (a != FIELDS[f].addr || n != FIELDS[f].size) return 0;
                it->poke_ptr = (u8 *)FIELDS[f].ptr;
            }
        }
    } else {
        it->poke_ptr = (u8 *)field_ptr(tok, &size);
        if (!it->poke_ptr || (size && size != n)) return 0;
    }
    it->poke_bytes = malloc(n);
    if (!it->poke_bytes) return 0;
    for (i = 0; i < n; i++) {
        char hx[3];
        if (!isxdigit((unsigned char)hex[2 * i]) || !isxdigit((unsigned char)hex[2 * i + 1]))
            return 0;
        hx[0] = hex[2 * i]; hx[1] = hex[2 * i + 1]; hx[2] = 0;
        it->poke_bytes[i] = (u8)strtoul(hx, NULL, 16);
    }
    it->poke_n = n;
    snprintf(it->label, sizeof it->label, "%s", tok);
    return 1;
}

static void load_input(const char *path) {
    FILE *f = fopen(path, "r");
    char line[1024];
    if (!f) { fprintf(stderr, "[TRACE] cannot open PC_INPUT %s\n", path); return; }
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == 0) continue;
        if (!strncmp(p, "hold", 4)) { g_hold = strtol(p + 4, NULL, 10); if (g_hold < 1) g_hold = 1; continue; }
        if (!strncmp(p, "clock ", 6)) {
            if (!strncmp(p + 6, "vblank", 6)) g_input_vblank = 1;
            else if (strncmp(p + 6, "game", 4)) {
                fprintf(stderr, "[TRACE] PC_INPUT: clock must be game or vblank: %s", line);
                _exit(2);
            }
            continue;
        }
        if (!strncmp(p, "wait ", 5)) {
            if (!parse_wait(p + 5)) {
                fprintf(stderr, "[TRACE] PC_INPUT: bad wait line: %s", line);
                _exit(2);
            }
            continue;
        }
        if (!strncmp(p, "branch ", 7) || !strncmp(p, "case ", 5)) {
            int branch = p[0] == 'b';
            if (!parse_wait_kind(p + (branch ? 7 : 5), branch ? SEQ_BRANCH : SEQ_CASE)) {
                fprintf(stderr, "[TRACE] PC_INPUT: bad %s line: %s", branch ? "branch" : "case", line);
                _exit(2);
            }
            continue;
        }
        if (!strncmp(p, "goto ", 5)) {
            seq_new(SEQ_GOTO)->target = (int)strtol(p + 5, NULL, 10);
            continue;
        }
        if (!strncmp(p, "poke ", 5)) {
            if (!parse_poke(p + 5)) {
                fprintf(stderr, "[TRACE] PC_INPUT: bad poke line: %s", line);
                _exit(2);
            }
            continue;
        }
        if (!strncmp(p, "end", 3)) {
            seq_new(SEQ_END)->delta = strtol(p + 3, NULL, 10);
            continue;
        }
        if (!strncmp(p, "shot ", 5)) {
            SeqItem *it = seq_new(SEQ_SHOT);
            char label[48];
            long delta = 0;
            if (sscanf(p + 5, "%47s %ld", label, &delta) < 1) continue;
            snprintf(it->label, sizeof it->label, "%s", label);
            it->delta = delta;
            continue;
        }
        if (*p == '+') {
            char *colon = strchr(p, ':');
            SeqItem *it;
            if (!colon) continue;
            it = seq_new(SEQ_PRESS);
            it->delta = strtol(p + 1, NULL, 10);
            it->mask = (u32)strtoul(colon + 1, &colon, 16);
            if (*colon == '*') it->hold = strtol(colon + 1, NULL, 10);
            continue;
        }
        {
            char *colon = strchr(p, ':');
            if (!colon) continue;
            add_press(strtol(p, NULL, 10), (u32)strtoul(colon + 1, NULL, 16), 0);
        }
    }
    fclose(f);
    if (!seq_valid()) {
        fprintf(stderr, "[TRACE] PC_INPUT: a branch without its cases or a jump outside the script\n");
        _exit(2);
    }
    if (g_npresses)   /* an anchored-only script has none (qsort(NULL): UBSan) */
        qsort(g_presses, g_npresses, sizeof *g_presses, press_cmp);
    g_input_loaded = 1;
    printf("[TRACE] PC_INPUT %s: %d presses, %d anchored items, hold %ld, clock %s\n",
           path, g_npresses, g_nseq, g_hold, g_input_vblank ? "vblank" : "game");
}

static int cond_holds(const WaitCond *c) {
    u32 i;
    int eq = 1;
    for (i = 0; i < c->size && eq; i++) eq = (c->ptr[i] & c->mask[i]) == c->value[i];
    return c->negate ? !eq : eq;
}

/* pc/hal/async_io.c (weak: the trace harnesses link this file alone) */
extern void pc_async_flush(void) __attribute__((weak));
/* pc/hal/instrument.c: REACH_LIST_FILE's function list (no-op otherwise) */
extern void instrument_reach_flush(void) __attribute__((weak));

/* pc/hal/cpu_cost.c: CPU_COST_PROFILE's per-function entry counts */
extern void hal_cpu_cost_flush(void) __attribute__((weak));

/* pc/src/main.c: the fault fold's recovery count ([FOLD] total=) */
extern void pc_fold_report(void) __attribute__((weak));

extern void music_pack_flush(void) __attribute__((weak));
/* pc/hal/spu.c: the run's audio-health totals in the log */
extern void spu_audio_health_final(void) __attribute__((weak));

static void reach_flush(void) {
    if (spu_audio_health_final) spu_audio_health_final();
    /* a song dump being written (pc/hal/music_pack.c) */
    if (music_pack_flush) music_pack_flush();
    if (pc_fold_report) pc_fold_report();
    if (instrument_reach_flush) instrument_reach_flush();
    if (hal_cpu_cost_flush) hal_cpu_cost_flush();
}

/* PC_INPUT_DUMP=PATH (pc/tools/savestate_roundtrip.py): at the end of the
 * run, every press the input produced -- the script's absolute lines, the
 * anchored presses as they were placed and each `mash` repeat -- as an
 * absolute PC_INPUT of single-frame lines in the same clock, so a later run
 * can replay exactly this input from any saved state.  Observation only. */
static void input_dump(void) {
    const char *path = getenv("PC_INPUT_DUMP");
    FILE *f;
    int i;
    long k;
    if (!path || !*path || !(f = fopen(path, "w"))) return;
    fprintf(f, "# every press of this run (pc/hal/trace.c PC_INPUT_DUMP)\nhold 1\nclock %s\n",
            g_input_vblank ? "vblank" : "game");
    for (i = 0; i < g_npresses; i++) {
        long h = g_presses[i].hold ? g_presses[i].hold : g_hold;
        if (!g_presses[i].mask) continue;
        for (k = 0; k < h; k++) fprintf(f, "%ld:%04x\n", g_presses[i].frame + k, (unsigned)g_presses[i].mask);
    }
    fclose(f);
}

/* pc/hal/card_writer.c (weak: harnesses link this file alone) */
extern void card_writer_wait(void) __attribute__((weak));

static void finish(int code) {
    if (card_writer_wait) card_writer_wait();   /* the memory card file is on disk */
    input_dump();
    if (g_trace) fclose(g_trace);
    g_trace = NULL;
    reach_flush();
    fflush(NULL);
    _exit(code);
}

/* `clock vblank` (PC_INPUT's header): presses, ends, shots and mash
 * repeats count VBlanks (hardware frames) after their anchor, as the
 * reference's -input does (the reference emulator places a press by host
 * frame; playthrough.py run_retail places each one at its milestone's host
 * frame + N).  Without it they count game frames (frame_counter), which
 * differ from VBlanks wherever a frame overruns: a press after the name
 * entry's kana overrun landed one VBlank later on the reference than on
 * the port, so the two sides saw different frames.  Waits and `within`
 * are unchanged (conditions are read at every VBlank either way). */
static long input_clock(void) {
    return g_input_vblank ? g_host_frame : (long)frame_counter;
}

/* Once per VBlank, before the pad poll: advance the anchored sequence. */
static void seq_step(void) {
    long g = input_clock();
    if (g_end_frame >= 0 && g >= g_end_frame) {
        printf("[TRACE] PC_INPUT end at game frame %ld vblank %ld\n", (long)frame_counter, g_host_frame);
        finish(0);
    }
    while (g_seq_pos < g_nseq) {
        SeqItem *it = &g_seq[g_seq_pos];
        if (it->kind == SEQ_PRESS) {
            g_seq_cursor += it->delta;
            insert_press(g_seq_cursor, it->mask & 0xFFFF, it->hold);
            g_seq_ready = g_seq_cursor + (it->hold > 0 ? it->hold : g_hold);
            g_seq_pos++;
            continue;
        }
        if (it->kind == SEQ_END) {
            g_end_frame = g_seq_cursor + it->delta;
            g_seq_pos++;
            continue;
        }
        if (it->kind == SEQ_SHOT) {
            /* the displayed image N game frames after the cursor */
            if (g < g_seq_cursor + it->delta) return;
            g_seq_cursor += it->delta;
            if (parity_shot && parity_shot(it->label))
                printf("[TRACE] PC_INPUT shot %s: game frame %ld vblank %ld\n",
                       it->label, (long)frame_counter, g_host_frame);
            g_seq_pos++;
            continue;
        }
        if (it->kind == SEQ_POKE) {
            /* a seeded run's state, at its defined point (see the header) */
            if (g < g_seq_cursor + it->delta) return;
            g_seq_cursor += it->delta;
            memcpy(it->poke_ptr, it->poke_bytes, it->poke_n);
            printf("[TRACE] PC_INPUT poke %s %u bytes: game frame %ld vblank %ld (SEEDED RUN)\n",
                   it->label, (unsigned)it->poke_n, (long)frame_counter, g_host_frame);
            g_seq_pos++;
            continue;
        }
        if (it->kind == SEQ_GOTO) {
            g_seq_pos = it->target;
            continue;
        }
        if (it->kind == SEQ_CASE) {         /* only reached through its branch */
            g_seq_pos++;
            continue;
        }
        /* SEQ_WAIT, or SEQ_BRANCH: the first of its cases whose conditions
         * hold is the milestone reached, and the script goes on at it */
        if (g < g_seq_ready) return;
        if (g_seq_wait_start < 0) {
            g_seq_wait_start = g_host_frame;
            it->mash_last = g;
            it->mash_i = 0;
            it->mash_gap = it->mash_every;
        }
        {
            int i, ok = 1, next = g_seq_pos + 1;
            const SeqItem *hit = it;
            if (it->kind == SEQ_BRANCH) {
                int k;
                ok = 0;
                for (k = 1; k <= it->ncases && !ok; k++) {
                    const SeqItem *c = &g_seq[g_seq_pos + k];
                    ok = 1;
                    for (i = 0; i < c->ncond && ok; i++) ok = cond_holds(&c->cond[i]);
                    if (ok) { hit = c; next = c->target; }
                }
            } else {
                for (i = 0; i < it->ncond && ok; i++) ok = cond_holds(&it->cond[i]);
            }
            if (!ok) {
                if (it->within > 0 && g_host_frame - g_seq_wait_start > it->within) {
                    printf("[TRACE] PC_INPUT wait %s NOT reached within %ld vblanks "
                           "(game frame %ld, vblank %ld)\n", it->label, it->within,
                           (long)frame_counter, g_host_frame);
                    finish(3);
                }
                /* mash: the press a player repeats until the screen comes
                 * (dialogue lines); never on the frame the wait holds */
                if (it->mash_every > 0 && g - it->mash_last >= it->mash_gap) {
                    long h = it->mash_hold[it->mash_i];   /* 0: the script's hold */
                    insert_press(g, it->mash_mask[it->mash_i], h);
                    it->mash_gap = h + 5 > it->mash_every ? h + 5 : it->mash_every;
                    it->mash_i = (it->mash_i + 1) % it->mash_n;
                    it->mash_last = g;
                }
                return;
            }
            if (it->kind == SEQ_BRANCH) {
                printf("[TRACE] PC_INPUT wait %s reached: game frame %ld vblank %ld\n",
                       it->label, (long)frame_counter, g_host_frame);
                printf("[TRACE] PC_INPUT branch %s -> %s\n", it->label, hit->label);
            }
            printf("[TRACE] PC_INPUT wait %s reached: game frame %ld vblank %ld\n",
                   hit->label, (long)frame_counter, g_host_frame);
            fflush(stdout);
            g_seq_cursor = g;
            g_seq_ready = g;
            g_seq_wait_start = -1;
            g_seq_pos = next;
        }
    }
}

/* Scene navigation (pc/src/main.c, SCENE_NAV): the boot runs first, so the
 * frame budget counts from the navigation, not from power-on.  trace_anchor()
 * moves the PC_TRACE_FRAMES base to the current game frame. */
static long g_anchor = 0;
static int g_vblank_budget = 0;
static int g_pre_irq = 0;
static int g_budget_hold = 0;   /* SCENE_NAV: budget starts at trace_anchor() */
static long budget_now(void) {
    return g_vblank_budget ? g_host_frame : (long)frame_counter;
}
void trace_anchor(void) {
    g_anchor = budget_now();
    g_budget_hold = 0;
    g_nav_vblanks = 0;
    parity_anchor();
}

void trace_init(void) {
    const char *e;
    if ((e = getenv("PC_TRACE_PHASE")) && *e) {
        if (!strcmp(e, "pre_irq")) g_pre_irq = 1;
        else if (strcmp(e, "post_irq")) {
            fprintf(stderr, "[TRACE] invalid PC_TRACE_PHASE\n");
            _exit(2);
        }
    }
    if ((e = getenv("PC_TRACE")) && *e) {
        g_trace = fopen(e, "w");
        if (!g_trace) fprintf(stderr, "[TRACE] cannot open PC_TRACE %s\n", e);
        else {
            printf("[TRACE] PC_TRACE -> %s\n", e);
            fprintf(g_trace, "# observation=%s\n",
                    g_pre_irq ? "pre_irq" : "post_irq");
        }
    }
    if ((e = getenv("PC_INPUT")) && *e) load_input(e);
    if ((e = getenv("PC_TURBO")) && *e && *e != '0') { g_turbo = 1; printf("[TRACE] PC_TURBO on\n"); }
    if ((e = getenv("PC_TRACE_FRAMES")) && *e) g_max_frames = strtol(e, NULL, 10);
    if ((e = getenv("PC_MARK_EVERY")) && *e) g_mark_every = strtol(e, NULL, 10);
    if ((e = getenv("PC_TRACE_CLOCK")) && *e) {
        if (!strcmp(e, "vblank")) g_vblank_budget = 1;
        else if (strcmp(e, "game")) {
            fprintf(stderr, "[TRACE] PC_TRACE_CLOCK must be game or vblank\n");
            _exit(2);
        }
    }
    if (getenv("SCENE_NAV")) g_budget_hold = 1;   /* budget counts post-nav */
    if ((e = getenv("PC_POKE")) && *e) load_pokes(e);
    if ((e = getenv("PC_TRACE_SYMS")) && *e) load_extra(e);
}

int trace_turbo(void) { return g_turbo; }
int trace_input_active(void) { return g_input_loaded; }

/* PSX button mask the script asks for at the current GAME frame (the same
 * hold rule as regtest's MaskForFrame: a press at f is held for [f, f+hold)). */
static u32 script_psx_mask(long game_frame) {
    static long last = -1;
    long maxhold = g_press_maxhold > g_hold ? g_press_maxhold : g_hold;
    u32 mask = 0;
    int i;
    /* presses that ended before this frame never count again: the scan
     * starts past them (the clock only moves forward, except across a
     * hot-loaded state, which rescans from the start) */
    if (game_frame < last) g_press_lo = 0;
    last = game_frame;
    while (g_press_lo < g_npresses &&
           g_presses[g_press_lo].frame + maxhold <= game_frame)
        g_press_lo++;
    for (i = g_press_lo; i < g_npresses; i++) {
        if (g_presses[i].frame > game_frame) break;
        if (game_frame < g_presses[i].frame +
            (g_presses[i].hold > 0 ? g_presses[i].hold : g_hold))
            mask |= g_presses[i].mask;
    }
    return mask & 0xFFFF;
}

/* The pad HAL's bit layout is the PSX mask byte-swapped (pad.c: PAD_CROSS
 * 0x0040, PAD_SELECT 0x0100, PAD_UP 0x1000). */
u32 trace_input_mask_pc(void) {
    u32 psx = script_psx_mask(input_clock());
    return ((psx & 0xFF) << 8) | ((psx >> 8) & 0xFF);
}

/* One line per VBlank, raised by the system clock's VBlank interrupt
 * (pc/hal/system.c hal_vblank_interrupt) -- the PS1 side writes one line per
 * emulated frame. Its observer runs BEFORE the CPU services the new IRQ;
 * select pre_irq to compare that boundary, not the legacy post-callback state.
 * Both boundaries fall within the game's frame (inside
 * vblank_function's VSync(0), inside movie_main's VSync(0) during the boot
 * movie, during a CD wait).  `frame=` counts VBlanks since the PC started;
 * the differ aligns on frame_counter, not on it.  (Until 2026-10-16 the PC
 * wrote one line per main-loop iteration, so the boot movie -- one main-loop
 * iteration, 150 game frames -- was invisible.) */
/* PC_POKE=G:FIELD=HEX[,G:FIELD=HEX...]: a test fixture for parity runs.
 * At the first VBlank at which frame_counter >= G, write HEX (bytes, as
 * the trace prints them) into FIELD before the line is written, at the
 * selected pre_irq/post_irq observation boundary.
 *
 * vN:FIELD=HEX instead counts hardware VBlanks after the navigation
 * (trace_anchor, SCENE_NAV), the clock of PC_PARITY_FRAMES checkpoints,
 * and writes right after the parity capture of VBlank N -- the reference
 * equivalent is the reference emulator's `-poke <anchor+N>:ADDR:HEX`, which
 * its frame hook applies after ParityReference::OnFrame captured
 * checkpoint N (the local build_parity_reference.py, pc/tools/refemu.py).  Used for the
 * scene-64 window-sprite comparison: `v1:class_flag=01` pairs with the
 * reference's `-poke 1382:800EF872:01` (anchor 1381).  Without a
 * navigation a vN entry never fires.
 *
 * FIELD is a traced field (FIELDS) or, for vN and G alike, any global by
 * its symbol name (field_ptr / dlsym); a traced field must get exactly its
 * size in bytes.  These intentionally state-mutating fixtures are not
 * natural boot parity evidence, and nothing sets PC_POKE in a normal run
 * (playthrough.py and av_parity.py strip PC_* and scene_matrix.py drops
 * PC_POKE; av_parity.py passes one only for --fixture-poke). */
typedef struct { long frame; int nav; u8 *ptr; u8 bytes[128]; u32 n; int done; } Poke;
static Poke g_pokes[16];
static int g_npokes = 0;
static void load_pokes(const char *spec) {
    const char *p = spec;
    while (*p && g_npokes < 16) {
        Poke *k = &g_pokes[g_npokes];
        char name[64];
        const char *colon = strchr(p, ':'), *eq = colon ? strchr(colon, '=') : NULL;
        size_t nl;
        u32 size = 0;
        if (!colon || !eq) break;
        k->nav = (*p == 'v');
        k->frame = strtol(k->nav ? p + 1 : p, NULL, 10);
        nl = (size_t)(eq - colon - 1);
        if (nl >= sizeof name) break;
        memcpy(name, colon + 1, nl); name[nl] = 0;
        k->ptr = (u8 *)field_ptr(name, &size);
        p = eq + 1;
        k->n = 0;
        k->done = 0;
        while (isxdigit((unsigned char)p[0]) && isxdigit((unsigned char)p[1]) && k->n < sizeof k->bytes) {
            char hx[3] = { p[0], p[1], 0 };
            k->bytes[k->n++] = (u8)strtoul(hx, NULL, 16);
            p += 2;
        }
        if (k->ptr && k->n && (size == 0 || k->n == size) &&
            (!k->nav || k->frame > 0)) {
            printf("[TRACE] PC_POKE: %s (%u bytes) at %s %ld (test fixture)\n", name,
                   (unsigned)k->n, k->nav ? "navigation VBlank" : "game frame", k->frame);
            g_npokes++;
        } else {
            fprintf(stderr, "[TRACE] PC_POKE: bad entry for %s (unknown field or wrong size)\n", name);
        }
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
    }
}
static void apply_pokes(void) {
    int i;
    for (i = 0; i < g_npokes; i++) {
        Poke *k = &g_pokes[i];
        if (!k->done && !k->nav && frame_counter >= k->frame) {
            memcpy(k->ptr, k->bytes, k->n);
            k->done = 1;
        }
    }
}
static void apply_nav_pokes(void) {
    int i;
    for (i = 0; i < g_npokes; i++) {
        Poke *k = &g_pokes[i];
        if (!k->done && k->nav && g_nav_vblanks >= k->frame) {
            memcpy(k->ptr, k->bytes, k->n);
            k->done = 1;
            printf("[TRACE] PC_POKE applied at navigation VBlank %ld\n", g_nav_vblanks);
        }
    }
}

static void write_vblank_trace(void) {
    if (g_trace) {
        size_t i;
        /* the mask the game saw this frame: pad_data, as PSX layout (the
         * matched pad chain stores active-high PSX bits in pad_data) */
        extern u64 hal_clock_ticks(void);
        /* tick= the system clock (PS1 master-clock ticks since the PC started),
         * as the regtest prints its global tick counter */
        fprintf(g_trace, "frame=%ld pad=%04x tick=%llu", g_host_frame, (unsigned)(pad_data & 0xFFFF),
                (unsigned long long)hal_clock_ticks());
        for (i = 0; i < NFIELDS; i++) {
            const u8 *b = (const u8 *)FIELDS[i].ptr;
            u32 k;
            fprintf(g_trace, " %s=", FIELDS[i].name);
            for (k = 0; k < FIELDS[i].size; k++) fprintf(g_trace, "%02x", b[k]);
        }
        for (i = 0; i < (size_t)g_nextra; i++) {
            u32 k;
            fprintf(g_trace, " %s=", g_extra[i].name);
            for (k = 0; k < g_extra[i].size; k++) fprintf(g_trace, "%02x", g_extra[i].ptr[k]);
        }
        fprintf(g_trace, " crashes=%d\n", g_crash_count);
        if ((g_host_frame & 63) == 0) fflush(g_trace);
    }
}

/* Callbacks, input polling, parity capture and budget completion retain
 * their order. Observation is passive unless PC_POKE explicitly requests
 * a state-mutating test fixture; that fixture precedes its selected sample. */
void trace_vblank_begin(void) {
    /* Anchored input resolves before this VBlank's pad poll samples it. */
    if (g_nseq) seq_step();
    if (g_pre_irq) {
        if (g_npokes) apply_pokes();
        write_vblank_trace();
    }
}

/* pc/hal/dbg_state.c after a state was loaded: the emulated clock is the
 * saved run's, so the VBlank count `clock vblank` input is keyed to
 * (g_host_frame, which this file keeps out of a restore) moves with it --
 * an absolute input recorded by PC_INPUT_DUMP then presses on the same
 * VBlanks after a load as in the run that recorded it. */
void trace_after_load(void) {
    if (g_clock_offset_valid) g_host_frame = (long)hal_clock_vblanks() + g_clock_offset;
}

void trace_vblank(void) {
    if (!g_pre_irq && g_npokes) apply_pokes();
    /* Capture/flush and budget exit still follow the completed interrupt. */
    parity_vblank();
    if (g_nav_vblanks >= 0) {
        g_nav_vblanks++;
        if (g_npokes) apply_nav_pokes();
    }
    if (!g_pre_irq) write_vblank_trace();
    if (g_mark_every > 0 && (long)frame_counter / g_mark_every != g_mark_last) {
        g_mark_last = (long)frame_counter / g_mark_every;
        printf("[MARK] game frame %ld vblank %ld step %02x/%02x/%02x\n", (long)frame_counter,
               g_host_frame, (unsigned)now_step, (unsigned)now_sub_step, (unsigned)now_sub_sub_step);
        fflush(stdout);
    }
    g_host_frame++;
    if (hal_clock_vblanks) {
        g_clock_offset = g_host_frame - (long)hal_clock_vblanks();
        g_clock_offset_valid = 1;
    }
    if (g_max_frames >= 0 && !g_budget_hold &&
        budget_now() - g_anchor >= g_max_frames) {
        if (g_trace) fclose(g_trace);
        g_trace = NULL;
        printf("[TRACE] PC_TRACE_FRAMES=%ld reached (clock=%s, frame_counter=%d), exiting\n",
               g_max_frames, g_vblank_budget ? "vblank" : "game", frame_counter);
        /* _exit, not exit: static destructors (the C++ SPU core) would run
         * while the SDL audio thread may still call into them -- a crash at
         * teardown that the scene matrix counted against the scene */
        reach_flush();
        if (pc_async_flush) pc_async_flush();   /* dumps still being written */
        if (card_writer_wait) card_writer_wait();
        fflush(NULL);
        _exit(0);
    }
}

/* once per main-loop iteration (kept for pc/src/main.c; the trace is per
 * VBlank now) */
void trace_frame(void) { }

void trace_shutdown(void) {
    if (g_trace) { fclose(g_trace); g_trace = NULL; }
}
