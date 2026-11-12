/* SPDX-License-Identifier: GPL-3.0-or-later */
/* cpu_cost.h -- the game-code CPU cost table (pc/hal/cpu_cost.c). */
#ifndef PC_CPU_COST_H
#define PC_CPU_COST_H

/* The optional-hook declarations below are weak: harnesses link HAL files
 * without cpu_cost.c and test the pointer.  cpu_cost.c defines
 * PC_CPU_COST_DEFINE before including this header so that its DEFINITIONS
 * stay strong: on Windows (PE/COFF) a weak definition becomes a weak
 * external that GNU ld may bind to another object's default of 0 -- the
 * first Windows build charged no BIOS/HAL time at all
 * (pc/tools/audit_pe_weak.py, docs/pc-port/WINDOWS-BUILD.md). */
#ifdef PC_CPU_COST_DEFINE
#define PC_CPU_COST_WEAK
#else
#define PC_CPU_COST_WEAK __attribute__((weak))
#endif

typedef struct {
    const char *name;   /* the function's name in the port (= retail's) */
    int step;           /* now_step it was measured in, -1: all scenes */
    unsigned ticks;     /* retail ticks from its entry to the next entry */
    const char *caller; /* entered from this function ("@ovl": overlay
                         * code); NULL: any caller */
} CpuCost;

extern const CpuCost cpu_cost_table[];      /* sorted by name */
extern const unsigned cpu_cost_count;
/* the boot's own rows, game frame 0 (cpu_cost_boot_table.c, BOOT_ROWS) */
extern const CpuCost cpu_cost_boot_table[]; /* sorted by name */
extern const unsigned cpu_cost_boot_count;
/* inside the VBlank exception's sequencer tick (cpu_cost_irq_table.c) */
extern const CpuCost cpu_cost_irq_table[];  /* sorted by name */
extern const unsigned cpu_cost_irq_count;
extern const unsigned cpu_cost_vblank_base;

/* arg0: the instrumented function's first argument register, as it
 * stood at its entry (cpu_cost.c ARG_ROWS) */
void hal_cpu_cost_enter(void *site, void *caller, unsigned long arg0);
void hal_cpu_cost_arm(void);
unsigned long long hal_cpu_cost_charged(void) PC_CPU_COST_WEAK;
unsigned hal_cpu_cost_net(unsigned ticks, unsigned long long mark) PC_CPU_COST_WEAK;
/* For HAL models measured as a span that includes instrumented calls:
 * the charge counter at the span's start, and the span less those calls'
 * charges (the span whole when cpu_cost.c is not linked). */
static __inline__ unsigned long long hal_cost_mark(void)
{
    return hal_cpu_cost_charged ? hal_cpu_cost_charged() : 0;
}
static __inline__ unsigned hal_cost_net(unsigned ticks, unsigned long long mark)
{
    return hal_cpu_cost_net ? hal_cpu_cost_net(ticks, mark) : ticks;
}
/* the caller's code after a BIOS call returns (cpu_cost.c CALL_CONTINUATION) */
extern void hal_cpu_cost_return(const char *row, void *ret) PC_CPU_COST_WEAK;
/* a HAL model's tail to its caller's next call: the boot's row for that
 * caller, else `dflt` (cpu_cost.c CALL_CONTINUATION) */
extern unsigned hal_cpu_cost_tail(const char *row, void *ret, unsigned dflt) PC_CPU_COST_WEAK;
void hal_cpu_cost_seq_begin(void);
void hal_cpu_cost_seq_end(void);
void hal_cpu_cost_seq_clear(void);
int hal_cpu_cost_vblank_ticks(unsigned *ticks);

/* Weak: harnesses link HAL files without cpu_cost.c. */
extern void hal_cpu_cost_named(const char *name, unsigned *cache) PC_CPU_COST_WEAK;

/* A HAL replacement of a pure-CPU SDK/BIOS function charges retail's time
 * for it (the table entry of that name). */
#define HAL_CPU_COST(name) do { \
        static unsigned cpu_cost_cache_; \
        if (hal_cpu_cost_named) hal_cpu_cost_named((name), &cpu_cost_cache_); \
    } while (0)

/* The same for the HAL's other pure-CPU SDK replacements (libgte's matrix
 * functions, ...), switched as a group: HAL_SDK_CPU=0 is their negative
 * control (cpu_cost.c hal_cpu_cost_sdk). */
extern void hal_cpu_cost_sdk(const char *name, unsigned *cache) PC_CPU_COST_WEAK;
#define HAL_SDK_COST(name) do { \
        static unsigned cpu_cost_cache_; \
        if (hal_cpu_cost_sdk) hal_cpu_cost_sdk((name), &cpu_cost_cache_); \
    } while (0)

/* Charged from the boot table only (cpu_cost.c BOOT_ROWS). */
extern void hal_cpu_cost_boot_named(const char *name) PC_CPU_COST_WEAK;
#define HAL_BOOT_COST(name) do { \
        if (hal_cpu_cost_boot_named) hal_cpu_cost_boot_named(name); \
    } while (0)

#endif
