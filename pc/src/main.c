/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * main.c — PC port entry point
 * Runs the FULL game initialization chain, stubbing only crash-prone functions.
 * The game handles its own GPU setup, font loading, scene dispatch.
 */
#include "hal.h"
#include "pc_irq.h"
#include "bios_printf.h"
#include "pc_watchdog.h"
#include "../hal/profile.h"
#include "../hal/pc_fs.h"
#include "pc_data_root.h"
#include "pc_setup.h"
#include "pc_settings.h"
#include "pc_retail_init.h"
#include <time.h>
#include "../hal/lang.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <execinfo.h>
#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <dlfcn.h>
/* POSIX numbers for the [CRASH] sig= line (the harness tools key on them) */
#define PC_SIGBUS  7
#define PC_SIGALRM 14
#else
#include <sys/mman.h>
#include <unistd.h>      /* STDERR_FILENO (glibc 2.31 headers do not pull it in) */
#define _GNU_SOURCE 1
#include <ucontext.h>
#include <dlfcn.h>
#define PC_SIGBUS  SIGBUS
#define PC_SIGALRM SIGALRM
#endif

extern void hal_gpu_init(void);
extern void hal_cpu_abort_work(void);
extern void hal_irq_abort(void);

/* The game's main entry — full init + infinite scene loop */
extern void _main(void);

/* Game's own init functions */
extern void graph_init(void);   /* display/draw env setup */
extern void system_reset(s32);    /* game state init */

/* Game globals (outside blob range) */
extern u8 now_step;
extern u8 now_sub_step;
extern s32 frame_counter;
extern s32 step_frame_cnt;
extern u8 D_800FF869;

/* Scene dispatch */
extern void game_main(void);
extern void ot_buff_set(void);

/* Rendering pipeline - calls sprite renderer sprite_slot_disp */
extern void vblank_function(s32);

/* SEGV/BUS recovery using setjmp/longjmp + signal re-registration.
 * Note: pc/include/setjmp.h is a PsyQ stub; we use the system headers
 * directly for the signal-safe types. */
#undef SETJMP_H
#include_next <setjmp.h>
#include_next <signal.h>

#ifdef _WIN32
/* <setjmp.h> resolves to pc/include/setjmp.h, the Psy-Q header: its
 * jmp_buf is long[32] -- 256 bytes on Linux (glibc stores 200), but 128
 * with Windows' 32-bit long, while MSVCRT's _setjmp stores a 256-byte,
 * 16-byte-aligned _JUMP_BUFFER: every frame's setjmp overran g_recover.
 * The recovery buffer and the CRT entry points are declared here by their
 * symbols.  The frame argument is NULL: longjmp then restores the
 * registers without unwinding the game frames (the crash recovery jumps
 * out of a redirected fault, docs/pc-port/WINDOWS-BUILD.md "Faults"). */
typedef unsigned long long pc_win_jmp_buf[32] __attribute__((aligned(16)));
#define jmp_buf pc_win_jmp_buf
extern int pc_crt_setjmp(void *buf, void *frame) __asm__("_setjmp")
    __attribute__((returns_twice, nothrow));
extern void pc_crt_longjmp(void *buf, int value) __asm__("longjmp")
    __attribute__((noreturn));
#undef setjmp
#undef longjmp
#define setjmp(buf) pc_crt_setjmp((buf), NULL)
#define longjmp(buf, value) pc_crt_longjmp((buf), (value))
#endif

static jmp_buf g_recover;
static volatile int g_recover_active = 0;
volatile int g_crash_count = 0;
static int g_crash_run;   /* frames in a row that ended in a crash */
volatile void *g_last_fault_addr = NULL;
volatile void *g_last_fault_pc   = NULL;
static char g_last_fault_sym[256] = {0};

/* Fault-fold diagnostics.  Every recovery by the sign-extension fold below
 * is counted; the first recovery at each faulting instruction prints one
 * `[FOLD] site` line (function+offset, the folded value, the scene), and
 * pc_fold_report() prints `[FOLD] total=N sites=M` at exit (atexit, and the
 * trace harness's _exit path in pc/hal/trace.c).  scene_matrix.py and
 * playthrough.py read these lines.  PC_FOLD=off disables the fold: a
 * sign-extended PS1 address then faults into the normal crash recovery,
 * which is how a run proves it does not depend on the fold. */
#define FOLD_SITES 256
static volatile long g_fold_count;
static void *g_fold_pc[FOLD_SITES];
static long g_fold_hits[FOLD_SITES];
static int g_fold_sites;
static int g_fold_off = -1;

static const char *fold_sym(void *pc, char *buf, size_t n) {
    Dl_info di;
    if (pc && dladdr(pc, &di) && di.dli_sname)
        snprintf(buf, n, "%s+0x%lx", di.dli_sname,
                 (long)((char *)pc - (char *)di.dli_saddr));
    else
        snprintf(buf, n, "pc=%p", pc);
    return buf;
}

void pc_fold_report(void) {
    static int done;
    int i;
    char sym[256];
    if (done) return;
    done = 1;
    fflush(stdout);   /* stdout and stderr share the harness's pipe */
    fprintf(stderr, "[FOLD] total=%ld sites=%d\n", (long)g_fold_count, g_fold_sites);
    for (i = 0; i < g_fold_sites; i++)
        fprintf(stderr, "[FOLD]   %8ld %s\n", g_fold_hits[i],
                fold_sym(g_fold_pc[i], sym, sizeof sym));
    fflush(stderr);
}

static void fold_note(void *pc, unsigned long long value) {
    int i;
    char sym[256];
    g_fold_count++;
    for (i = 0; i < g_fold_sites; i++)
        if (g_fold_pc[i] == pc) { g_fold_hits[i]++; return; }
    if (g_fold_sites < FOLD_SITES) {
        g_fold_pc[g_fold_sites] = pc;
        g_fold_hits[g_fold_sites++] = 1;
    }
    fflush(stdout);   /* never split a buffered stdout line in the pipe */
    fprintf(stderr, "[FOLD] site %s value=%016llx scene=%d sub=%d\n",
            fold_sym(pc, sym, sizeof sym), value,
            (int)(*(volatile u8 *)&now_step), (int)(*(volatile u8 *)&now_sub_step));
    fflush(stderr);
}

/* PS1 zero-page semantics (see the fault handlers below): decode the
 * instruction at PC as one of the load/store forms the matched code uses
 * on a low address.  Returns its length (0 when it is not one of them, or
 * not decoded with certainty) and the register a load writes: *gpr
 * (0=RAX .. 15=R15) or *xmm (an XMM register), both -1 for a store.
 * Conservative subset: 32/64-bit mov, movzx/movsx, movslq loads; 32/64-bit
 * mov stores; movdqa/movdqu/movups to and from XMM. */
static long zp_decode(const unsigned char *pc, int *gpr, int *xmm)
{
    const unsigned char *ip = pc;
    int rex = 0, op0, modrm, mod, store = -1, dreg = -1;
    long plen = -1;
    *gpr = -1;
    *xmm = -1;
    /* prefixes: legacy + REX (plain gcc output) */
    for (;;) {
        unsigned char c = *ip;
        if (c == 0x66 || c == 0xF3 || c == 0xF2 || c == 0xF0 || c == 0x67 ||
            c == 0x26 || c == 0x2E || c == 0x36 || c == 0x3E ||
            c == 0x64 || c == 0x65) ip++;
        else break;
    }
    if (((*ip) & 0xF0) == 0x40) { rex = *ip; ip++; }
    op0 = *ip++;
    if (op0 == 0x0F) { op0 = 0x0F00 | *ip++; }
    switch (op0) {
    case 0x63:                     /* movslq r64, r/m32   */ dreg = 1; break;
    case 0x8B:                     /* mov   r32/64, r/m   */ dreg = 1; break;
    case 0x0FB6: case 0x0FBE:      /* movzbl / movsbl     */ dreg = 1; break;
    case 0x0FB7: case 0x0FBF:      /* movzwl / movswl     */ dreg = 1; break;
    case 0x89:                     /* mov   r/m, r32/64   */ store = 1; break;
    case 0xC7:                     /* mov   r/m, imm32    */ store = 1; break;
    case 0x0F6F:                   /* movdqa/movdqu xmm,m */ dreg = 16; break;
    case 0x0F10:                   /* movups xmm, m       */ dreg = 16; break;
    case 0x0F7F:                   /* movdqa/movdqu m,xmm */ store = 1; break;
    case 0x0F11:                   /* movups m, xmm       */ store = 1; break;
    default: break;
    }
    if (dreg < 1 && store != 1) return 0;
    modrm = *ip++;
    mod = (modrm >> 6) & 3;
    if (mod == 3) return 0;
    {
        int rm = modrm & 7;
        int sib_base = -1;
        if (rm == 4) { sib_base = (*ip) & 7; ip++; }   /* SIB byte */
        if (mod == 1) { ip += 1; }          /* disp8 */
        else if (mod == 2) { ip += 4; }     /* disp32 */
        else if (mod == 0 && (rm == 5 || sib_base == 5)) { ip += 4; }
            /* rm==5: RIP-relative; sib base==5: disp32, no base */
        if (store == 1 && op0 == 0xC7) { ip += 4; } /* imm32 */
        plen = (long)(ip - pc);
    }
    if (plen <= 0 || plen > 18) return 0;
    if (dreg >= 16)
        *xmm = ((modrm >> 3) & 7) | ((rex & 4) ? 8 : 0);
    else if (dreg == 1)
        *gpr = ((modrm >> 3) & 7) | ((rex & 4) ? 8 : 0);   /* REX.R */
    return plen;
}

/* The [CRASH] report of an unrecovered fault (both platforms): the fault
 * site and its caller by name, the scene, the optional slot dump and a
 * backtrace.  Returns 1 when the main loop's recovery point is armed (the
 * caller then longjmps there), after counting the recovery. */
static int crash_report(int sig, void *pc, void *ra, void *fault_addr)
{
    g_last_fault_pc = pc;
    g_last_fault_addr = fault_addr;
    /* Try to resolve the return address: the caller of a NULL or wild
     * function pointer (RSP[0] right after the `call`), or of a libc leaf
     * (memcpy, strlen) whose own symbol dladdr cannot name.  The explorer
     * (pc/tools/explorer.py) keys such crashes on this line. */
    {
        Dl_info dp;
        int named = pc && dladdr(pc, &dp) && dp.dli_sname;
        if (named && (dp.dli_sname[0] == '_' && dp.dli_sname[1] == '_'))
            named = 0;            /* a libc internal (__memmove_avx_...) */
        if (named || !ra)
            ra = NULL;
    }
    if (ra) {
        Dl_info dr;
        if (dladdr(ra, &dr) && dr.dli_sname) {
            long off = (long)((char *)ra - (char *)dr.dli_saddr);
            fprintf(stderr, "[CRASH] caller via RA: %s+0x%lx (%s)\n",
                    dr.dli_sname, off, pc ? "unnamed or wild pc" : "called NULL fn ptr");
        } else {
            fprintf(stderr, "[CRASH] caller via RA: %p (unresolved)\n", ra);
        }
        fflush(stderr);
    }

    /* Try to resolve the symbol (non-async-signal-safe, but we're crashing). */
    Dl_info di;
    g_last_fault_sym[0] = '\0';
    if (pc && dladdr(pc, &di) && di.dli_sname) {
        long off = (long)((char *)pc - (char *)di.dli_saddr);
        snprintf(g_last_fault_sym, sizeof(g_last_fault_sym),
                 "%s+0x%lx", di.dli_sname, off);
    } else if (pc) {
        snprintf(g_last_fault_sym, sizeof(g_last_fault_sym),
                 "pc=%p", pc);
    }

    /* A scene that faults every frame would write this report (and the
     * backtrace below) 60 times a second: the first three in full, then one
     * line every 600 (a player's log reached 353 MB in a minute,
     * 2026-11-07). */
    if (g_crash_count >= 3 && g_crash_count % 600) {
        if (g_recover_active) {
            g_recover_active = 0;
            g_crash_count++;
            return 1;
        }
        return 0;
    }

    /* Log crash details (stderr is line-buffered; should flush before jump) */
    fprintf(stderr, "[CRASH] sig=%d pc=%p sym=%s fault_addr=%p scene=%d sub=%d\n",
            sig, pc,
            g_last_fault_sym[0] ? g_last_fault_sym : "(unresolved)",
            g_last_fault_addr,
            (int)(*(volatile u8 *)&now_step),
            (int)(*(volatile u8 *)&now_sub_step));

    /* PC_DEBUG=slot: one-shot sprite-slot scan on the first crash -- which
     * class_w slots are active and what their hdr pointer holds (the
     * sprite_slot_disp NULL-deref triage for the unshadowing climb). */
    {
        static int dumped = 0;
        const char *dbg = getenv("PC_DEBUG");
        extern u8 class_w[];
        extern s32 class_max;
        int si, shown = 0;
        if (!dumped && dbg && strstr(dbg, "slot")) {
            fprintf(stderr, "[SLOT] fault=%s @800e2ec0=%016lx %016lx\n",
                    g_last_fault_sym,
                    *(unsigned long *)0x800E2EC0ul,
                    *(unsigned long *)(0x800E2EC0ul + 8));
            dumped = 1;
            fprintf(stderr, "[SLOT] count=%d\n", (int)class_max);
            for (si = 0; si < 160 && shown < 24; si++) {
                u8 *s = class_w + si * 0x58;
                unsigned long hdr = *(unsigned long *)(s + 0x40);
                if ((s[3] & 0x80) || hdr) {
                    fprintf(stderr, "[SLOT] #%d f2=%02x f3=%02x hdr=%016lx "
                            "cels=%016lx anims=%016lx frame=%d\n",
                            si, s[2], s[3], hdr,
                            *(unsigned long *)(s + 0x10),
                            *(unsigned long *)(s + 0x30),
                            (int)*(s16 *)(s + 0x24));
                    shown++;
                }
            }
        }
    }

    /* Backtrace (may be limited under signal, but useful) */
    {
        static volatile sig_atomic_t tracing;
        void *bt[16];
        int n;
        /* Write symbols directly: no per-recovery symbol-string allocation
         * survives the longjmp. The first-fault sym= line remains above. */
        /* A damaged frame can fault inside the unwinder itself. Never
         * recursively unwind that diagnostic fault. A longjmp leaves this
         * flag set, disabling only further backtraces for this process. */
        if (!tracing) {
            tracing = 1;
            n = backtrace(bt, 16);
            backtrace_symbols_fd(bt, n, STDERR_FILENO);
            tracing = 0;
        }
    }
    fflush(stderr);

    if (g_recover_active) {
        g_recover_active = 0;
        g_crash_count++;
        return 1;
    }
    return 0;
}

/* Abandon the interrupted work and return to the armed recovery point. */
static void crash_recover(int sig)
{
    hal_cpu_abort_work();
    hal_irq_abort();
    hal_sdk_irq_abort();
    longjmp(g_recover, sig);
}

#ifdef _WIN32
/* ---- Windows: the same fault model through a vectored exception handler ----
 * (docs/pc-port/WINDOWS-BUILD.md "Faults").  Linux handles SIGSEGV/SIGBUS/
 * SIGFPE in crash_sigaction below; here an access violation (and the
 * misalignment / integer-divide exceptions) of the game's own code goes
 * through the same steps in the same order: the overlay call redirect, the
 * zero-page emulation, the sign-extension fold, then crash_report and the
 * recovery.  The recovery cannot longjmp out of the exception dispatcher,
 * so the handler points the faulting thread at win_recover_entry (a call
 * frame below the faulting one) and continues: the thread then longjmps to
 * the armed recovery point exactly as the signal handler does (setjmp is
 * the non-unwinding CRT form, see g_recover).
 *
 * Which faults: those of this executable's code, of code outside every
 * module (a wild or PS1-window RIP), and of the C runtime the game calls
 * (msvcrt memcpy/memset/strlen on a game pointer) -- the cases the Linux
 * handler sees.  A fault inside another module (ntdll, SDL, a GL driver)
 * may be one that module handles itself, and is left to it. */
#ifndef STDERR_FILENO
#define STDERR_FILENO 2
#endif
static DWORD g_game_thread_id;
static volatile int g_win_recover_sig;

static void win_recover_entry(void) { crash_recover(g_win_recover_sig); }

/* Make the thread whose context is C call FN when it resumes. */
static void win_redirect(CONTEXT *c, void (*fn)(void))
{
    uintptr_t sp = ((uintptr_t)c->Rsp - 128) & ~(uintptr_t)15;
    sp -= 8;                                 /* rsp % 16 == 8 at a function entry */
    *(uintptr_t *)sp = 0;                    /* no return: fn longjmps or exits */
    c->Rsp = sp;
    c->Rip = (uintptr_t)fn;
}

static int win_fault_is_ours(DWORD64 rip)
{
    HMODULE m = NULL;
    wchar_t name[MAX_PATH];
    const wchar_t *base;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)(uintptr_t)rip, &m))
        return 1;                            /* no module: wild / PS1-window code */
    if (m == GetModuleHandleW(NULL)) return 1;
    if (!GetModuleFileNameW(m, name, MAX_PATH)) return 0;
    base = wcsrchr(name, L'\\');
    base = base ? base + 1 : name;
    return !_wcsicmp(base, L"msvcrt.dll") || !_wcsicmp(base, L"ucrtbase.dll");
}

static LONG CALLBACK win_fault_handler(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *c = ep->ContextRecord;
    DWORD64 *const gpr[16] = {
        &c->Rax, &c->Rcx, &c->Rdx, &c->Rbx, &c->Rsp, &c->Rbp, &c->Rsi, &c->Rdi,
        &c->R8, &c->R9, &c->R10, &c->R11, &c->R12, &c->R13, &c->R14, &c->R15
    };
    void *pc = (void *)(uintptr_t)c->Rip, *ra = NULL, *fault_addr = NULL;
    int sig, have_addr = 0;

    switch (er->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
        sig = SIGSEGV;
        if (er->NumberParameters >= 2) {
            fault_addr = (void *)(uintptr_t)er->ExceptionInformation[1];
            have_addr = 1;
        }
        break;
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        sig = PC_SIGBUS;
        break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
        sig = SIGFPE;
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (!win_fault_is_ours(c->Rip)) return EXCEPTION_CONTINUE_SEARCH;

    /* a call through a PS1 code address (pc/hal/overlay.c) */
    if (sig == SIGSEGV) {
        extern int hal_overlay_redirect_pc(unsigned long long *rip);
        if (hal_overlay_redirect_pc((unsigned long long *)&c->Rip))
            return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (c->Rsp) ra = *(void **)(uintptr_t)c->Rsp;

    /* PS1 zero-page semantics: as crash_sigaction */
    if (have_addr && (uintptr_t)fault_addr < 0x10000u) {
        static int zp_emu_debug = -1;
        int g, x;
        long plen;
        if (zp_emu_debug < 0) {
            const char *e = getenv("PC_DEBUG");
            zp_emu_debug = (e && strstr(e, "zp")) ? 1 : 0;
        }
        plen = zp_decode((const unsigned char *)pc, &g, &x);
        if (plen > 0) {
            if (x >= 0) memset(&(&c->Xmm0)[x], 0, sizeof c->Xmm0);
            else if (g >= 0) *gpr[g] = 0;
            c->Rip += (DWORD64)plen;
            if (zp_emu_debug)
                fprintf(stderr, "[ZP] %s at %p (addr=%p): emulated, skip %ld bytes\n",
                        (g >= 0 || x >= 0) ? "zero-load" : "noop-store", pc, fault_addr, plen);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    /* the sign-extension fold: as crash_sigaction (the 16 GPRs and RIP) */
    if (g_fold_off < 0) {
        const char *e = getenv("PC_FOLD");
        g_fold_off = (e && strcmp(e, "off") == 0) ? 1 : 0;
    }
    if ((sig == SIGSEGV || sig == PC_SIGBUS) && !g_fold_off) {
        int gi, folded = 0;
        unsigned long long first = 0;
        for (gi = 0; gi < 17; gi++) {
            DWORD64 *r = gi < 16 ? gpr[gi] : &c->Rip;
            unsigned long long v = *r;
            if ((v >> 32) == 0xFFFFFFFFull &&
                (u32)v >= 0x80000000u && (u32)v < 0x80800000u) {
                *r = (DWORD64)(u32)v;
                if (!folded) first = v;
                folded = 1;
            }
        }
        if (folded) {
            fold_note(pc, first);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    if (crash_report(sig, pc, ra, fault_addr)) {
        g_win_recover_sig = sig;
        win_redirect(c, win_recover_entry);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    fflush(NULL);
    return EXCEPTION_CONTINUE_SEARCH;        /* not recoverable: the OS ends it */
}

/* The watchdog (pc/hal/watchdog.c) found the game thread stalled: it has
 * suspended it and hands its context here.  As SIGALRM on Linux, the report
 * and the recovery run on the game thread (win_alarm_entry). */
static void *g_alarm_pc, *g_alarm_ra;

static void win_alarm_entry(void)
{
    if (crash_report(PC_SIGALRM, g_alarm_pc, g_alarm_ra, NULL))
        crash_recover(PC_SIGALRM);
    fflush(NULL);
    /* Linux dies of the signal; 128 + SIGALRM is the shell's spelling */
    TerminateProcess(GetCurrentProcess(), 128 + PC_SIGALRM);
}

void pc_win_alarm_redirect(void *context)
{
    CONTEXT *c = context;
    g_alarm_pc = (void *)(uintptr_t)c->Rip;
    g_alarm_ra = c->Rsp ? *(void **)(uintptr_t)c->Rsp : NULL;
    win_redirect(c, win_alarm_entry);
}

/* the game thread: pc/hal/watchdog.c suspends it */
unsigned long pc_win_game_thread_id(void) { return g_game_thread_id; }

static void install_crash_handler(void) {
    static int reported;
    if (!reported) {
        reported = 1;
        atexit(pc_fold_report);
        AddVectoredExceptionHandler(1, win_fault_handler);
    }
}
#else
/* siginfo/ucontext-aware handler: records PC + fault address, resolves
 * the nearest symbol via dladdr() so we can identify the faulting function. */
static void crash_sigaction(int sig, siginfo_t *info, void *ucv) {
    ucontext_t *uc = (ucontext_t *)ucv;
    /* a call through a PS1 overlay code address: run the loaded overlay's
     * native function instead (pc/hal/overlay.c) -- not a crash */
    if (sig == SIGSEGV) {
        extern int hal_overlay_fault_redirect(void *ucontext);
        if (hal_overlay_fault_redirect(ucv)) return;
    }
    void *pc = NULL;
    void *ra = NULL;
    void *rbp_caller = NULL;
#ifdef REG_RIP
    pc = (void *)uc->uc_mcontext.gregs[REG_RIP];
    {
        /* When pc=0 (NULL function pointer call), RSP[0] is the return
         * address of whoever did `call <NULL>`. Capture it for triage. */
        void **rsp = (void **)uc->uc_mcontext.gregs[REG_RSP];
        if (rsp) ra = rsp[0];
        rbp_caller = (void *)uc->uc_mcontext.gregs[REG_RBP];
    }
#elif defined(REG_EIP)
    pc = (void *)uc->uc_mcontext.gregs[REG_EIP];
#endif
    /* PS1 zero-page semantics: on the console the low 64KB is RAM, so
     * pointer guards read zero through NULL and return gracefully
     * (`if (map->ncellw == 0) return;` over a NULL map; GsGetLws(NULL)
     * walking a zero chain).  Address 0 cannot be mapped on the host
     * (vm.mmap_min_addr = 65536), so the load/store forms the matched code
     * uses are EMULATED here: a load from below 0x10000 yields zero, a
     * store there is skipped, and the instruction pointer advances past
     * the instruction.  Conservative subset (32/64-bit mov, movzx/movsx,
     * movslq loads; 32/64-bit mov stores); anything not decoded with
     * certainty falls through to the recovery below exactly as before.
     * Known divergence: a store followed by a read-back of the same low
     * address sees stale zeros (the console would see its own store).
     * Memory faults only: a timer's SIGALRM carries si_addr 0 too, and
     * the watchdog's alarm landing on a `mov` was "emulated" -- the
     * instruction skipped, the stall never reported (found writing the
     * Windows handler, which takes the address from the exception record;
     * pc/tools/test_fault_paths.py stall). */
    if ((sig == SIGSEGV || sig == SIGBUS) && info &&
        (unsigned long)info->si_addr < 0x10000ul) {
        static int zp_emu_debug = -1;
        int gpr, xmm;
        long plen;
        if (zp_emu_debug < 0) {
            const char *e = getenv("PC_DEBUG");
            zp_emu_debug = (e && strstr(e, "zp")) ? 1 : 0;
        }
        plen = zp_decode((unsigned char *)pc, &gpr, &xmm);
        if (plen > 0) {
            /* register numbering: 0=RAX .. 7=RDI, 8=R8 .. 15=R15 */
            static const int map16[16] = {
                REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP,
                REG_RSI, REG_RDI, REG_R8, REG_R9, REG_R10, REG_R11,
                REG_R12, REG_R13, REG_R14, REG_R15
            };
            if (xmm >= 0) {
                /* SSE zero-load: the modrm.reg XMM register reads zero */
                struct _libc_fpstate *fp = uc->uc_mcontext.fpregs;
                if (fp) memset(&fp->_xmm[xmm], 0, sizeof fp->_xmm[0]);
            } else if (gpr >= 0) {
                uc->uc_mcontext.gregs[map16[gpr]] = 0;
            }
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)(unsigned long)pc + plen;
            if (zp_emu_debug) {
                fprintf(stderr, "[ZP] %s at %p (addr=%p): emulated, skip %ld bytes\n",
                        (gpr >= 0 || xmm >= 0) ? "zero-load" : "noop-store", pc, info->si_addr, plen);
            }
            return;
        }
    }

    /* The s32-pointer idiom: matched code materialises PS1 addresses in 32
     * bits and the host SIGN-EXTENDS them (0x8012xxxx -> 0xFFFFFFFF8012xxxx,
     * kernel space).  hal_ps1_ptr folds that shape at the HAL boundary; here
     * the fold happens at the FAULT, for every register that carries the
     * shape (including RIP for the sign-extended fn-pointer jumps), and the
     * faulting instruction retries once.  Only the sign-extended PS1 range
     * folds -- NULL and wild pointers keep faulting into the recovery below
     * exactly as before.  Documented host divergence: the console's `lw`
     * produces the zero-extended address.
     *
     * Since 2026-10-31 the build zero-extends every signed-32 -> pointer
     * conversion of the game C (pc/tools/adapt_ps1_addresses.py, gated by
     * audit_ps1_pointer_casts.py SEXT-PTR), and the playthroughs and the
     * scene matrix recover here 0 times (from ~48,000).  The fold stays as
     * a counted safety net, not a mechanism: it still covers what the
     * audit cannot see (a value sign-extended through a 64-bit integer
     * type, a HAL path, code no route reaches yet), every recovery prints
     * a [FOLD] site, and scene_matrix.py / playthrough.py fail on one. */
    if (g_fold_off < 0) {
        const char *e = getenv("PC_FOLD");
        g_fold_off = (e && strcmp(e, "off") == 0) ? 1 : 0;
    }
    if ((sig == SIGSEGV || sig == SIGBUS) && !g_fold_off) {
        int gi, folded = 0;
        unsigned long first = 0;
        /* Each successful retry consumes the sign-extended register value.
         * The same instruction/address can legitimately recur next frame;
         * caching that pair permanently disables adaptation after one use.
         * If the retry still faults, no register folds a second time and
         * normal crash recovery below handles the unresolved access. */
        for (gi = 0; gi < NGREG; gi++) {
            unsigned long v = (unsigned long)uc->uc_mcontext.gregs[gi];
            if ((v >> 32) == 0xFFFFFFFFul &&
                (u32)v >= 0x80000000u && (u32)v < 0x80800000u) {
                uc->uc_mcontext.gregs[gi] = (greg_t)(u32)v;
                if (!folded) first = v;
                folded = 1;
            }
        }
        if (folded) {
            fold_note(pc, first);
            return;   /* retry the instruction with folded pointers */
        }
    }

    if (crash_report(sig, pc, ra, info ? info->si_addr : NULL))
        crash_recover(sig);
    /* Not recoverable — reset handler and re-raise for core dump */
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_crash_handler(void) {
    static int reported;
    struct sigaction sa;
    if (!reported) {
        reported = 1;
        atexit(pc_fold_report);
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_sigaction;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESTART;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGFPE,  &sa, NULL);
    /* Record the interrupted PC for watchdog failures as well. */
    sigaction(SIGALRM, &sa, NULL);
}

#endif /* _WIN32 */

#ifdef _WIN32
/* ---- Windows: the same layout without a stack switch ----
 * (docs/pc-port/WINDOWS-BUILD.md "Address space").  The exe is linked at a
 * fixed image base of 0x400000 with no ASLR and a 64 MB main-thread stack
 * (pc/Makefile); Windows then places that stack, the CRT heap and every
 * VirtualAlloc bottom-up, far below 2 GB, so the game runs on the main
 * thread.  main() still checks the stack (pc/hal/win/win_heap.c checks every
 * malloc), maps the PS1 ranges first, and switches stdio to binary: a
 * text-mode stream would turn "\n" into "\r\n" in the logs and traces the
 * harness compares, and _fmode makes every later fopen binary too (a
 * forgotten "b" would otherwise corrupt binary assets on read). */
static int real_main(int argc, char *argv[]);

int main(int argc, char *argv[]) {
    extern int pc_map_fixed_regions(void);
    volatile char here;
    _fmode = _O_BINARY;
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    if (pc_map_fixed_regions() != 0) return 1;
    if ((uintptr_t)&here >= 0x80000000u) {
        fprintf(stderr, "[INIT] FATAL: the main-thread stack is at %p, above 2 GB "
                "(an ASLR / high-entropy image?): game code keeps stack addresses "
                "in 32-bit integers\n", (void *)&here);
        return 1;
    }
    g_game_thread_id = GetCurrentThreadId();
    printf("[INIT] PS1 RAM and MMIO mapped; main-thread stack at %p\n", (void *)&here);
    return real_main(argc, argv);
}
#else
/* ---- the game runs with its stack and heap below 2 GB ----
 * The game's C keeps addresses in 32-bit integers all over: `(s32)pages`
 * passed for a `s32 *` parameter (NAME_ENT's intro hands its local page
 * table to bg_set that way), s32 globals holding buffer addresses, u32
 * struct fields.  That is exact on the PS1 and truncates a 64-bit pointer.
 * The binary is linked -no-pie, so code, data and bss already sit below
 * 2 GB; the stack (0x7ffc...) and mmap'd malloc blocks did not.  main()
 * therefore moves the game onto a stack mapped with MAP_32BIT and keeps
 * malloc on the brk heap, so a pointer survives the round trip through an
 * s32 (sign extension included: everything is under 0x80000000).  The PS1
 * RAM windows (0x80010000.., 0x80132000..) are above it on purpose -- they
 * are PS1 addresses, which the HAL folds (hal_ps1_ptr). */
/* glibc mallopt (pc/include/malloc.h shadows <malloc.h> with the Psy-Q one) */
extern int mallopt(int param, int value);
#define GLIBC_M_TOP_PAD        (-2)
#define GLIBC_M_MMAP_THRESHOLD (-3)
#define GLIBC_M_MMAP_MAX       (-4)
#define GAME_STACK_SIZE (64u << 20)
static int g_main_argc;
static char **g_main_argv;
static int g_main_ret;
static int real_main(int argc, char *argv[]);
static void game_stack_entry(void) { g_main_ret = real_main(g_main_argc, g_main_argv); }

int main(int argc, char *argv[]) {
    static ucontext_t main_ctx, game_ctx;
    void *stk;
    {
        /* The fixed PS1 ranges (RAM 0x80000000, the register page
         * 0x1F801000) are mapped FIRST, MAP_FIXED_NOREPLACE, before the brk
         * heap grows: mapped later with MAP_FIXED, the register page could
         * replace a page of a heap that had grown over it (pc/src/ps1_regions.c).
         * A randomised brk start too close below a range would cap the
         * heap: re-execute once without randomisation (the heap then starts
         * after .bss, ~480 MB below the register page). */
        extern int pc_map_fixed_regions(void);
        extern unsigned long pc_heap_room(void);
        extern const unsigned long PC_HEAP_ROOM_MIN;
        extern int personality(unsigned long persona);
        extern int execv(const char *path, char *const argv[]);
        extern void *sbrk(intptr_t increment);
        unsigned long room = pc_heap_room();
        if (room < PC_HEAP_ROOM_MIN && !getenv("PC_LAYOUT_REEXEC")) {
            int pers = personality(0xffffffffUL);
            if (pers != -1 && !(pers & 0x0040000)) {
                setenv("PC_LAYOUT_REEXEC", "1", 1);          /* exec once */
                if (personality((unsigned long)pers | 0x0040000UL) != -1)
                    execv("/proc/self/exe", argv);           /* returns only on failure */
            }
        }
        if (pc_map_fixed_regions() != 0) {
            /* a randomised brk start can sit across a fixed range already
             * (heap 1f7f5000-1f816000 over the register page: room above it
             * is large, so the check above passed); retry once without
             * randomisation.  PC_LAYOUT_NO_RETRY=1 restores the hard exit. */
            int pers = personality(0xffffffffUL);
            if (!getenv("PC_LAYOUT_REEXEC") && !getenv("PC_LAYOUT_NO_RETRY") &&
                pers != -1 && !(pers & 0x0040000)) {
                fprintf(stderr, "[INIT] fixed range blocked; re-executing without ASLR\n");
                setenv("PC_LAYOUT_REEXEC", "1", 1);
                if (personality((unsigned long)pers | 0x0040000UL) != -1)
                    execv("/proc/self/exe", argv);
            }
            return 1;
        }
        room = pc_heap_room();
        printf("[INIT] PS1 RAM and MMIO mapped; brk heap at %p, %lu MB to the next fixed range%s\n",
               sbrk(0), room >> 20, getenv("PC_LAYOUT_REEXEC") ? " (re-executed without ASLR)" : "");
        if (room < PC_HEAP_ROOM_MIN)
            fprintf(stderr, "[INIT] WARNING: brk heap room %lu MB < %lu MB; malloc may leave "
                    "the low heap\n", room >> 20, PC_HEAP_ROOM_MIN >> 20);
    }
    mallopt(GLIBC_M_MMAP_MAX, 0);                      /* every malloc from the brk heap (low) */
    mallopt(GLIBC_M_MMAP_THRESHOLD, 1 << 30);
    mallopt(GLIBC_M_TOP_PAD, 64 << 20);
    stk = MAP_FAILED;
    {
        /* Debug toolkit only (pc/hal/dbg_state.c, docs/pc-port/DEBUG-TOOLS.md):
         * with one of its variables set, the process runs without address
         * randomisation (re-executed once with ADDR_NO_RANDOMIZE) and the
         * game stack sits at a fixed address.  A save state is an image of
         * this process; with the heap, libraries and stacks where they were
         * in the saving process, the host values game code picks up from
         * uninitialised locals and the heap/MMIO-window layout are the same
         * in the process that loads it.  TOKI_SAVE_DIR (set by the player
         * launcher, pc/start_game.py) asks for the same layout, so a
         * quicksave (pc/hal/quicksave.c) loads into a later session with
         * its game stack restored.  Harness runs set none of these and are
         * untouched. */
        static const char *const dbg_vars[] = {
            "PC_SAVESTATE", "PC_LOADSTATE", "PC_DBG_CMDS", "PC_DBG_CONSOLE", "PC_DBG_SLOT",
            "TOKI_SAVE_DIR", NULL
        };
        extern int personality(unsigned long persona);
        extern int execv(const char *path, char *const argv[]);
        int i;
        for (i = 0; dbg_vars[i]; i++) {
            const char *e = getenv(dbg_vars[i]);
            if (e && *e) {
                int pers = personality(0xffffffffUL);
                if (pers != -1 && !(pers & 0x0040000) && !getenv("PC_DBG_ASLR")) {
                    setenv("PC_DBG_ASLR", "off", 1);       /* exec once */
                    if (personality((unsigned long)pers | 0x0040000UL) != -1)
                        execv("/proc/self/exe", argv);       /* returns only on failure */
                }
                stk = mmap((void *)0x60000000ul, GAME_STACK_SIZE, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
                break;
            }
        }
    }
    if (stk == MAP_FAILED)
        stk = mmap(NULL, GAME_STACK_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (stk == MAP_FAILED || (uintptr_t)stk + GAME_STACK_SIZE > 0x80000000u) {
        fprintf(stderr, "[INIT] low game stack unavailable; running on the host stack\n");
        return real_main(argc, argv);
    }
    g_main_argc = argc; g_main_argv = argv;
    getcontext(&game_ctx);
    game_ctx.uc_stack.ss_sp = stk;
    game_ctx.uc_stack.ss_size = GAME_STACK_SIZE;
    game_ctx.uc_link = &main_ctx;
    makecontext(&game_ctx, game_stack_entry, 0);
    swapcontext(&main_ctx, &game_ctx);
    return g_main_ret;
}

#endif /* _WIN32 */

/* A fresh, zeroed window over part of the mapped PS1 RAM block (the low
 * RAM below the executable, the mesh-pointer and logo-header regions):
 * Linux maps new anonymous pages over it (MAP_FIXED), Windows clears the
 * committed pages -- the same contents.  NULL on failure. */
static void *ps1_fresh_window(uintptr_t addr, size_t size)
{
#ifdef _WIN32
    memset((void *)addr, 0, size);
    return (void *)addr;
#else
    void *p = mmap((void *)addr, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}

/* PC_TEST_FAULT=VBLANK:KIND -- tests only (pc/tools/test_fault_paths.py,
 * which runs the Linux build and, under Wine, the Windows build): at the
 * VBLANKth emulated VBlank (pc/hal/system.c hal_vblank_interrupt, on the
 * game thread inside the main loop's recovery region -- the boot runs
 * nested in game_main, so the loop itself may not turn), provoke one of
 * the faults the handlers above model, once:
 *   segv   a store to an unmapped address: a [CRASH] line and the recovery
 *   zero   a load from the PS1 zero page: emulated (reads 0), no crash
 *   fold   a load through a sign-extended PS1 address: one [FOLD] site
 *   stall  a host stall: the watchdog's [CRASH] sig=14 and the recovery
 * Unset (every real run), it is one getenv at the first VBlank. */
void pc_test_fault_tick(void)
{
    static int at = -2, vblanks;
    static char kind[8];
    if (at == -2) {
        const char *e = getenv("PC_TEST_FAULT");
        at = -1;
        if (e && sscanf(e, "%d:%7s", &at, kind) != 2) at = -1;
    }
    if (at < 0 || vblanks++ != at) return;
    at = -1;
    if (!g_recover_active) return;
    fprintf(stderr, "[TEST] PC_TEST_FAULT %s at VBlank %d\n", kind, vblanks - 1);
    fflush(stderr);
    if (!strcmp(kind, "segv")) {
        volatile uintptr_t bad = (uintptr_t)0x10000000000ull;   /* 1 TB: never mapped */
        *(volatile int *)bad = 1;
    } else if (!strcmp(kind, "zero")) {
        volatile uintptr_t low = 0x40;
        volatile u32 v = *(volatile u32 *)low;
        fprintf(stderr, "[TEST] zero-page load read %u\n", (unsigned)v);
    } else if (!strcmp(kind, "fold")) {
        volatile s32 ps1 = (s32)0x800E8DA0u;                   /* cdq_head */
        volatile u8 v = *(volatile u8 *)(intptr_t)ps1;        /* sign-extended */
        fprintf(stderr, "[TEST] folded load read %u\n", (unsigned)v);
    } else if (!strcmp(kind, "stall")) {
        /* mostly loads, so the watchdog's alarm lands on a `mov` (the
         * signal must not be mistaken for a zero-page fault) */
        volatile int spin = 1, a, b, c, d;
        while (spin) { a = spin; b = spin; c = spin; d = spin; }
        (void)a; (void)b; (void)c; (void)d;
    } else if (!strcmp(kind, "sleep")) {
        /* a stall inside the host OS (here Sleep in a system DLL) longer
         * than the watchdog: on Windows the watchdog waits for it to
         * return instead of jumping out of the DLL (pc/hal/watchdog.c) */
        SDL_Delay(7000);                  /* pc/hal/watchdog.c WATCHDOG_SEC is 5 */
    }
    fprintf(stderr, "[TEST] PC_TEST_FAULT %s returned\n", kind);
    fflush(stderr);
}

#ifdef _WIN32
/* The Windows build is a GUI program (no console window behind the game).
 * Started from a Command Prompt for --check / --licenses / --prepare-assets,
 * it borrows that console for its output, unless stdout already goes
 * somewhere (a pipe or a file: the harnesses). */
static void win_attach_console(void) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN) return;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
        printf("\n");
    }
}
#endif

static int real_main(int argc, char *argv[]) {
    const char *asset_dir;
    PcSetupOpts setup;
    int status;

    /* --profile=parity|enhanced (--enhanced, --parity): pc/hal/profile.h.
     * Taken out of argv before the asset directory is read. */
    argc = hal_profile_parse_args(argc, argv);
    /* --disc / --audio-track / --check / --licenses / --prepare-assets /
     * --verify-assets (pc/include/pc_setup.h) */
    argc = pc_setup_args(argc, argv, &setup);
    pc_data_root();
#ifdef _WIN32
    if (setup.cli || setup.licenses || setup.check) win_attach_console();
#endif
    /* an install of the old layout (assets/, assets-en/) moves to data/
     * (pc/include/pc_data_root.h); not for --licenses / --prepare-assets,
     * which change nothing of the install */
    if (!setup.cli && !setup.licenses) pc_data_layout_migrate();
    if (setup.check && !getenv("TOKI_LOG_OFF")) {
        char lp[PC_ROOT_PATH_MAX + 32];
        snprintf(lp, sizeof lp, "%s/tokimeki-pc.log", pc_data_dir());
        pc_fs_mkdirs(pc_data_dir());
        pc_log_open(lp, 0);
    }
    if (pc_setup_cli(&setup, argc, argv, &status)) return status;

    /* Which game data: an explicit folder, the historical ./pc/assets, or
     * the data root's assets/ (checked); NULL = the setup screen makes it. */
    asset_dir = pc_setup_choose(argc > 1 ? argv[1] : NULL, &setup);
    if ((!asset_dir || pc_data_root_player()) && !getenv("TOKI_LOG_OFF")) {
        /* tokimeki-pc.log in the data folder: the player's runs only.  In
         * an automated run it is still written (tests read it), but stdout
         * stays where the harness reads it. */
        char lp[PC_ROOT_PATH_MAX + 32];
        int harness = pc_settings_harness_reason((pc_getenv_fn)getenv) != NULL;
        snprintf(lp, sizeof lp, "%s/tokimeki-pc.log", pc_data_dir());
        pc_fs_mkdirs(pc_data_dir());
        if (pc_log_open(lp, !harness) == 0) {
            time_t now = time(NULL);
            char when[64];
            strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&now));
            pc_logf("[LOG] tokimeki-pc started %s; log: %s", when, lp);
        }
    }

    printf("Heartthrob Memorial PC Enhanced (unofficial PC port for Tokimeki Memorial: Forever with You)\n");
    printf("Asset directory: %s\n", asset_dir ? asset_dir : "(none yet: first-run setup)");
    fflush(stdout);

    /* window.ini / hints.ini of the old layout -> [state] of settings.ini
     * (a player's settings file only; an automated run has none) */
    {
        const PcSettings *ps = pc_settings_get();
        if (!ps->harness && ps->file_path[0]) {
            int n = pc_settings_fold_legacy(ps->file_path, ps->config_dir);
            if (n) pc_logf("[DATA] %d old state file(s) folded into %s and removed", n, ps->file_path);
        }
    }

    /* [game] language (pc/hal/lang.c): before any asset is opened.  ja is
     * a no-op; en prepares <asset_dir>/en from the embedded pack.  On a
     * first run there is no folder yet: hal_init runs it right after the
     * setup screen made one. */
    if (asset_dir) hal_lang_init(asset_dir);

    /* NULL: hal_init opens the window and runs the setup screen in it */
    hal_init(asset_dir);
    asset_dir = pc_asset_dir();
    pc_logf("[INIT] game data: %s", asset_dir);
    pc_setup_menu_register();

    /* Map PS1 overlay address space (0x80132000-0x80232000, 1MB).
     * TT overlay and others access globals at 0x8015XXXX-0x8016XXXX via
     * hardcoded PS1 pointer casts.  On x86-64, 0x80XXXXXX addresses are
     * valid user-space (zero-extended), so mmap here provides zero-initialised
     * memory that prevents SIGSEGV on those accesses.
     *
     * NOTE: do NOT extend this to cover main-binary RAM (0x80000000-0x80132000).
     * Doing so makes raw-pointer writes to state globals succeed when they
     * previously crashed-and-recovered-to-title via setjmp. That pushes
     * `now_step` (current scene) past 64 during init, so the title screen
     * never renders. Scene-specific raw pointer crashes should be fixed
     * surgically at the callsite (replace `*(T*)0x800XXXXX` with the named
     * global) instead of papered over with a broader mmap. */
    /* The globals model (2026-10-18): the FULL 2 MB PS1 RAM at 0x80000000.
     * Data symbols resolve here (pc/ld/psx_globals.syms via --just-symbols)
     * and the CD HAL delivers the EXN/sector data into it.  This supersedes
     * the old partial windows AND the former 'do NOT extend to main-binary
     * RAM' note -- the rule is that the matched PS1 code runs against the
     * real data, and the scene matrix is the oracle for the fallout. */
    {
        /* mapped by main() before the heap grew (pc/src/ps1_regions.c) */
        printf("[INIT] PS1 RAM block mapped at %p\n", (void *)0x80000000ul);
        /* Boot data: the retail executable's data segments (the clean boot
         * state).  The PS-X EXE header carries the load address and size.
         * A user-supplied, gitignored disks/dumps/ram.bin is a MID-GAME
         * snapshot and NOT a reliable source (its 0xB7080 slot may hold text,
         * not the libetc vtable), so the EXE wins and the dump is a fallback. */
        {
            char exe_path[PC_ROOT_PATH_MAX + 32], en_exe[PC_ROOT_PATH_MAX + 32];
            const char *boot_exe = lang_path(pc_asset_path(exe_path, sizeof exe_path, "raw/PSX.EXE"),
                                             en_exe, sizeof en_exe);
            FILE *rf = fopen(boot_exe, "rb");
            if (rf) {
                unsigned char hdr[0x800];
                size_t got = fread(hdr, 1, sizeof hdr, rf);
                if (got == sizeof hdr && memcmp(hdr, "PS-X EXE", 8) == 0) {
                    u32 load = (u32)hdr[0x18] | ((u32)hdr[0x19] << 8) |
                               ((u32)hdr[0x1A] << 16) | ((u32)hdr[0x1B] << 24);
                    u32 size = (u32)hdr[0x1C] | ((u32)hdr[0x1D] << 8) |
                               ((u32)hdr[0x1E] << 16) | ((u32)hdr[0x1F] << 24);
                    if (load >= 0x80000000u && load + size <= 0x80200000u) {
                        fread((void *)(uintptr_t)load, 1, size, rf);
                        printf("[INIT] boot data from the EXE: %u bytes at 0x%08X\n",
                               (unsigned)size, (unsigned)load);
                    }
                    /* the generated globals' retail initial values (pc_retail_init.h):
                     * from the executable just loaded -- the Japanese one, as
                     * their initialisers were, or with [game] language = en the
                     * English one, whose bytes the console has at those
                     * addresses.  Most of these names link to their PS1
                     * addresses (psx_globals.syms), so the fill writes PS1 RAM:
                     * from the Japanese file it put Japanese back over the
                     * English join_club_message strings, club names, cursor /
                     * voice settings and save dialogs
                     * (docs/translation/BROKEN-STRINGS-AUDIT.md section 1). */
                    pc_retail_init_load(boot_exe);
                }
                fclose(rf);
            } else {
                FILE *df = fopen("disks/dumps/ram.bin", "rb");
                if (df) {
                    size_t got = fread((void *)(uintptr_t)0x80000000u, 1, 0x200000u, df);
                    fclose(df);
                    printf("[INIT] boot data seeded from disks/dumps/ram.bin (%u bytes)\n",
                           (unsigned)got);
                } else {
                    printf("[INIT] no boot image; PS1 RAM starts zeroed\n");
                }
            }
        }
    }

    /* Map the low PS1 RAM window 0x80010000-0x80041000 (196 KB, below the
     * executable).  The sound driver's CD loads land there: device_load_start
     * reads VAB headers / SEQ data to 0x80010000, 0x80012000, 0x8001A000 and
     * 0x8001C000, and device_port_init then checks a word of that data.  The
     * CD HAL (pc/hal/cd.c) delivers sectors only into mapped windows.
     * Added 2026-10-15 with the tick-accurate CD model. */
    {
        void *low_mem = ps1_fresh_window(0x80010000u, 0x31000);
        if (!low_mem) {
            perror("[INIT] mmap low RAM window failed");
        } else {
            printf("[INIT] Low RAM window mapped at %p\n", low_mem);
        }
    }

    /* 0x800C0000-0x800E0000 holds the executable's own bytes, loaded just
     * above (PSX.EXE spans 0x80041000-0x800E9000; with [game] language = en
     * the English one).  The two windows below are re-mapped fresh, so their
     * bytes are kept across that and put back: the same bytes the tracked
     * retail blob pc/src/game_data.c used to supply (it was byte-identical
     * to PSX.EXE there).  The executable contains no retail data of its own
     * (pc/tools/audit_retail_bytes.py). */
    static u8 exe_c0[0x20000];
    memcpy(exe_c0, (void *)(uintptr_t)0x800C0000u, sizeof exe_c0);

    /* Map sprite-mesh pointer-table region 0x800C0000-0x800D0000 (64KB) and
     * seed it from the executable's bytes. sprite_slot_disp (sprite emit driver)
     * indexes pv1[fi] off slot pointers set by icon_init etc. as
     * `(u32)&basyo_init_tbl1`. Those symbols are .set-aliased to their PS1
     * absolute addresses in pc/src/ps1_addr_aliases.c so the driver lands
     * on the authentic 174-entry pointer table.
     *
     * Without this mmap, the driver's bit31-gate on *pv1 fails (scalar=0)
     * and it falls through to the printf-stub path, emitting no sprite.
     *
     * Sprite slots (class_w) are BSS and carry across scene transitions.
     * If a previous scene's icon_init populated slots but the current
     * scene hasn't re-initialised them, `fi` can be out-of-range.
     * sprite_slot_disp guards pv1[fi] results against this 64KB range to
     * prevent SEGV on stale slots. */
    {
        void *mesh_mem = ps1_fresh_window(0x800C0000u, 0x10000);
        if (!mesh_mem) {
            perror("[INIT] mmap mesh-pointer region failed");
        } else {
            memcpy(mesh_mem, exe_c0, 0x10000);
            printf("[INIT] Mesh-pointer region mapped at %p (seeded %u bytes)\n",
                   mesh_mem, 0x10000u);
        }
        /* Seed CLUT/GPU table heads (iterated by hud_palette_init). Values
         * match PS1 .data at these addresses; rest of each table is zeroed
         * by the MAP_ANONYMOUS mmap above. See globals.c commentary. */
        *(volatile u32 *)0x800CC520 = 0x04210000;
        *(volatile u32 *)0x800CC700 = 0x04210000;
        *(volatile u32 *)0x800CC720 = 0x03E00000;
        *(volatile u32 *)0x800CC740 = 0x03E00000;
        *(volatile u32 *)0x800CC7C0 = 0x001F0000;
        *(volatile u32 *)0x800CC840 = 0x00000000;
    }

    /* Map 0x800D0000-0x800E0000 (64KB) for logo TIM header fields used
     * by hizuke_init: D_800DFC30 / D_800DFC5C / D_800DFF14 / D_800DFF54.
     * Aliases declared in pc/src/ps1_addr_aliases.c. Without this the
     * globals resolve to host addresses and their low 32 bits leak into
     * slot[12]/[16]/[52], producing invisible logo prims. */
    {
        void *logo_mem = ps1_fresh_window(0x800D0000u, 0x10000);
        if (!logo_mem) {
            perror("[INIT] mmap logo-header region failed");
        } else {
            memcpy(logo_mem, exe_c0 + 0x10000, 0x10000);
            printf("[INIT] Logo-header region mapped at %p (seeded %u bytes)\n",
                   logo_mem, 0x10000u);
        }
    }

    /* English: the English executable's bytes over the windows (already
     * there now: they were kept from the loaded executable), and the patch
     * module's data (pc/hal/lang.c). */
    hal_lang_boot_fixup();

    /* The PSX MMIO page (0x1F801000-0x1F802000): libsnd/libpress code
     * dereferences hardware-register addresses through pointer cells
     * (libpress_dma0_chcr_reg_ptr/libpress_dma1_chcr_reg_ptr, ...); writes land in this zeroed page.  It is
     * mapped by main() with the RAM, before the heap can grow over it
     * (pc/src/ps1_regions.c). */
    printf("[INIT] PSX MMIO region mapped at %p (4KB no-op buffer)\n", (void *)0x1F801000ul);

    hal_gpu_init();
    overlay_init();
    asset_map_init();

    /* Load font atlas into VRAM */
    tim_load_asset("EFONT162.TIM");

    /* Try to run the game's own init functions */
    printf("[INIT] Running game initialization...\n");
    install_crash_handler();

    g_recover_active = 1;
    if (setjmp(g_recover) == 0) {
        /* Restore _main's ResetCallback/CdInit/printf prologue, then
         * input_events_init before ResetGraph/screen_clear and the loop.
         * The event initializer registers the VBlank
         * kernel event (vblank_counter_tick -> vblnk_counter, the srand seed)
         * and the memory-card event, and starts the pad.  The PC init never
         * called it, so vblnk_counter stayed 0 (reference trace: 602 by
         * the end of the boot movie).  Added 2026-10-15. */
        {
            extern void input_events_init(void);
            extern void screen_clear(void);
            /* pc/hal/cpu_cost.c: _main's own init is game code too
             * (input_events_init, card_ev_set, SD_Init, system_reset's
             * game_work_init1 / girl_rec_init / work_clear ...): on the
             * console it takes ~250,000 ticks between _bu_init's return
             * and the movie's Setloc that the port charged nothing, so the
             * boot movie started that much earlier against the VBlanks.
             * Arm the cost model from _main's entry.  BOOT_CPU_COST=0 is
             * the negative control (armed at the main loop, as before). */
            {
                if (hal_model_on("BOOT_CPU_COST")) {
                    extern void hal_cpu_cost_arm(void);
                    hal_cpu_cost_arm();
                }
            }
            cd_hal_boot_handoff();
            ResetCallback();
            CdInit();
            /* _main_str0 is only a byte in generated host globals. Read
             * the full original string from the mapped owner EXE data. */
            func_800B4850((const char *)(uintptr_t)0x800B5A90u);
            input_events_init();
            ResetGraph(0);
            screen_clear();
        }
        /* _main's init order: SD_Init (sound driver state + SsInit over the
         * SPU HAL), CDQ_init (the CD queue: cdq_head.flags = 2, phase -1),
         * graph_init, system_reset(1).  SD_Init and CDQ_init were never
         * called on the PC until 2026-10-15 (the sound command queue and the
         * CD queue were pc-port stubs). */
        {
            extern void SD_Init(void);
            extern void CDQ_init(void);
            printf("[INIT] SD_Init / CDQ_init...\n");
            SD_Init();
            CDQ_init();
        }
        printf("[INIT] graph_init (display setup)...\n");
        graph_init();
    } else {
        printf("[INIT] CRASH in graph_init — skipping\n");
    }

    g_recover_active = 1;
    if (setjmp(g_recover) == 0) {
        printf("[INIT] system_reset (game state init)...\n");
        system_reset(1);    /* _main passes 1: clear VRAM and the 6796-byte state block too */
    } else {
        printf("[INIT] CRASH in system_reset — skipping\n");
    }
    g_recover_active = 0;
    printf("[INIT] Init complete. Scene=%d\n", now_step);

    /* system_reset owns brightness, fade velocity and voice settings.
     * bri_step is a signed increment, not a green modulation channel. */

    /* Enter the PS1 boot chain at scene 0. Game init defaults
     * now_step to something else; we force 0 so game_main
     * dispatches to boot_main, which drops into boot_phase_switch
     * (Konami FMV phase machine) and queues KONAMIC3.STR via
     * cd_read_start2. The HAL's str_player_pump() picks it up and
     * plays it inline — see src/main/pc-port/cd_read_start2.c.
     *
     * D_800FF869 is the memcard-OK flag consulted by boot_main:
     *   0 -> run the FMV path (what we want).
     *   1 -> skip FMV, jump straight to the memcard-check scene (18).
     * Leave it at 0 (default) so the boot FMV plays.
     *
     * 2026-10-15: the force to 0 is gone.  system_reset() sets now_step = 16
     * and the PS1 runs scene 16 for ~150 frames before scene 0 (reference
     * emulator trace); skipping it was a PC-only scene jump.  It had been forced
     * because a mis-filed override (pc-port/prim_init.c, really a broken
     * translation of srn_tpage_show) crashed scene 16; that file is deleted. */

    /* Dev: force overlay scene for testing (set via SCENE env var) */
    {
        const char *scene_env = getenv("SCENE");
        if (scene_env) {
            int s = atoi(scene_env);
            printf("[DEV] Forcing scene to %d from SCENE env\n", s);
            now_step = (u8)s;
            /* (A SCENE= jump used to pin cd_state2 = 1 so polls of
             * get_CD_State2() & 1 would not stall.  The CD queue is real now
             * (pc/hal/cd.c): a scene that waits for a load it never issued
             * waits, as it would on the console.  2026-10-15) */
        }
    }

    /* Scene navigation (the scene-matrix oracle mode): SCENE_NAV=N runs the
     * REAL boot first -- the scene-16 movie, scene 0's boot_data_load (the CD
     * queue, hud_palette_init, the icon_init slot population) -- and then
     * navigates to scene N the way the game itself changes scenes (goto_step:
     * it resets the sub-steps and runs the transition side effects).  This is
     * the faithful scenario the direct SCENE= start skips: the matched code
     * trusts the boot state (sprite slots, CD queue, decoder state), and
     * starting scenes cold dereferences state no PS1 path ever leaves unset.
     * PC_TRACE_FRAMES counts from the navigation (trace_anchor). */
    int nav_target = -1;
    int nav_done = 0;
    {
        const char *nav_env = getenv("SCENE_NAV");
        if (nav_env) {
            nav_target = atoi(nav_env);
            printf("[NAV] SCENE_NAV=%d: boot first, goto_step(%d) at the title\n",
                   nav_target, nav_target);
        }
    }

    /* Dev: SCENE_LOCK=N holds now_step at N every frame so attract-mode
     * auto-advance doesn't change scene during visual verification. */
    int scene_lock = -1;
    {
        const char *lock_env = getenv("SCENE_LOCK");
        if (lock_env) {
            scene_lock = atoi(lock_env);
            printf("[DEV] SCENE_LOCK=%d (holding now_step each frame)\n", scene_lock);
        }
    }

    /* Enter main game loop with crash recovery */
    install_crash_handler();
    /* Trace-build (TRACE=1): arm per-function ENTER/EXIT logging now that
     * stdio + dlfcn are safe. Output to OVL_TRACE_FILE or stderr. No-op
     * in non-TRACE builds (the symbol is weak in instrument.c). */
    {
        extern void instrument_arm(void) __attribute__((weak));
        if (instrument_arm) instrument_arm();
    }
    {
        /* pc/hal/cpu_cost.c: game code takes retail's CPU time from here on */
        extern void hal_cpu_cost_arm(void);
        hal_cpu_cost_arm();
    }
    printf("Entering main loop (scene=%d)...\n", now_step);
    {
        /* pc/hal/trace.c: PC_TRACE / PC_INPUT / PC_TURBO / PC_TRACE_FRAMES
         * (the PC side of pc/tools/trace_compare.py) */
        extern void trace_init(void);
        trace_init();
    }
    {
        /* pc/hal/dbg_console.c: the debug toolkit (save states, command
         * scripts, live console; docs/pc-port/DEBUG-TOOLS.md).  Off unless
         * one of its PC_SAVESTATE / PC_LOADSTATE / PC_DBG_* variables or a
         * hotkey asks; then it acts only at dbg_frame_boundary() below. */
        extern void dbg_toolkit_init(void);
        dbg_toolkit_init();
    }
    hal_frame_begin();   /* host events; afterwards every waiting VSync presents + polls (pc/hal/system.c) */
    while (hal_is_running()) {
        {
            /* the debug toolkit's frame boundary: no game code on the stack
             * (returns at once when the toolkit is unused) */
            extern void dbg_frame_boundary(void);
            dbg_frame_boundary();
        }
        /* A quit decided at the boundary (the resume state just written,
         * pc/hal/save_auto.c; the toolkit's `winquit`) ends the run here:
         * until 2026-11-03 one more game frame ran after the resume state
         * was written, so that frame's work (a memory-card write) was in
         * the session but not in the state offered as "where you left off". */
        if (!hal_is_running()) break;

        /* GFXTEST standalone mode — skip ALL game logic */
        {
            extern int gfxtest_is_active(void);
            extern void gfxtest_draw(void);
            if (gfxtest_is_active()) {
                gfxtest_draw();  /* presents before restoring its origin */
                hal_frame_begin();
                continue;
            }
        }

        if (scene_lock >= 0) {
            now_step = (u8)scene_lock;
        }

        /* SCENE_NAV: navigate once the boot chain has run.  The title (18) is
         * the booted state -- boot_data_load has already run hud_palette_init /
         * icon_init by then, so the sprite slots the matched code trusts are
         * populated exactly as on the console.  3000 game frames is a generous
         * fallback for boots that stall before the title (logged as such). */
        if (nav_target >= 0 && !nav_done) {
            extern void goto_step(s32);
            extern void trace_anchor(void);
            if ((int)now_step == 18 || (int)frame_counter >= 3000) {
                if ((int)now_step != nav_target) {
                    printf("[NAV] boot done at game frame %d (scene %d): "
                           "goto_step(%d)\n",
                           (int)frame_counter, (int)now_step, nav_target);
                    goto_step(nav_target);
                } else {
                    printf("[NAV] boot done at game frame %d: already at "
                           "scene %d\n", (int)frame_counter, nav_target);
                }
                trace_anchor();
                nav_done = 1;
            }
        }


        /* Drive scene-based BGM — runs before the game dispatches so the
         * music is already queued by the time rendering starts. */
        {
            extern void audio_scene_tick(u8 current_scene);
            audio_scene_tick(now_step);
        }

        /* (Two per-frame pins were removed on 2026-10-15: D_800BA14C = 0
         * and cd_state2 = 1 for the title scenes.  cd_state2 is
         * cdq_head.flags (pc/src/layout_aliases.ld), written by the matched
         * CD queue worker below when a read completes.) */

        g_recover_active = 1;
        {
            extern u8 now_sub_sub_step;
            u8 pre_sub = now_sub_sub_step;

            /* Bound work between completed frames. Standalone loops such
             * as TT refresh this after VSync/presentation; a computation or
             * rendering stall still expires. TRACE builds retain WATCHDOG_SEC. */
            pc_watchdog_arm();

            if (setjmp(g_recover) == 0) {
                ot_buff_set();
                /* Retail _main does not clear VRAM here. The game's draw
                 * environment/ordering table owns framebuffer clearing. */
                /* Clear the OT AFTER ot_buff_set sets up buffers for this frame.
                 * OT is at ot_buff0 + (activeBuff * 1024), 256 entries. */
                {
                    extern u8 ot_buff0[];
                    extern s32 activeBuff;
                    #define OT_SIZE 256
                    u32 *ot_base = (u32 *)&ot_buff0[activeBuff << 10];
                    static int clr_trace = 0;
                    if (clr_trace < 3) { debug_log("ClearOTagR: buf=%d ot=%p", activeBuff, (void*)ot_base); clr_trace++; }
                    { extern void gpu_clear_otag_r(u32 *, s32); gpu_clear_otag_r(ot_base, OT_SIZE); }
                    #undef OT_SIZE
                }
                {
                    static int scene_trace = 0;
                    if (scene_trace < 5) {
                        debug_log("MAIN: calling game_main (scene dispatch)");
                        scene_trace++;
                    }
                }
                /* _main's frame, in _main's order: ot_buff_set, game_main,
                 * frame_counter++, vblank_function(1) (its VSync(0) is the
                 * frame's VBlank -- the pad is polled there, by
                 * hal_vblank_interrupt -- and pad_main runs right after it),
                 * the CD queue worker, the sound command processor.  Until
                 * 2026-10-16 the PC polled the pad and ran pad_main before
                 * game_main and counted the frame after the VBlank. */
                game_main();
                if (scene_lock >= 0) {
                    now_step = (u8)scene_lock;
                }
                frame_counter++;
                {
                    static int render_trace = 0;
                    if (render_trace < 5) {
                        debug_log("MAIN: game_main returned, calling vblank_function");
                        render_trace++;
                    }
                }
                /* _main's vblank_function(1).  class_flag (sprite_slot_disp
                 * on/off) is the game's own: class_switch sets it
                 * (title_data_read, scene inits).  A PC-only `class_flag = 1`
                 * here pinned it on every frame until 2026-10-30. */
                vblank_function(1);
                {
                    static int done_trace = 0;
                    if (done_trace < 5) {
                        debug_log("MAIN: vblank_function returned");
                        done_trace++;
                    }
                }
                /* The frame's VBlank fell inside vblank_function (its
                 * VSync(0) waits on the system clock, pc/hal/system.c).  Then
                 * what _main does right after: the CD queue worker
                 * (func_800457FC -> cdq_command_process) and the sound
                 * driver's command processor.  Neither ran on the PC until
                 * 2026-10-15. */
                {
                    extern void func_800457FC(void);
                    extern void SD_CmdProc(void);
                    extern u8 sd_proc_on;
                    func_800457FC();
                    if (sd_proc_on != 0) {
                        SD_CmdProc();
                    }
                }
                g_crash_run = 0;
            } else {
                g_crash_run++;
                if (g_crash_count <= 3 || g_crash_count % 600 == 0)
                    debug_log("TIMEOUT/CRASH #%d scene=%d sub=%d/%d pc=%p sym=%s fault=%p",
                              g_crash_count, now_step, now_sub_step, now_sub_sub_step,
                              g_last_fault_pc,
                              g_last_fault_sym[0] ? g_last_fault_sym : "(none)",
                              g_last_fault_addr);
                /* A player's game that faults on every frame for five seconds
                 * cannot go on: say so and stop, instead of a frozen window.
                 * Automated runs keep running (the scene matrix records the
                 * scene as CRASH). */
                if (g_crash_run >= 300 && !pc_settings_get()->harness) {
                    char msg[512];
                    const char *lp = pc_log_path();
                    snprintf(msg, sizeof msg,
                             "The game hit an error it cannot recover from "
                             "(scene %d, %s).\n\nPlease report it with the log file:\n%s",
                             (int)now_step,
                             g_last_fault_sym[0] ? g_last_fault_sym : "unknown",
                             lp ? lp : "(no log file)");
                    pc_logf("[FATAL] %d crashing frames in a row; stopping. %s", g_crash_run,
                            g_last_fault_sym);
                    fflush(stdout);
                    fflush(stderr);
                    hal_show_error("Heartthrob Memorial PC Enhanced", msg);
                    break;
                }
                /* (The recovery used to advance now_sub_sub_step so a scene
                 * crashing every frame would move on -- a PC-only scene
                 * advance.  Removed 2026-10-15; a crashing scene now stays
                 * put and shows in the matrix as CRASH.) */
                (void)pre_sub;
            }

            pc_watchdog_disarm();

            /* (str_player_pump() was called here for the FMV the old
             * pc-port cd_read_start2 queued; the movie runs through libstr /
             * libpress now -- pc/hal/libpress.c -- and nothing queues.) */
        }
        g_recover_active = 0;

        /* (A PC-only "render flush" DrawOTag of the frame's OT ran here,
         * and a DrawSync(0) after the trace line, since the April 2026 POC.
         * The matched vblank_function already draws that OT (GsDrawOt), so
         * every OT was drawn twice, and once the GPU took drawing time
         * (pc/hal/gpu.c gpu_prim_ticks) the extra DrawSync waited ~190,000
         * ticks a frame that retail never waits.  _main does neither.
         * Removed 2026-10-31.) */

        /* Log frame loop health every 60 frames */
        if ((frame_counter % 60) == 0) {
            debug_log("HEARTBEAT frame=%d scene=%d sub=%d crashes=%d",
                      frame_counter, now_step, now_sub_step, g_crash_count);
            /* the log is a file: flush it once a second, so a hang or a
             * killed process still leaves what led up to it */
            fflush(stdout);
            fflush(stderr);
        }

        /* (frame_counter++ is right after game_main, as in _main.
         * step_frame_cnt is incremented by the matched game_main; the PC
         * loop used to increment it a second time, running every scene timer
         * at double speed -- found by pc/tools/trace_compare.py, 2026-10-15) */
        {
            extern void trace_frame(void);   /* pc/hal/trace.c: one trace line per game frame */
            trace_frame();
        }

        /* Drain any pending quit events that arrived during game logic
         * (SIGALRM can interrupt SDL_PollEvent in hal_frame_begin) */
        hal_poll_quit();
        if (!hal_is_running()) break;

        /* (The loop used to flip PSDIDX here and copy it to activeBuff.
         * vblank_function's GsSwapDispBuff (matched) already flips it, so
         * the two flips cancelled and the game drew into the same buffer
         * every frame (trace: disp_buf stuck at 1; the PS1 alternates).
         * ot_buff_set sets activeBuff = GsGetActiveBuff() at the top of the
         * frame, as on the console.  2026-10-16) */
        /* Keep the matched GsSwapDispBuff's display/draw state. Reapplying
         * the old HAL's private environments here overwrote retail VRAM
         * placement after every game frame. */

        /* Sound-test overlay (F8) — drawn last so it sits on top of
         * whatever the game rendered this frame. */
        {
            extern int soundtest_is_active(void);
            extern void soundtest_draw(void);
            if (soundtest_is_active()) soundtest_draw();
        }

        /* Graphics-test overlay (F7) */
        {
            extern int gfxtest_is_active(void);
            extern void gfxtest_draw(void);
            if (gfxtest_is_active()) gfxtest_draw();
        }

        /* (the frame was presented by vblank_function's VSync(0)) */
    }

    hal_shutdown();
    return 0;
}
