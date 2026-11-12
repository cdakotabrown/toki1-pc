/* SPDX-License-Identifier: GPL-3.0-or-later */
/* overlay.c — EXN Overlay Dispatch System for PC Port
 *
 * On PS1, 27 EXN overlay files contain scene-specific code loaded at
 * runtime to 0x80132000. The entry point function at 0x8013E830
 * (D_8013E830) handles the current scene.
 *
 * For the PC port, we use overlay_dispatch() which checks the dispatch
 * table for a real handler, falling back to the stub screen for
 * unimplemented scenes.
 */
#include "hal.h"
#include "pc_data_root.h"
#include "lang.h"
#include <stdio.h>
#include <stdint.h>

extern void ClearImage(RECT *rect, u8 r, u8 g, u8 b);
extern u32 PadRead(s32 port);
extern void goto_step(s32);
extern void draw_sprt(s32 x, s32 y, s32 w, s32 h, u8 u0, u8 v0,
                      u16 clut, u16 tpage, u8 r, u8 g, u8 b);
extern void debug_log(const char *fmt, ...);

extern u8 now_step;

#define CHAR_U(ch) ((ch >= 'A' && ch <= 'Z') ? ((ch - 'A') * 8) : \
                    (ch >= 'a' && ch <= 'z') ? ((ch - 'a') * 8) : \
                    (ch >= '0' && ch <= '9') ? ((ch - '0') * 8) : 0)
#define CHAR_V(ch) ((ch >= 'A' && ch <= 'Z') ? 0 : \
                    (ch >= 'a' && ch <= 'z') ? 32 : \
                    (ch >= '0' && ch <= '9') ? 16 : 0)

static void draw_text(s32 x, s32 y, const char *text, u8 r, u8 g, u8 b) {
    u16 tpage = 0x000A;
    u16 clut = (481 << 6) | 0;
    s32 i;
    for (i = 0; text[i]; i++) {
        char ch = text[i];
        if (ch == ' ') continue;
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
            draw_sprt(x + i * 8, y, 8, 14, CHAR_U(ch), CHAR_V(ch), clut, tpage, r, g, b);
        }
    }
}

/* Stub handler — shown for unimplemented overlay scenes */
void overlay_stub_handler(void) {
    RECT r;
    static s32 debounce = 0;
    const char *scene_name;
    const char *overlay_name;

    /* Get scene info from dispatch table */
    extern const char *overlay_get_name(u8);
    extern const char *overlay_get_description(u8);
    scene_name = overlay_get_description(now_step);
    overlay_name = overlay_get_name(now_step);

    /* Dark blue background */
    r.x = 0; r.y = 0; r.w = 320; r.h = 240;
    ClearImage(&r, 0, 0, 40);

    /* Scene info box */
    r.x = 30; r.y = 60; r.w = 260; r.h = 120;
    ClearImage(&r, 10, 10, 30);

    /* Title */
    draw_text(60, 70, "SCENE", 255, 200, 100);

    /* Scene ID number */
    {
        char num[8];
        snprintf(num, sizeof(num), "%03d", now_step);
        draw_text(120, 70, num, 255, 255, 100);
    }

    /* Overlay name */
    draw_text(60, 85, overlay_name, 200, 200, 100);

    /* Scene description */
    draw_text(60, 100, scene_name, 255, 255, 255);

    /* Instructions */
    draw_text(40, 130, "NOT YET IMPLEMENTED", 200, 100, 100);
    draw_text(40, 155, "PRESS ESC TO RETURN", 150, 150, 200);

    DrawSync(0);

    /* Handle input — ESC returns to title */
    if (debounce > 0) debounce--;
    if (debounce == 0) {
        u32 pad = PadRead(0);
        u32 buttons = ~(pad & 0xFFFF);
        if (buttons & 0x0020) { /* CIRCLE = ESC */
            debounce = 30;
            debug_log("Overlay stub: returning to title from scene %d (%s)",
                      now_step, overlay_name);
            goto_step(50);
        }
    }
}

/* ---- the loaded overlay (the PS1 mechanism) ----------------------------
 * The main binary loads an overlay the way the console does: overlay_load_main
 * (step 2) -> overlay_load_init picks the EXN by prev_step and queues a
 * 96-sector read of it to 0x80132000; scene_entry_set then points
 * game_mode_step at the scene's entry (`func_80144A14`, ...) and game_main calls
 * through it.  On the PC every overlay is a native module, so a PS1 code
 * address in the window names "that function of the EXN now loaded": the CD
 * HAL reports each .EXN it delivers to 0x80132000 (hal_overlay_set_loaded),
 * and the generated trampolines (pc/src/overlay_symtab.c) resolve their
 * address in that overlay's table.  The EXN bytes themselves land in the
 * window too, as on the console. */
#include "overlay_symtab.h"
#include <string.h>
#include <stdlib.h>
#ifndef _WIN32
#include <ucontext.h>
#endif
#include "../src/psx_main_map.h"

static const OverlayTable *g_loaded;

void hal_overlay_set_loaded(const char *exn_path) {
    const char *base = strrchr(exn_path, '/');
    char name[32];
    size_t n;
    int i;
    base = base ? base + 1 : exn_path;
    n = strcspn(base, ".");
    if (n >= sizeof name) n = sizeof name - 1;
    memcpy(name, base, n); name[n] = 0;
    for (i = 0; i < g_overlay_table_count; i++) {
        if (!strcmp(g_overlay_tables[i].name, name)) {
            if (g_loaded != &g_overlay_tables[i])
                debug_log("[Overlay] %s.EXN loaded (%d functions)", name, g_overlay_tables[i].n);
            g_loaded = &g_overlay_tables[i];
            return;
        }
    }
    debug_log("[Overlay] %s: no native module", exn_path);
    g_loaded = NULL;
}

/* Load an overlay's EXN bytes into the mapped window (0x80132000) and
 * register its native module.  This is the PS1 loader flow
 * (overlay_load_init's CD read) that the PC scene dispatch must reproduce:
 * without it the window holds zeros and every table/string the overlay data
 * contains reads as NULL (the KANGEI/OMIMAI dispatch crashes).  Idempotent.
 * Returns 1 when the EXN was read. */
int hal_overlay_load_exn(const char *name) {
    char path[512];
    FILE *f;
    size_t got;
    static char loaded[32];
    if (!strcmp(loaded, name)) return 1;
    snprintf(path, sizeof path, "%s/raw/CDROM/EXEDIR/%s.EXN", pc_asset_dir(), name);
    {
        char en[512];
        const char *p = lang_path(path, en, sizeof en);   /* English EXN */
        if (p != path) snprintf(path, sizeof path, "%s", p);
    }
    f = fopen(path, "rb");
    if (!f) {
        debug_log("[Overlay] %s.EXN not readable (%s); window stays as-is", name, path);
        return 0;
    }
    got = fread((void *)(uintptr_t)0x80132000u, 1, 0x100000u, f);
    fclose(f);
    snprintf(loaded, sizeof loaded, "%s", name);
    debug_log("[Overlay] %s.EXN loaded to 0x80132000 (%u bytes)", name, (unsigned)got);
    hal_overlay_set_loaded(path);
    return 1;
}

const char *hal_overlay_loaded(void) { return g_loaded ? g_loaded->name : NULL; }

/* The English patch's overlay hooks (pc/tools/gen_lang_hooks.py): with
 * [game] language = en a call that arrives by address reaches the hooked
 * function's stub, like the module's own calls.  Weak: harnesses without
 * the translation layer. */
typedef struct { const char *ov; unsigned addr; void *fn; void *hook; } LangOvlRedirect;
extern const LangOvlRedirect g_lang_ovl_redirect[] __attribute__((weak));

static void *lang_redirect(void *fn) {
    const LangOvlRedirect *r;
    if (!lang_en() || !g_lang_ovl_redirect || !fn) return fn;
    for (r = g_lang_ovl_redirect; r->fn; r++)
        if (r->fn == fn) return r->hook;
    return fn;
}

void *hal_overlay_lookup(u32 addr) {
    int lo = 0, hi;
    if (!g_loaded) return NULL;
    hi = g_loaded->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        u32 a = g_loaded->tab[mid].addr;
        if (a == addr) return lang_redirect(g_loaded->tab[mid].fn);
        if (a < addr) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

static s32 overlay_unresolved(void) { return 0; }
int hal_overlay_unresolved(void) { return 0; }

/* A PS1 code address -> its native function: overlay code (the window,
 * >= 0x80132000) through the loaded overlay's table, main-binary code
 * through the registry map.  Retail data tables name both: DATE's
 * date_main0 handler table (D_8015BF30) holds cd_wait_timeout
 * (0x8007C814) and dec_bg_load_check (0x8007EE3C) beside the overlay's
 * own handlers.  NULL when nothing is there. */
static void *ps1_code_lookup(u32 addr) {
    int lo, hi;
    if (addr >= 0x80132000u) return hal_overlay_lookup(addr);
    lo = 0;
    hi = g_psx_main_map_n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_psx_main_map[mid].ps1 == addr) return g_psx_main_map[mid].native;
        if (g_psx_main_map[mid].ps1 < addr) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

/* called by overlay_tramp with the trampoline's PS1 address, and by the
 * generated function-table reads (pc/tools/ps1_function_tables.py) with a
 * retail table word, which may name main-binary code */
void *overlay_resolve(u32 addr) {
    void *fn = ps1_code_lookup(addr);
    if (!fn) {
        static u32 warned[64]; static int nw;
        int i;
        for (i = 0; i < nw && warned[i] != addr; i++) ;
        if (i == nw && nw < 64) {
            warned[nw++] = addr;
            fprintf(stderr, "[Overlay] call to 0x%08X: no function there in the loaded overlay (%s)"
                    " or the main binary; returns 0\n",
                    addr, g_loaded ? g_loaded->name : "none loaded");
        }
        return (void *)overlay_unresolved;
    }
    return fn;
}

/* overlay_tramp: entered by `jmp` from a trampoline with %eax = the PS1
 * address; keeps the argument registers, resolves, and tail-jumps.
 * System V (Linux): six integer argument registers, the PS1 address goes to
 * overlay_resolve in %edi.  Microsoft x64 (Windows):
 * four, %rcx %rdx %r8 %r9, the
 * address in %ecx, and the callee is owed 32 bytes of shadow space; stack
 * arguments (the fifth on) stay where the caller put them because %rsp is
 * restored before the tail jump.  Neither saves %xmm0-3: PS1 code passes no
 * floating-point arguments. */
#ifdef _WIN32
__asm__(
    ".text\n"
    ".globl overlay_tramp\n"
    ".def overlay_tramp; .scl 2; .type 32; .endef\n"
    "overlay_tramp:\n"
    "  push %rcx\n  push %rdx\n  push %r8\n  push %r9\n"
    "  sub $40, %rsp\n"
    "  mov %eax, %ecx\n"
    "  call overlay_resolve\n"
    "  add $40, %rsp\n"
    "  pop %r9\n  pop %r8\n  pop %rdx\n  pop %rcx\n"
    "  jmp *%rax\n");
#else
__asm__(
    ".text\n"
    ".globl overlay_tramp\n"
    ".type overlay_tramp, @function\n"
    "overlay_tramp:\n"
    "  push %rdi\n  push %rsi\n  push %rdx\n  push %rcx\n  push %r8\n  push %r9\n"
    "  sub $8, %rsp\n"
    "  mov %eax, %edi\n"
    "  call overlay_resolve\n"
    "  add $8, %rsp\n"
    "  pop %r9\n  pop %r8\n  pop %rcx\n  pop %rdx\n  pop %rsi\n  pop %rdi\n"
    "  jmp *%rax\n");
#endif

/* A call through a raw PS1 overlay code address (a table word, a pointer
 * read from overlay data) jumps into the window, which is mapped without
 * execute permission, and faults with RIP == that address.  The return
 * address and the arguments are where the call left them, so moving RIP to
 * the native function is the call the console made.  Returns 1 if fixed. */
int hal_overlay_redirect_pc(unsigned long long *rip_reg) {
    u64 rip = (u64)*rip_reg;
    void *fn = NULL;
    /* the full PS1 RAM: the data's function pointers name either overlay
     * code (the window) or main-binary code (the registry map) */
    if (rip < 0x80000000u || rip >= 0x80200000u) return 0;
    fn = ps1_code_lookup((u32)rip);
    if (!fn) {
        /* An overlay code address with nothing loaded/resolvable: the boot's
         * transient state (game_system_init's default entry 0x80132000 before
         * the loader flow delivers an EXN).  Same treatment as the trampoline
         * path's overlay_unresolved: run the safe no-op rather than crash. */
        extern int hal_overlay_unresolved(void);
        fn = (void *)(uintptr_t)hal_overlay_unresolved;
    }
    *rip_reg = (unsigned long long)(uintptr_t)fn;
    return 1;
}

#ifndef _WIN32
/* the Linux SIGSEGV handler's entry (pc/src/main.c); Windows passes the
 * CONTEXT's Rip to hal_overlay_redirect_pc directly */
int hal_overlay_fault_redirect(void *ucv) {
    ucontext_t *uc = (ucontext_t *)ucv;
    return hal_overlay_redirect_pc((unsigned long long *)&uc->uc_mcontext.gregs[REG_RIP]);
}
#endif

/* Initialize the overlay system.
 * NO PC dispatch is installed: the matched PS1 flow IS the dispatch
 * (game_system_init sets game_mode_step = 0x80132000; scene_entry_set points
 * it at each scene's entry as the loader flow completes).  The PC only
 * provides the environment: the mapped window, the CD HAL delivering EXNs
 * into it, and the trampolines/fault-redirect resolving the PS1 addresses.
 * The old `game_mode_step = overlay_dispatch` override (a PC reinvention of
 * the dispatch) is gone per the no-PC-custom-stuff-unless-required rule;
 * pc/hal/overlay_dispatch.c is retained only as the retail scene->EXN
 * reference table it was re-derived from and can be deleted once nothing
 * reads it. */
void overlay_init(void) {
    printf("[HAL/Overlay] Environment ready (PS1 dispatch runs the scenes)\n");
}
