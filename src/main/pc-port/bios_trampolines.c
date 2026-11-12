/* bios_trampolines.c — PsyQ BIOS function stubs for PC port.
 *
 * The game contains 16-byte thunks at 0x800B47XX–0x800B5A20 that dispatch to
 * PSX BIOS (A/B/C tables) via `jr` with syscall number in $t1. Native
 * adapters below implement memory, events and card contracts; unsupported
 * entries remain explicit stubs. Printf uses the bounded BIOS formatter in
 * pc/hal/bios_printf.c, rather than forwarding PS1 arguments to libc.
 *
 * 2026-10-08: the thunks carry their Sony symbol names in the PS1 tree now
 * (strncpy, memcpy, printf, rand, open, read, write, ...; T1 identity by BIOS
 * table:number against the Psy-Q archive members).  The definitions below keep
 * the address names on purpose: pc/include/game/sony_bios_alias.h maps each Sony
 * name back to its func_800Bxxxx trampoline for the game translation units, so PC
 * behaviour is unchanged by the rename (memcpy excepted, see that header).
 */

#include "types.h"
#include "pc_irq.h"
#include "bios_printf.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Memory-card save layer (pc/hal/memcard.c) exported via hal.h. Forward
 * declarations here so this TU stays C-only and doesn't drag the full
 * HAL header set into src/main/pc-port/. */
extern int  memcard_bu_init(void);
extern long memcard_card_load(long chan);
extern long memcard_card_info(long chan);
extern long memcard_card_status(long chan);
extern void memcard_card_init(void);
extern void memcard_card_start(void);
extern void memcard_vblank_tick(void);
extern long memcard_card_auto(long enabled);
extern void memcard_new_card(void);
extern long memcard_card_write(long chan, long sector, const u8 *buffer);
extern int memcard_file_delete(const char *filename);
extern int  memcard_file_open(const char *filename, u32 accessmode);
extern int  memcard_file_read(int fd, void *buf, int nbytes);
extern int  memcard_file_write(int fd, const void *buf, int nbytes);
extern int  memcard_file_close(int fd);
extern int  memcard_file_seek(int fd, int offset, int mode);

/* ---- A-table string / memory functions ----
 * The kernel's own byte loops (SCPH-5500 ROM: bzero 0xBFC01ACC, strncpy
 * 0xBFC03418, strcpy 0xBFC033C8, strcat 0xBFC03190, strncat 0xBFC03200,
 * bcopy 0xBFC01A90, memcpy 0xBFC02B50, memmove 0xBFC02BC8, memchr
 * 0xBFC02CC0), with their NULL checks and return values.  Until 2026-10-16
 * all but memcpy were `return 0` no-ops -- so system_reset's
 * bzero(&screen_w, 6796) cleared nothing and its strncpy of the default
 * player name / names (player_rec, p_sei, p_mei, p_adana) copied nothing.
 *
 * TIME: bzero's CD-booted SCPH-5500 path measures 317 + 132*n ticks
 * for positive lengths, with its first byte store near tick 283. Each store
 * is visible before the next interrupt, including writes to vblnk_counter
 * inside screen_w. Zero length takes 223 ticks. Other byte loops retain
 * their older instruction estimates; general game C still consumes no CPU
 * time. hal_cpu_spend excludes time spent inside interrupt callbacks from
 * the caller's CPU work budget. Cache and IRQ entry overhead remain unmodeled.
 *
 * A pointer the game passes as a PS1 address is folded to the mapped RAM
 * window (hal_ps1_ptr); a range that is not mapped on the PC is skipped
 * with one warning per function -- the "real call crashes on bad ptrs" the
 * old no-ops avoided.  On the console those writes land in RAM. */
extern void *hal_ps1_ptr(const void *p);
extern int pc_ptr_is_mapped(const void *p, unsigned long bytes);
extern void hal_cpu_spend(u32 ticks);
extern void hal_cpu_cost_return(const char *row, void *ret) __attribute__((weak));
extern u32 hal_cpu_ticks_to_vblank(void);
#define BIOS_TICKS_PER_INSN 34u
extern unsigned long long hal_clock_ticks(void);
static int bios_debug(void) {
    static int d = -1;
    if (d < 0) { const char *e = getenv("PC_DEBUG"); d = (e && strstr(e, "bios")) ? 1 : 0; }
    return d;
}
static void bios_spend_named(u32 insns, const char *fn) {
    if (bios_debug() && insns > 64)
        fprintf(stderr, "[BIOS t%llu] %s: %u insns (%.3f frames)\n", hal_clock_ticks(), fn, insns,
                insns * 34.0 / 566204.0);
    hal_cpu_spend(insns * 34u);
}
#define bios_spend(n) bios_spend_named((n), __func__)
static void *bios_ptr(const void *p, u32 n, const char *fn) {
    void *q;
    if (!p) return 0;
    q = hal_ps1_ptr(p);
    if (!pc_ptr_is_mapped(q, n ? n : 1)) {
        static const char *warned[16]; int i;
        for (i = 0; i < 16 && warned[i]; i++) if (warned[i] == fn) return 0;
        if (i < 16) warned[i] = fn;
        fprintf(stderr, "[BIOS] %s: %p (%u bytes) is not mapped on the PC; skipped\n", fn, p, (unsigned)n);
        return 0;
    }
    return q;
}
static u32 bios_strlen_mapped(const char *s) { u32 n = 0; while (s[n]) n++; return n; }

/* A(0x28) bzero(p, n) */
void *func_800B4820(void *p, s32 n) {
    u8 *d;
    if (!p) return 0;
    if (n <= 0) { hal_cpu_spend(223u); return 0; }
    if (bios_debug())
        fprintf(stderr, "[BIOS t%llu] bzero(%p, %d) from %p\n", hal_clock_ticks(), p, (int)n, __builtin_return_address(0));
    d = bios_ptr(p, (u32)n, "bzero");
    if (!d) return 0;
    /* CD-booted SCPH-5500 probe: 317 + 132*n ticks. The first SB
     * boundary is near tick 283. Retire bytes individually so callbacks
     * observe only stores reached by CPU work, not a pre-cleared chunk. */
    hal_cpu_spend(283u);
    while (n > 0) {
        *d++ = 0;
        hal_cpu_spend(132u);
        n--;
    }
    hal_cpu_spend(34u);
    if (hal_cpu_cost_return) hal_cpu_cost_return("bzero@ret", __builtin_return_address(0));
    return p;
}
/* strncpy / strcpy time, measured (regtest host, SCPH-5500; the
 * new_game probe trace: entry to the caller's next call, exceptions
 * excluded; source lengths read from the EXE's strings):
 *   strncpy  362 + 270.5 per byte copied (through the NUL, at most n)
 *            + 166.9 per padding NUL       (8 calls, n 2..256; residuals
 *            under 20 ticks, e.g. n=16 len=8: 3,956 measured, 3,965)
 *   strcpy   286 + 175.8 per byte copied with the NUL (len 17: 3,449..
 *            3,452; len 2: 804..946)
 * The old estimates (7 and 6 instructions a byte at 34 ticks, no padding,
 * no entry) were 13,020 -> 8,136 ticks short over system_init's four
 * strncpys.  BIOS_STRING_COST=0 restores them (negative control). */
static int bios_string_cost(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("BIOS_STRING_COST"); on = !(e && e[0] == '0'); }
    return on;
}
/* A(0x1A) strncpy(dst, src, n): copies through the NUL, then pads with NULs to n */
char *func_800B4830(char *dst, const char *src, s32 n) {
    char *d; const char *sp; s32 i = 0, copied;
    if (!dst || !src) return 0;
    d = bios_ptr(dst, n > 0 ? (u32)n : 1u, "strncpy"); sp = bios_ptr(src, 1, "strncpy");
    if (!d || !sp || n <= 0) return dst;
    while (i < n) { char c = sp[i]; d[i++] = c; if (!c) break; }
    copied = i;
    if (bios_string_cost())
        hal_cpu_spend(362u + (u32)((2705u * (u32)copied + 1669u * (u32)(n - copied) + 5u) / 10u));
    else
        bios_spend(7u * (u32)i);
    while (i < n) d[i++] = 0;
    return dst;
}
/* A(0x19) strcpy */
char *func_800B4890(char *dst, const char *src) {
    char *d; const char *sp; u32 len;
    if (!dst || !src) return 0;
    sp = bios_ptr(src, 1, "strcpy"); if (!sp) return dst;
    len = bios_strlen_mapped(sp);
    d = bios_ptr(dst, len + 1, "strcpy"); if (!d) return dst;
    memmove(d, sp, len + 1);
    if (bios_string_cost())
        hal_cpu_spend(286u + (1758u * (len + 1) + 5u) / 10u);
    else
        bios_spend(6u * (len + 1));
    return dst;
}
/* A(0x15) strcat */
char *func_800B48A0(char *dst, const char *src) {
    char *d; const char *sp; u32 dl, sl;
    if (!dst || !src) return 0;
    d = bios_ptr(dst, 1, "strcat"); sp = bios_ptr(src, 1, "strcat"); if (!d || !sp) return dst;
    dl = bios_strlen_mapped(d); sl = bios_strlen_mapped(sp);
    if (!bios_ptr(d + dl, sl + 1, "strcat")) return dst;
    memmove(d + dl, sp, sl + 1);
    bios_spend(4u * dl + 6u * (sl + 1));
    return dst;
}
/* A(0x16) strncat: appends at most n characters, then a NUL */
char *func_800B48B0(char *dst, const char *src, s32 n) {
    char *d; const char *sp; u32 dl; s32 i;
    if (!dst || !src) return 0;
    d = bios_ptr(dst, 1, "strncat"); sp = bios_ptr(src, 1, "strncat"); if (!d || !sp) return dst;
    dl = bios_strlen_mapped(d); d += dl;
    for (i = 0; ; i++) {
        char c = sp[i];
        d[i] = c;
        if (!c) break;
        if (i + 1 > n) { d[i] = 0; break; }     /* the counter ran out: the last byte becomes the NUL */
    }
    bios_spend(4u * dl + 8u * (u32)(i + 1));
    return dst;
}
/* A(0x27) bcopy(src, dst, n) */
void *func_800B5680(const void *src, void *dst, s32 n) {
    const u8 *sp; u8 *d;
    if (!src) return 0;
    if (n <= 0) return (void *)src;
    sp = bios_ptr(src, (u32)n, "bcopy"); d = bios_ptr(dst, (u32)n, "bcopy");
    if (sp && d) { memmove(d, sp, (u32)n); bios_spend(6u * (u32)n); }
    return (void *)src;
}
/* A(0x2E) memchr */
void *func_800B56B0(const void *p, s32 c, s32 n) {
    const u8 *q; s32 i;
    if (!p || n <= 0) return 0;
    q = bios_ptr(p, (u32)n, "memchr"); if (!q) return 0;
    for (i = 0; i < n; i++) if (q[i] == (u8)c) { bios_spend(5u * (u32)(i + 1)); return (u8 *)p + i; }
    bios_spend(5u * (u32)n);
    return 0;
}
/* A(0x2C) memmove */
void *func_800B56C0(void *dst, const void *src, s32 n) {
    u8 *d; const u8 *sp;
    if (!dst) return 0;
    if (n <= 0) return dst;
    d = bios_ptr(dst, (u32)n, "memmove"); sp = bios_ptr(src, (u32)n, "memmove");
    if (d && sp) { memmove(d, sp, (u32)n); bios_spend(6u * (u32)n); }
    return dst;
}

/* A-table */
s32 _bu_init(void) { return (s32)memcard_bu_init(); }               /* A(0x70) _bu_init */
s32 _card_auto(s32 enabled) { return (s32)memcard_card_auto(enabled); }
s32 _card_info(s32 chan) { return (s32)memcard_card_info(chan); }     /* A(0xAB) _card_info */
s32 _card_load(s32 chan) { return (s32)memcard_card_load(chan); }
/* A(0x2A) memcpy.  The game C calls `memcpy` both for this thunk and for the
 * IDO builtin (inlined lw/sw on PS1); since the 2026-10-08 rename both spell
 * the same name and sony_bios_alias.h routes both here, so this one must be a
 * real copy (it was a `return 0` no-op while only the thunk callers reached
 * it).  The kernel memcpy has libc memcpy's exact contract. */
/* Its time, measured to its return (regtest host, SCPH-5500, the
 * boot's two calls with the return addresses probed): 3,526 ticks for 16
 * bytes, 25,547 for 124 -- 264 + 203.9 a byte.  The old estimate (6
 * instructions a byte at 34 ticks) left out the 264.  Applied in the boot
 * (game frame 0) only: in game the port routes IDO's inlined structure
 * copies here too (above), which retail never calls, and those copies'
 * time is not measured.  BIOS_MEMCPY_COST=0 restores the old estimate
 * (negative control). */
extern s32 frame_counter;
static int bios_memcpy_cost(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("BIOS_MEMCPY_COST"); on = !(e && e[0] == '0'); }
    return on && frame_counter == 0;
}
void *func_800B4840(void *dst, const void *src, u32 n) {
    u8 *d = bios_ptr(dst, n, "memcpy"); const u8 *sp = bios_ptr(src, n, "memcpy");
    if (!dst) return 0;
    if (d && sp && (s32)n > 0) {
        __builtin_memcpy(d, sp, n);
        if (bios_memcpy_cost()) hal_cpu_spend(264u + (2039u * n + 5u) / 10u);
        else bios_spend(6u * n);
    }
    if (hal_cpu_cost_return) hal_cpu_cost_return("memcpy@ret", __builtin_return_address(0));
    return dst;
}
/* A(0x3F): explicit PS1 formatting/return semantics and profiled CPU work. */
s32 func_800B4850(const char *format, ...) {
    va_list args;
    s32 result;
    va_start(args, format);
    result = hal_bios_vprintf(format, args);
    va_end(args);
    return result;
}
__attribute__((naked)) void func_800B4860(void) { __asm__("jmp abs"); }  /* A(0x0E) abs */
/* A(0x2F) rand / A(0x30) srand: the PS1 kernel's generator, not the host's.
 * seed = seed * 0x41C64E6D + 0x3039; rand = (seed >> 16) & 0x7FFF (psx-spx).
 * The game seeds it from vblnk_counter (title_data_read, wipe_disp), so with
 * the VBlank event ticking (below) a scripted run draws the same numbers as
 * the console.  Until 2026-10-15 these jumped to glibc rand/srand. */
u32 g_psx_rand_seed = 0;   /* traced as rand_seed (pc/hal/trace.c) */
s32 func_800B4870(void) {
    g_psx_rand_seed = g_psx_rand_seed * 0x41C64E6Du + 0x3039u;
    return (s32)((g_psx_rand_seed >> 16) & 0x7FFF);
}
__attribute__((naked)) void func_800B4880(void) { __asm__("jmp strlen"); } /* A(0x1B) strlen */
void func_800B48C0(u32 seed) { g_psx_rand_seed = seed; }
__attribute__((naked)) void func_800B5650(void) { __asm__("jmp strcmp"); } /* A(0x17) */
__attribute__((naked)) void func_800B5670(void) { __asm__("jmp strncmp"); } /* A(0x18) */
s32 func_800B5690(void) { return 0; } /* A(0x13) SaveState */
s32 InitHeap(void) { return 0; } /* A(0x39) InitHeap */
s32 FlushCache(void) { return 0; } /* A(0x44) FlushCache */
s32 _96_init(void) { return 0; } /* A(0x71) _96_init */
s32 LoadExec(void) { return 0; } /* A(0x51) LoadExec */
s32 _96_remove(void) { return 0; } /* A(0x72) _96_remove */
s32 GPU_cw(void) { return 0; } /* A(0x49) GPU_dw */

/* The BIOS pad/card driver set-up calls take CPU time on the console
 * (regtest host, SCPH-5500, the no-input boot: entry to the next
 * main-binary entry, interrupt exceptions excluded): InitCARD 47,940,
 * InitPAD 47,767 (each about one FlushCache, ~47,700, plus its own work),
 * StartCARD 970, StartPAD 952, ChangeClearPAD 55.  They charge those
 * measured times by name (pc/hal/cpu_cost.c hal_cpu_cost_named, the boot
 * rows of pc/tools/cpu_costs.tsv).  BIOS_CPU_COST=0 is the negative
 * control (with the syscall exceptions below). */
extern void hal_cpu_cost_named(const char *name, unsigned *cache) __attribute__((weak));
static int bios_cpu_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("BIOS_CPU_COST");
        on = !(e && e[0] == '0');
    }
    return on;
}
#define BIOS_CPU_COST(name) do { \
        static unsigned cost_cache_; \
        if (hal_cpu_cost_named && bios_cpu_on()) hal_cpu_cost_named((name), &cost_cache_); \
    } while (0)

/* Whether the BIOS runs its card or pad driver at each VBlank (StartCARD /
 * StartPAD).  Before either, the VBlank exception is libetc's alone
 * (pc/hal/system.c VBLANK_EXCEPTION_BARE_TICKS). */
static int g_bios_vblank_drivers;
int hal_bios_vblank_drivers(void) { return g_bios_vblank_drivers; }

/* B-table */
s32 InitCARD(s32 mode) { (void)mode; BIOS_CPU_COST("InitCARD"); memcard_card_init(); return 0; }
static void syscall_cpu(unsigned ticks);
s32 StartCARD(void) {
    BIOS_CPU_COST("StartCARD");
    syscall_cpu(1247u);
    memcard_card_start();
    g_bios_vblank_drivers = 1;
    return 1;
}
s32 _card_status(s32 chan) { return (s32)memcard_card_status(chan); } /* B(0x5C) _card_status */
s32 _new_card(void) { memcard_new_card(); return 0; }
s32 _card_write(s32 chan, s32 sector, const u8 *buffer) {
    return (s32)memcard_card_write(chan, sector, buffer);
}
s32 func_800B5660(void) { return 0; } /* B(0x38) FileGetDeviceFlag */
s32 func_800B56A0(void) { return 0; } /* B(0x3F) firstfile */
s32 InitPAD(void) { BIOS_CPU_COST("InitPAD"); return 0; } /* B(0x12) InitPAD */
s32 StartPAD(void) { BIOS_CPU_COST("StartPAD"); syscall_cpu(1142u); g_bios_vblank_drivers = 1; return 0; } /* B(0x13) StartPAD */
s32 ChangeClearPAD(void) { BIOS_CPU_COST("ChangeClearPAD"); return 0; } /* B(0x5B) ChangeClearPAD */
/* B(0x08)/0x0C/0x0D/0x09/0x0B/0x0A: kernel events.  The game opens a VBlank
 * event (class 0xF2000003 = RCnt3, mode 0x1000 = callback) whose handler
 * is vblank_counter_tick (vblank_counter_event_init); on the console the
 * kernel runs it from the VBlank interrupt once per frame.  The table below
 * keeps the registrations and pc/src/main.c / VSync() tick the enabled
 * VBlank callbacks once per frame (hal_bios_vblank_tick). Card completions
 * deliver HwCARD/SwCARD events through hal_bios_deliver_event; polling-mode
 * events latch until TestEvent consumes them. */
typedef struct {
    u32 cls, spec, mode;
    void (*fn)(void);
    int enabled, used, pending;
} BiosEvent;
static BiosEvent g_bios_events[32];

/* OpenEvent 316 / EnableEvent 62 ticks of BIOS CPU (the boot's ten
 * calls, cost table rows; their critical sections are inside). */
s32 OpenEvent(u32 cls, u32 spec, u32 mode, void (*fn)(void)) {
    int i;
    BIOS_CPU_COST("OpenEvent");
    for (i = 0; i < 32; i++) {
        if (!g_bios_events[i].used) {
            g_bios_events[i].cls = cls; g_bios_events[i].spec = spec; g_bios_events[i].mode = mode;
            g_bios_events[i].fn = fn; g_bios_events[i].enabled = 0; g_bios_events[i].used = 1;
            g_bios_events[i].pending = 0;
            return (s32)(0xF1000000u | (u32)i);     /* an event descriptor, as the kernel returns */
        }
    }
    return -1;
}
static BiosEvent *bios_event(s32 ev) {
    u32 i = (u32)ev & 0xFFFF;
    return ((u32)ev >> 24) == 0xF1 && i < 32 && g_bios_events[i].used ? &g_bios_events[i] : 0;
}
s32 EnableEvent(s32 ev) {
    BiosEvent *e = bios_event(ev);
    BIOS_CPU_COST("EnableEvent");
    if (e) { e->enabled = 1; e->pending = 0; }
    return 1;
}
s32 DisableEvent(s32 ev) { BiosEvent *e = bios_event(ev); if (e) { e->enabled = 0; e->pending = 0; } return 1; }
s32 CloseEvent(s32 ev) { BiosEvent *e = bios_event(ev); if (e) e->used = 0; return 1; }
s32 TestEvent(s32 ev) {
    extern void hal_cpu_cost_named(const char *name, unsigned *cache) __attribute__((weak));
    static unsigned cost;   /* retail's measured B(0x0B) time, pc/hal/cpu_cost.c */
    BiosEvent *e;
    if (hal_cpu_cost_named) hal_cpu_cost_named("TestEvent", &cost);
    e = bios_event(ev);
    if (!e || !e->enabled || e->mode != 0x2000 || !e->pending) return 0;
    e->pending = 0;
    return 1;
}

void hal_bios_deliver_event(u32 cls, u32 spec) {
    int i;
    for (i = 0; i < 32; i++) {
        BiosEvent *e;
        e = &g_bios_events[i];
        if (!e->used || !e->enabled || e->cls != cls || e->spec != spec)
            continue;
        if (e->mode == 0x1000 && e->fn) e->fn();
        else if (e->mode == 0x2000) e->pending = 1;
    }
}

/* once per frame (the VBlank interrupt): run the enabled VBlank callbacks */
void hal_bios_vblank_tick(void) {
    int i;
    for (i = 0; i < 32; i++)
        if (g_bios_events[i].used && g_bios_events[i].enabled && g_bios_events[i].fn &&
            g_bios_events[i].cls == 0xF2000003u && (g_bios_events[i].mode & 0x1000))
            g_bios_events[i].fn();
    memcard_vblank_tick();
}
s32 func_800B5920(s32 fd) { return memcard_file_close(fd); }                                  /* B(0x36) FileClose */
s32 format(void) { return 0; }                                                          /* B(0x41) rename */
s32 func_800B5940(const char *name, u32 mode) { return memcard_file_open(name, mode); }        /* B(0x32) FileOpen */
s32 delete(const char *name) { return memcard_file_delete(name); }                      /* B(0x45) FileDelete */
s32 func_800B5960(s32 fd, void *buf, s32 n) { return memcard_file_read(fd, buf, n); }          /* B(0x34) FileRead */
s32 func_800B5970(s32 fd, const void *buf, s32 n) { return memcard_file_write(fd, buf, n); }   /* B(0x35) FileWrite */
s32 DeliverEvent(u32 cls, u32 spec) {
    hal_bios_deliver_event(cls, spec);
    return 0;
}
s32 WaitEvent(s32 ev) {
    BiosEvent *e;
    for (;;) {
        e = bios_event(ev);
        if (!e || !e->enabled || e->mode != 0x2000) return 0;
        if (e->pending) {
            e->pending = 0;
            return 1;
        }
        /* BIOS polling keeps hardware and VBlank events progressing. */
        hal_cpu_spend(128u);
    }
}
s32 HookEntryInt(void) { return 0; } /* B(0x19) SetCustomExitFromException */
s32 ReturnFromException(void) { return 0; } /* B(0x17) ReturnFromException */
s32 ResetEntryInt(void) { return 0; } /* B(0x18) SetDefaultExitFromException */

/* C-table */
s32 ChangeClearRCnt(void) { return 0; } /* C(0x0A) ChangeClearRCnt */

/* ------------------------------------------------------------------
 * Relocated 2026-08-23 from src/main/semi-matched/func_800B*.c.
 *
 * These eight entries sit in the same 16-byte trampoline table and were
 * previously carried as no-op "semi-matched" C stubs.  They are not
 * decompilable -- `jr $t2` into the kernel vector, a bare `syscall`, and
 * reads/writes of $sp and $gp have no C89 form -- so the PS1 side is now
 * byte-perfect ASM (asm_only, blocker category `bios_trampoline`;
 * generator tools/analysis/gen_bios_thunk_asm.py).  The PC side still
 * needs native definitions; the critical calls use the VBlank controller.
 *
 * DIVERGENCE: stack/GP and obsolete pad thunks retain their neutral native
 * values. Critical-section calls now gate the native VBlank IRQ latch;
 * other hardware IRQ routing and BIOS dispatch timing remain incomplete.
 * ------------------------------------------------------------------ */
void *SetSp(void *newSp) { (void)newSp; return (void *)0; } /* SetSp  */
s32   GetGp(void) { return 0; }                             /* GetGp  */
/* Enter/ExitCriticalSection are SYSCALLs: each is an exception through the
 * kernel, 688 / 514 ticks entry to return (medians of 53 each over the
 * new_game probe, regtest host SCPH-5500; an exit that delivers a
 * latched interrupt is that interrupt's exception, not counted here).
 * StartPAD and StartCARD make the same pair inside the BIOS (1,142 /
 * 1,247 measured, the cost table's rows exclude exception time).
 * BIOS_CPU_COST=0 is the negative control. */
#define SYSCALL_ENTER_CRITICAL_TICKS 688u
#define SYSCALL_EXIT_CRITICAL_TICKS  514u
extern void hal_cpu_charge(unsigned ticks) __attribute__((weak));
static void syscall_cpu(unsigned ticks) {
    if (bios_cpu_on() && hal_cpu_charge) hal_cpu_charge(ticks);
}
s32 EnterCriticalSection(void) {
    syscall_cpu(SYSCALL_ENTER_CRITICAL_TICKS);
    return hal_irq_enter_critical();
}
void ExitCriticalSection(void) {
    syscall_cpu(SYSCALL_EXIT_CRITICAL_TICKS);
    hal_irq_exit_critical();
}
u32   GetSp(void) { return 0; }                             /* GetSp  */
void  PAD_init(void) { } /* B(0x15) OutdatedPadInitAndStart */
void  PAD_dr(void) { } /* B(0x16) OutdatedPadGetButtons   */
void  StopPAD(void) { } /* B(0x14) StopPad                 */
