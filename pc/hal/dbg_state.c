/* SPDX-License-Identifier: GPL-3.0-or-later */
/* dbg_state.c -- save states for the PC port: the debug toolkit's `save` /
 * `load`, and under the player's quicksaves, the save browser, autosaves,
 * the resume state (pc/hal/quicksave.c) and rewind (pc/hal/rewind.c).
 *
 * What a state is: the emulated machine and the port's game-relevant
 * native state, named explicitly, at a main-loop boundary.
 *
 *   PRAM  PS1 RAM, 0x80000000, 2 MB: every game global the build links at
 *         its PS1 address (pc/ld/psx_globals.syms), the loaded overlay's EXN
 *         bytes and data, the game heap, the pad buffers.
 *   MMIO  the hardware-register window, 0x1F801000, 4 KB.
 *   NATV  one record per native data object of the PROJECT's own code --
 *         the matched and pc-port C, the overlays, the HAL device models
 *         (VRAM g_vram, the GPU/GTE/CD/XA/MDEC/SPU state, the SPU core
 *         core with its sound RAM, the memory card g_card, timers/IRQ, the
 *         system clock, the CPU-cost model, the pad device, the loaded
 *         overlay's identity) -- keyed by name (`file.c:static`, `global`),
 *         with its size and bytes.  Which objects are the project's comes
 *         from the build: pc/tools/gen_state_inventory.py lists the source
 *         files and global data symbols of every object the executable is
 *         linked from (pc/build/gen/state_inventory.c).  Everything else in
 *         the executable's writable sections -- libc, the C++ runtime, SDL and
 *         winpthreads where they are linked in statically -- is the host's
 *         and never saved or restored.
 *   STAK  the dead part of the game stack below the boundary (see below).
 *   INFO  text: format, build id, platform, frame, scene, overlay, sizes.
 *   MAPS  the host's memory map at the save (advisory, for the audit).
 *
 * What is NOT saved (KEEP rules below): project objects that hold host
 * resources or the player's session -- SDL handles, FILE pointers, heap
 * buffers made at init, thread ids, crash-recovery jump buffers, the
 * observers (trace, parity artifacts, debug HUD, this toolkit), the
 * settings, the profile, the quicksave/autosave/rewind bookkeeping, the
 * extras.  Bytes of a KEEP range inside a saved object are written as zero
 * and skipped by a load.  pc/tools/savestate_tool.py `audit` reports every
 * saved word that holds a host heap/stack/library pointer; the
 * `independent` run of pc/tools/savestate_roundtrip.py (two power-ons must
 * save the same state) is the check that decides.
 *
 * QUIESCE rules name pointers that hold a host heap buffer only while a
 * device operation is in flight (a timed SPU DMA's copy of its data, an
 * open movie stream).  A save waits for a boundary where they are NULL.
 *
 * The native stack.  The boundary is the top of pc/src/main.c's main loop:
 * game_main has returned, the frame's VBlank, CD worker and sound command
 * processor have run, and the toolkit runs on its own stack
 * (dbg_plat_run_on_toolkit_stack).  The live frames above the boundary are
 * main()'s and the loop's own, which the loading process re-creates by
 * being at the same boundary of the same executable.  Nothing on the stack
 * is a live game value -- but game code reads uninitialised locals (cd.c's
 * issue_raw copies 16 parameter bytes from 4-byte libcd arrays), so the
 * leftovers in the dead window below the boundary are inputs of later
 * frames.  STAK carries STACK_SNAP bytes of that window (minus STACK_GAP
 * right below the boundary, where the switch to the toolkit's stack keeps
 * live words) and a load writes them at the same distance below its own
 * boundary.  A state is only ever taken or loaded at that boundary.
 *
 * File format (little-endian): "TOKISTv2", then sections
 *   tag[4] u32 flags u64 addr u64 size u64 stored  <stored bytes>
 * flags bit 0: page-encoded (per 4 KB page one byte, 0 = all zero, 1 = the
 * page follows).  NATV's body: u32 count, then per record
 *   u16 key_len, key, u64 addr (the saver's), u64 size, u64 stored,
 *   <stored bytes: the size bytes, page-encoded>.
 *
 * A state loads only into the executable that wrote it (build id, which
 * `ld --build-id` writes on both platforms): native records hold host code
 * and data addresses (function-pointer tables, the PS1 RAM words the HAL
 * stores code addresses in), which differ between any two builds.  The
 * file is the same format on Linux and Windows.
 *
 * Determinism: with the audio mixed on the game thread (PC_PARITY_DIR) a
 * loaded state replays bit-exactly (pc/tools/savestate_roundtrip.py,
 * test_quicksave.py, test_rewind.py; on Windows under Wine with --runner).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "types.h"
#include "dbg_toolkit.h"
#include "dbg_platform.h"
#include "state_sign.h"
#include "pc_data_root.h"

extern s32 frame_counter;
extern u8 now_step, now_sub_step, now_sub_sub_step, now_girl;
extern u32 hal_clock_vblanks(void);
extern unsigned long long hal_clock_ticks(void);
extern const char *hal_overlay_loaded(void);
extern void spu_hal_audio_lock(void);
extern void spu_hal_audio_unlock(void);
extern int pc_ptr_is_mapped(const void *p, unsigned long bytes);

/* the build's inventory (pc/tools/gen_state_inventory.py) */
extern const char *const pc_state_inv_files[];
extern const int pc_state_inv_nfiles;
extern const char *const pc_state_inv_globals[];
extern const int pc_state_inv_nglobals;
extern const char *const pc_state_inv_global_files[];   /* parallel to _globals */

#define PRAM_BASE 0x80000000ul
#define PRAM_SIZE 0x200000ul
#define MMIO_BASE 0x1F801000ul
#define MMIO_SIZE 0x1000ul
#define PAGE 4096ul
/* The game stack below the main-loop boundary (pc/hal/dbg_cmds.c sets
 * dbg_boundary_sp): STACK_SNAP bytes, minus the STACK_GAP just below the
 * boundary, where the switch to the toolkit's stack keeps live words. */
#define STACK_SNAP (512ul << 10)
#define STACK_GAP  4096ul
extern unsigned long dbg_boundary_sp;

#define malloc(n) dbg_mem_alloc(n)
#define free(p) dbg_mem_free(p)
#define realloc(p, n) dbg_mem_realloc(p, n)

/* an output buffer the state file is assembled in */
typedef struct { unsigned char *p; unsigned long n, cap; int ok; } Out;
static void out_put(Out *o, const void *d, unsigned long n) {
    if (!o->ok) return;
    if (o->n + n > o->cap) {
        unsigned long cap = o->cap ? o->cap : (1ul << 20);
        while (cap < o->n + n) cap *= 2;
        o->p = realloc(o->p, cap);
        if (!o->p) { o->ok = 0; return; }
        o->cap = cap;
    }
    memcpy(o->p + o->n, d, n);
    o->n += n;
}

static unsigned long stack_lo(void) {
    return (dbg_boundary_sp & ~15ul) - STACK_SNAP;
}
static unsigned long stack_size(void) {
    unsigned long lo = stack_lo();
    if (!dbg_boundary_sp || !pc_ptr_is_mapped((const void *)lo, STACK_SNAP - STACK_GAP)) return 0;
    return STACK_SNAP - STACK_GAP;
}

/* ------------------------------------------------------------------ */
/* the executable's symbols                                            */
/* ------------------------------------------------------------------ */

static const DbgImgSym *g_syms;
static int g_nsyms;
static int g_img;               /* 0 not tried, 1 ok, -1 failed */

static int img_load(void) {
    if (!g_img) g_img = dbg_plat_image(&g_syms, &g_nsyms) ? 1 : -1;
    return g_img > 0;
}

/* `foo` matches `foo` and the function-static spelling `foo.3` */
static int name_eq(const char *sym, const char *want) {
    size_t n = strlen(want);
    if (strncmp(sym, want, n)) return 0;
    return sym[n] == 0 || (sym[n] == '.' && sym[n + 1] >= '0' && sym[n + 1] <= '9');
}

void *dbg_sym_lookup(const char *name, unsigned long *size) {
    const char *colon = strchr(name, ':');
    char file[128];
    int i;
    if (size) *size = 0;
    if (!img_load()) return NULL;
    file[0] = 0;
    if (colon && (size_t)(colon - name) < sizeof file) {
        memcpy(file, name, (size_t)(colon - name));
        file[colon - name] = 0;
        name = colon + 1;
    }
    /* globals first (a static of the same name in some file loses) */
    if (!file[0]) {
        for (i = 0; i < g_nsyms; i++)
            if (!g_syms[i].file && !strcmp(g_syms[i].name, name)) {
                if (size) *size = g_syms[i].size;
                return (void *)g_syms[i].addr;
            }
    }
    for (i = 0; i < g_nsyms; i++) {
        if (!g_syms[i].file || !name_eq(g_syms[i].name, name)) continue;
        if (file[0] && strcmp(g_syms[i].file, file)) continue;
        if (size) *size = g_syms[i].size;
        return (void *)g_syms[i].addr;
    }
    return NULL;
}

const char *dbg_sym_name(const void *addr, char *buf, size_t cap) {
    unsigned long a = (unsigned long)addr;
    int lo = 0, hi, best = -1;
    if (!img_load() || !g_nsyms) return NULL;
    hi = g_nsyms - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_syms[mid].addr <= a) { best = mid; lo = mid + 1; } else hi = mid - 1;
    }
    /* the nearest symbol at or below that actually contains a */
    for (; best >= 0 && best > (lo - 64); best--) {
        const DbgImgSym *s = &g_syms[best];
        if (a >= s->addr && a < s->addr + (s->size ? s->size : 1)) {
            if (s->file) snprintf(buf, cap, "%s:%s+0x%lx", s->file, s->name, a - s->addr);
            else snprintf(buf, cap, "%s+0x%lx", s->name, a - s->addr);
            return buf;
        }
    }
    return NULL;
}

int dbg_addr_ok(const void *addr, unsigned long bytes) {
    unsigned long a = (unsigned long)addr, r[8][2];
    int i, n;
    if (a + bytes < a) return 0;
    if (a >= PRAM_BASE && a + bytes <= PRAM_BASE + PRAM_SIZE) return 1;
    if (a >= MMIO_BASE && a + bytes <= MMIO_BASE + MMIO_SIZE) return 1;
    n = dbg_plat_data_ranges(r, 8);
    for (i = 0; i < n; i++)
        if (a >= r[i][0] && a + bytes <= r[i][1]) return 1;
    return 0;
}

const char *dbg_state_build_id(void) {
    return dbg_plat_build_id();
}

/* ------------------------------------------------------------------ */
/* rules                                                               */
/* ------------------------------------------------------------------ */

/* Whole source files whose statics belong to the process, not the
 * machine: observers, the host HUD/test screens, the PC-only music players
 * (not retail paths), this toolkit, and the PC entry point (argv,
 * crash-recovery jmp_buf, the low game stack). */
static const char *const KEEP_FILES[] = {
    "main.c", "trace.c", "debug.c", "instrument.c", "nbn_trace.c",
    "watchdog.c", "gfx_test.c", "audio_soundtest.c", "bios_printf.c",
    "audio_scene.c",
    "dbg_state.c", "dbg_cmds.c", "dbg_console.c", "pc_debug.c",
    /* the toolkit's host layer: symbol tables, its own stack, sockets
     * (pc/hal/dbg_platform.h) */
    "dbg_platform_posix.c", "win_dbg_platform.c",
    /* the Windows shims: the COFF symbol table dladdr reads, the malloc
     * placement check, the POSIX compatibility layer (pc/hal/win) */
    "win_dl.c", "win_heap.c", "win_compat.c",
    /* the memory card's file writer (pc/hal/card_writer.c): a thread and
     * the image it is writing, the host's side of a card write */
    "card_writer.c",
    /* where the player's card and quicksaves live (host configuration) */
    "save_paths.c",
    /* where the game data and the data root are, the player log, the
     * first-run setup and the menu's verify job (host configuration) */
    "data_root.c", "assets.c", "setup_screen.c", "asset_setup.c", "pc_fs.c",
    /* the settings menu's pages and items (host function pointers, the
     * open page) and the save browser / autosave / resume bookkeeping */
    "menu.c", "save_browser.c", "save_auto.c", "assist_menu.c",
    /* the player's session, not the machine: the quicksave slot selection
     * and requests; the presenter (window handles, toasts, pause, remembered
     * geometry, the PNG writer's CRC cache); the settings as this session loaded and changed them; the
     * run profile and its switch values (a state saved under one profile
     * loads into a session of the other without changing it); the enhanced
     * input assists (fast-forward key, auto-advance), host input like
     * pad.c's.  A state from another session must not carry any of them in
     * (pc/tools/test_quicksave.py checks the profile). */
    "quicksave.c", "present.c", "present_layout.c", "settings.c", "profile.c",
    "enhance.c",
    /* the UI text's draw batch (the presenter's scratch, pc/hal/ui_draw.c) */
    "ui_draw.c",
    /* music packs: the open files, the decoders, the dump and the event log
     * (host resources); the song state is re-read from libsnd every VBlank */
    "music_pack.c", "music_decode.c",
    /* SoundFont mode (the loaded SF2 and its TSF instances on the heap, the
     * notes it plays, the map) and the enhanced sound settings and the
     * 48 kHz resampler (pc/hal/soundfont.c, spu_quality.c,
     * audio_resample.c): the session's; a load re-derives the notes from
     * the next key-ons */
    "soundfont.c", "spu_quality.c", "audio_resample.c",
    /* the dialogue backlog's list, its view and the voice replay (a heap
     * buffer the audio thread plays): the session's, like the presenter.
     * Its capture (backlog_capture.c) watches the machine and is
     * restored with it. */
    "backlog.c", "backlog_view.c", "voice_replay.c",
    /* the rewind buffer (its snapshots, the worker) and the set of dialogue
     * pages read in any session: the player's, never the machine's */
    "rewind.c", "seen_text.c",
    /* the extras (endings seen, achievements, the gallery's record and
     * what it is watching, the disc-read ring it names pictures by): the
     * player's, never a state's -- a load must not forget an unlock */
    "extras.c", "extras_state.c", "extras_gallery.c", "extras_view.c",
    /* Host resources that were saved and restored until 2026-11-03
     * (docs/pc-port/STATE-MACHINES.md, "Save-state ownership"): a state
     * loaded into another process wrote that process's heap pointers back.
     *  - the file writer (pc/hal/async_io.c): its mutex, condition
     *    variables, thread and queue -- a load in a new session left the
     *    next texture dump / screenshot / gallery picture locking a dead
     *    mutex (hang, then the watchdog's crash loop);
     *  - texture dumping and packs (pc/hal/gpu_texrep.c): heap tables,
     *    the index FILE*; its per-texel owner check (g_oval) already
     *    re-validates against the restored VRAM;
     *  - the host input configuration and pointer state (pc/hal/pad.c,
     *    all of it: key bindings, mouse mode, cursor, steering, the text
     *    assist's mouse pulse -- the session's, from settings.ini);
     *  - the status page's view state and its page id (pc/hal/status_view.c). */
    "async_io.c", "gpu_texrep.c", "pad.c", "status_view.c",
    /* Found by StateOwnershipIsListed (2026-11-03): host-side files that
     * were machine state only by default -- the CRT filter's 1.2 MB scratch
     * accumulator (pc/hal/present_crt.c, saved in every state and rewind
     * snapshot), the asset manifest as loaded at start-up (asset_map.c,
     * configuration like assets.c), and two observers: the visual sweep's
     * screenshot writer (sweep_shot.c) and the English text audit's log
     * (text_audit.c), which must not forget or repeat what they wrote
     * because the machine was loaded. */
    "present_crt.c", "asset_map.c", "sweep_shot.c", "text_audit.c",
    /* The CONTROLS page (pc/hal/controls_view.c, 2026-11-06): a pc_menu
     * page like the settings menu's -- its page id, the legend read from
     * the host keymaps, its layout and scroll, and the one-time F2 hint's
     * "shown" flag.  Host UI and the install's hint record: a load must not
     * re-show the hint or bring back another session's bindings. */
    "controls_view.c",
    /* The GUIDE pages (pc/hal/guide_*.c, 2026-11-03): the view's page,
     * tab, scroll and layout, the document on the heap (guide_model.c D,
     * a host pointer), and the rule tables read once from the player's
     * disc files (guide_rules*.c: EXN tables, never PS1 RAM).  Reference
     * UI and a cache of constant disc data; nothing the game reads. */
    "guide_view.c", "guide_model.c", "guide_rules.c", "guide_rules_dates.c",
    /* The save-state signing key (pc/hal/state_sign.c, 2026-11-13): the
     * install's secret.  A state that held it would hand anyone it is
     * shared with the key to forge states; never saved, never restored. */
    "state_sign.c",
    /* The list of the files the embedded translation pack makes
     * (pc/hal/translation.c): a cache of constant pack data, host side. */
    "translation.c",
    NULL
};

/* The other side of KEEP_FILES: PC-layer source files (pc/hal) whose data
 * a state saves ON PURPOSE -- device models, the machine's clock, the
 * game-visible parts of the PC features.  Every pc/hal file with data in
 * a state must be named here or kept out (KEEP_FILES, or each of its
 * objects in KEEP_SYMS): pc/tools/test_state_machines.py
 * StateOwnershipIsListed fails on a new module that is in neither list, so
 * "machine state by default" is never an accident (STATE-MACHINES.md R1).
 * Listed only, never read by the save itself (the inventory dump prints
 * it). */
static const char *const MACHINE_FILES[] = {
    /* the CPU, its clock and interrupts: the system clock and VSync, the
     * CPU-cost model, the VBlank IRQ and the SDK's handlers */
    "system.c", "cpu_cost.c", "irq.c", "sdk_irq.c",
    /* the GPU and GTE device models (VRAM, the draw state, the command
     * queue, ResetGraph, the clear diagnostics hook, PGXP's per-vertex
     * cache of the restored geometry) */
    "gpu.c", "gpu_queue.c", "gpu_reset.c", "gpu_clear.c", "gpu_pgxp.c", "gte.c",
    /* the CD drive, XA, the MDEC / movie decoder */
    "cd.c", "cd_audio.c", "libpress.c",
    /* the SPU, its DMA and reset, the PC SEQ player's register hook */
    "spu.c", "spu_transfer.c", "spu_reset.c", "seq_spu.c",
    /* the memory card (a load keeps the live image: quicksave.c card_hold),
     * the pad device the game polls */
    "memcard.c", "pad_device.c",
    /* the loaded overlay and the scene dispatch's memory of it */
    "overlay.c", "overlay_dispatch.c",
    /* the text language the game's memory holds (section 2.1) */
    "lang.c",
    /* PC features that watch the machine and are restored with it: the
     * backlog's capture, quick text (STATE-MACHINES.md 3.7, 3.9) */
    "backlog_capture.c", "quick_text.c",
    /* the parity artifacts' audio-rate accumulator (the rest is KEEP_SYMS) */
    "parity.c",
    NULL
};

/* Single statics/globals: "file.c:name" (a static; `name` also matches the
 * function-static spelling name.N), "name" (a global), optionally
 * "+OFF:LEN" for a byte range inside it.  Each group says what the host
 * resource is.  pc/tools/savestate_tool.py audit finds new ones. */
static const char *const KEEP_SYMS[] = {
    /* pc/src/main.c globals: crash bookkeeping */
    "g_crash_count", "g_last_fault_addr", "g_last_fault_pc",
    /* pc/src/main.c (Windows): the game thread's id the watchdog and the
     * fault handler compare with */
    "g_game_thread_id",
    /* pc/include/pc_debug.h flags (PC_DEBUG, the console's `debug`) */
    "g_pc_debug_flags", "g_pc_debug_initialized",
    /* pc/hal/system.c: SDL window/renderer, quit flag, asset dir, the
     * host's wall-clock pacing (the emulated VSync clock is restored) */
    "system.c:g_asset_dir", "system.c:g_window", "system.c:g_renderer",
    "system.c:g_running", "system.c:g_frame_start", "system.c:g_frame_start_tick",
    "system.c:g_pacing_remainder", "system.c:g_headless_mode", "system.c:remainder",
    "system.c:g_pace_resync",
    /* TOKI_TEST_QUIT_FRAMES (a test's run length, host side) */
    "system.c:quit_at", "system.c:frames",
    /* pc/hal/parity.c: the artifact writer (its audio-rate accumulator
     * g_remainder IS machine state and is restored) */
    "parity.c:g_initialized", "parity.c:g_enabled", "parity.c:g_anchored",
    "parity.c:g_done", "parity.c:g_points", "parity.c:g_count", "parity.c:g_next",
    "parity.c:g_offset", "parity.c:g_samples", "parity.c:g_dir", "parity.c:g_pcm",
    "parity.c:g_state",
    /* pc/hal/spu.c: SDL audio device, WAV dump file, and the PC-only SEQ /
     * XA file players (sound test, PC_SCENE_MUSIC; not retail paths) */
    "spu.c:g_audio_dev", "spu.c:g_audio_initialized", "spu.c:g_wav_file",
    "spu.c:g_wav_samples", "spu.c:g_seq", "spu.c:g_bank", "spu.c:g_seq_volume",
    "spu.c:g_xa_stream", "spu.c:g_xa_ring", "spu.c:g_xa_ring_head",
    "spu.c:g_xa_ring_tail", "spu.c:g_xa_loop", "spu.c:g_xa_active",
    "spu.c:g_tsf", "spu.c:g_current_sf2",
    /* the SPU core's output streams (unique_ptr<AudioStream>
     * audio_stream / null_audio_stream, upstream/spu.cpp SPUState; the
     * vendored source is unmodified, so the offset is fixed) and the
     * shim's host sink */
    "spu.cpp:_ZN3SPUL7s_stateE+0x1230:16", "spu_shim.cpp:_ZL6g_sink",
    /* the core's music split (README "PC patches"): set by music_pack.c,
     * which is the session's; its shadow RAM is a heap block */
    "spu.cpp:_ZN3SPUL10s_pc_splitE",
    /* the core's enhanced-quality patch: the settings, the injection
     * pointer and the floating-point reverb unit (its work area is a heap
     * block), and the sinc tables (a cache); s_pc_hist, the voices' sample
     * history, is machine state and saved */
    "spu.cpp:_ZN3SPUL12s_pc_qualityE", "spu.cpp:_ZN3SPUL9s_pc_sincE",
    "spu.cpp:_ZN3SPUL15s_pc_sinc_readyE",
    /* pc/hal/cd.c: the open sector-source file and which file it is (a
     * cache: every read seeks; a mismatch reopens) */
    "cd.c:g_sector_file", "cd.c:g_sector_path", "cd.c:g_sector_file_lba",
    "cd.c:g_sector_file_nsec", "cd.c:g_sector_file_size",
    /* pc/hal/cpu_cost.c: caches keyed by host code addresses (dladdr
     * names), the main thread id, env configuration */
    "cpu_cost.c:g_slots", "cpu_cost.c:g_callers", "cpu_cost.c:g_main_thread",
    "cpu_cost.c:g_profile", "cpu_cost.c:g_trace_names", "cpu_cost.c:g_armed",
    "cpu_cost.c:g_seq_armed", "cpu_cost.c:g_ovl_ctx",
    /* pc/hal/gpu.c: SDL textures, env configuration, diagnostic counters */
    "gpu.c:g_display_tex", "gpu.c:vram_tex", "gpu.c:g_use_mdfn_gpu",
    "gpu.c:g_gp0_trace", "gpu.c:g_gp0_hist", "gpu.c:g_gp0_unhandled",
    "gpu.c:g_gp0_incomplete", "gpu.c:g_gp0_rect_oversize", "gpu.c:g_gp0_e1_flip",
    "g_blend_abr_hist", "gpu.c:g_setaddr_count", "gpu.c:g_addprim_count",
    "g_ot_setaddr_calls", "gpu.c:g_gp0_frame_dump",
    /* the internal-resolution shadow (pc/hal/gpu_hires.c): its scale and
     * heap buffers, the N x display texture and picture (dbg_state_after_restore
     * resyncs the shadow against the restored VRAM); the texture packs'
     * and the extras gallery's hook switches (session decisions) */
    "g_hires", "gpu.c:hires_tex", "gpu.c:hires_w", "gpu.c:hires_h",
    "gpu_hires.c:pixels", "gpu_hires.c:pixels_scale",
    "g_texrep_on", "g_texrep_replace", "g_extras_gpu",
    /* pc/hal/libpress.c: decoded-image cache (malloc'd frames, BS still
     * streams) keyed by the game's buffer address */
    "libpress.c:g_dec", "libpress.c:g_dec_rr", "libpress.c:g_cur",
    "libpress.c:g_stream_path",
    /* pc/hal/mdec_native.c: the decoder's tables, computed once from their
     * definitions (constants: the process's, not the machine's) */
    "mdec_native.c:g_ac", "mdec_native.c:g_ac_ready",
    "mdec_native.c:g_zigzag", "mdec_native.c:g_quant",
    "mdec_native.c:g_scale", "mdec_native.c:g_tables_ready",
    /* pc/hal/pad.c is host input as a whole (KEEP_FILES); the PS1-side pad
     * device state (pad_device.c, pad buffers in PS1 RAM) is restored,
     * except its configuration (the session's) */
    "pad_device.c:g_cfg",
    /* pad_device.c: what the host's hands are doing now -- the mouse
     * buttons held and the hold-off a closed menu / backlog / rewind armed
     * (a load from the save browser must not hand the held confirm key to
     * the loaded game) */
    "pad_device.c:g_mouse_left", "pad_device.c:g_mouse_right", "pad_device.c:g_hold_off",
    /* spu.c: the device hold (the rewind / pause loop holds it while a
     * restore writes the machine) */
    "spu.c:g_held",
    /* pc/hal/spu.c: the player's volume gains ([audio], Ctrl+= / Ctrl+-) */
    "spu.c:g_gain_spu", "spu.c:g_gain_cd", "spu.c:g_gain_sfx",
    /* pc/hal/spu.c: the real-time device's register queue and its host
     * clock (a load's clock jump flushes it), the audio-health counters,
     * the dump rings / files and their buffers, env configuration */
    "spu.c:g_rt", "spu.c:g_hl", "spu.c:g_realtime_tag", "spu.c:g_tap_out",
    "spu.c:g_tap_hash", "spu.c:g_hash_log", "spu.c:g_raw_dump", "spu.c:chunk",
    "spu.c:last_ms", "spu.c:last_sig", "spu.c:done", "spu.c:ms",
    NULL
};

/* Pointers that hold a host heap buffer only while an operation is in
 * flight; a save waits for a boundary where each is NULL.  Each names the
 * pointer member itself (+OFF:8), so the test is a NULL check -- never a
 * value-range guess, which ASLR would make vary between runs. */
static const char *const QUIESCE_SYMS[] = {
    "spu_transfer.c:g_transfer+0:8",   /* SpuTransfer.data: a timed SPU DMA's copy */
    "libpress.c:g_frames+0:8",         /* a movie stream is open */
    "libpress.c:g_sector_audio+0:8",
    NULL
};

typedef struct { unsigned long lo, hi; } Range;

/* the index of S in a sorted list, -1 when absent */
static int str_find(const char *const *sorted, int n, const char *s) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(sorted[mid], s);
        if (!c) return mid;
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

static int file_kept(const char *file) {
    int i;
    for (i = 0; KEEP_FILES[i]; i++)
        if (!strcmp(file, KEEP_FILES[i])) return 1;
    return 0;
}

/* "file:name+OFF:LEN" -> does it name s?  *off / *len get the byte range
 * (0 / the symbol size when none is given) */
static int rule_matches_range(const char *rule, const DbgImgSym *s,
                              unsigned long *off, unsigned long *len) {
    char buf[160], *colon, *plus;
    snprintf(buf, sizeof buf, "%s", rule);
    *off = 0;
    *len = s->size ? s->size : 8;
    plus = strchr(buf, '+');
    if (plus) {
        char *l = strchr(plus, ':');
        *plus = 0;
        *off = strtoul(plus + 1, NULL, 0);
        *len = l ? strtoul(l + 1, NULL, 0) : 8;
    }
    colon = strchr(buf, ':');
    if (!colon) return !s->file && !strcmp(s->name, buf);
    *colon = 0;
    if (!s->file || strcmp(s->file, buf)) return 0;
    return name_eq(s->name, colon + 1);
}

/* ---- the records: what a state holds of the native data ----
 * Built once per process from the symbol table, the inventory and the
 * rules; read-only afterwards (the writer thread reads it). */
enum { CL_SAVED, CL_PARTIAL, CL_KEEP_FILE, CL_KEEP_SYM, CL_FOREIGN, CL_COVERED };
typedef struct {
    unsigned long addr, size;
    char *key;                  /* `file.c:name` / `name`, `#N` for the N-th of a key */
    int sym;                    /* index into g_syms */
    int nkeep;                  /* KEEP pieces inside (offsets into the record) */
    Range *keep;
    unsigned long off;          /* offset in a snapshot's packed buffer */
} Rec;

static Rec *g_recs;             /* by address */
static int g_nrecs;
static int *g_by_key;           /* indexes into g_recs, by key */
static unsigned long g_natv_bytes;
static unsigned char *g_class;  /* per symbol: CL_* (the inventory dump) */
static int g_built;             /* 0 not tried, 1 ok, -1 failed */
static char g_unavail[256];

static int key_cmp(const void *a, const void *b) {
    return strcmp(g_recs[*(const int *)a].key, g_recs[*(const int *)b].key);
}

static char *dup_str(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

/* the bytes of symbol s a state keeps out: whole (1), some (*pieces), none */
static int keep_pieces(const DbgImgSym *s, unsigned long size, Range *pieces, int cap, int *n) {
    int k;
    *n = 0;
    for (k = 0; KEEP_SYMS[k]; k++) {
        unsigned long off, len;
        if (!rule_matches_range(KEEP_SYMS[k], s, &off, &len)) continue;
        if (off == 0 && len >= size) return 1;
        if (off >= size) continue;
        if (off + len > size) len = size - off;
        if (*n < cap) { pieces[*n].lo = off; pieces[*n].hi = off + len; (*n)++; }
    }
    return 0;
}

/* the end of the writable section holding ADDR (0 when none) */
static unsigned long data_end(unsigned long addr) {
    unsigned long r[8][2];
    int i, n = dbg_plat_data_ranges(r, 8);
    for (i = 0; i < n; i++)
        if (addr >= r[i][0] && addr < r[i][1]) return r[i][1];
    return 0;
}

static int build_records(void) {
    unsigned long covered = 0;
    int i, cap;
    if (g_built) return g_built > 0;
    g_built = -1;
    if (!img_load()) {
        snprintf(g_unavail, sizeof g_unavail, "cannot read the executable's symbols: %s",
                 dbg_plat_image_error());
        return 0;
    }
    if (pc_state_inv_nfiles <= 0) {
        snprintf(g_unavail, sizeof g_unavail, "the build has no state inventory");
        return 0;
    }
    cap = g_nsyms;
    g_recs = malloc((unsigned long)(cap ? cap : 1) * sizeof *g_recs);
    g_class = malloc((unsigned long)(cap ? cap : 1));
    if (!g_recs || !g_class) { snprintf(g_unavail, sizeof g_unavail, "out of memory"); return 0; }
    memset(g_class, CL_FOREIGN, (size_t)(cap ? cap : 1));
    for (i = 0; i < g_nsyms; i++) {
        const DbgImgSym *s = &g_syms[i];
        unsigned long size = s->size;
        Range pieces[8];
        int np, project, gi = -1;
        char key[300];
        Rec *r;
        unsigned long hi;
        if (!s->data || !(hi = data_end(s->addr))) continue;
        if (s->file) project = str_find(pc_state_inv_files, pc_state_inv_nfiles, s->file) >= 0;
        else project = (gi = str_find(pc_state_inv_globals, pc_state_inv_nglobals, s->name)) >= 0;
        if (!project) continue;
        if (!size) {
            /* an assembler label without .size: up to the next symbol */
            int j = i + 1;
            while (j < g_nsyms && g_syms[j].addr == s->addr) j++;
            size = j < g_nsyms && g_syms[j].addr < hi ? g_syms[j].addr - s->addr : 0;
            if (!size) { g_class[i] = CL_COVERED; continue; }
        }
        if (s->addr + size > hi) size = hi - s->addr;
        /* an alias of an object already recorded (the linker-script
         * PROVIDE views, a C++ alias): the bytes are in that record */
        if (s->addr + size <= covered) { g_class[i] = CL_COVERED; continue; }
        if (s->addr < covered) {
            fprintf(stderr, "[DBG] state: %s%s%s overlaps the previous object; its first %lu "
                    "bytes are in that record\n", s->file ? s->file : "", s->file ? ":" : "",
                    s->name, covered - s->addr);
            size -= covered - s->addr;
        }
        /* a static of a KEEP file, or a global defined in one */
        if ((s->file && file_kept(s->file)) || (gi >= 0 && file_kept(pc_state_inv_global_files[gi]))) {
            g_class[i] = CL_KEEP_FILE;
            continue;
        }
        if (keep_pieces(s, size, pieces, 8, &np)) { g_class[i] = CL_KEEP_SYM; continue; }
        g_class[i] = np ? CL_PARTIAL : CL_SAVED;
        if (s->file) snprintf(key, sizeof key, "%s:%s", s->file, s->name);
        else snprintf(key, sizeof key, "%s", s->name);
        r = &g_recs[g_nrecs++];
        memset(r, 0, sizeof *r);
        r->addr = s->addr + (s->addr < covered ? covered - s->addr : 0);
        r->size = size;
        r->sym = i;
        r->key = dup_str(key);
        if (np) {
            r->keep = malloc((unsigned long)np * sizeof *r->keep);
            if (r->keep) { memcpy(r->keep, pieces, (size_t)np * sizeof *pieces); r->nkeep = np; }
        }
        r->off = g_natv_bytes;
        g_natv_bytes += size;
        covered = r->addr + size;
    }
    /* keys: the N-th object of one key (a static of two files that share a
     * basename) gets `#N`, in address order */
    g_by_key = malloc((unsigned long)(g_nrecs ? g_nrecs : 1) * sizeof *g_by_key);
    if (!g_by_key) { snprintf(g_unavail, sizeof g_unavail, "out of memory"); return 0; }
    for (i = 0; i < g_nrecs; i++) g_by_key[i] = i;
    qsort(g_by_key, (size_t)g_nrecs, sizeof *g_by_key, key_cmp);
    for (i = 1; i < g_nrecs; i++) {
        int k = i, n = 2;
        while (k < g_nrecs && !strcmp(g_recs[g_by_key[k]].key, g_recs[g_by_key[i - 1]].key)) k++;
        if (k == i) continue;
        /* g_by_key[i-1 .. k-1] share a key; qsort's order among them is not
         * defined, so number them by address */
        {
            int a, b;
            for (a = i - 1; a < k; a++)
                for (b = a + 1; b < k; b++)
                    if (g_recs[g_by_key[b]].addr < g_recs[g_by_key[a]].addr) {
                        int t = g_by_key[a]; g_by_key[a] = g_by_key[b]; g_by_key[b] = t;
                    }
            for (a = i; a < k; a++) {
                Rec *r = &g_recs[g_by_key[a]];
                char key[320];
                snprintf(key, sizeof key, "%s#%d", r->key, n++);
                free(r->key);
                r->key = dup_str(key);
            }
        }
        i = k;
    }
    qsort(g_by_key, (size_t)g_nrecs, sizeof *g_by_key, key_cmp);
    g_built = 1;
    return 1;
}

static int rec_find(const char *key) {
    int lo = 0, hi = g_nrecs - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(g_recs[g_by_key[mid]].key, key);
        if (!c) return g_by_key[mid];
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

const char *dbg_state_unavailable(void) {
    return build_records() ? NULL : g_unavail;
}

/* NULL when every QUIESCE pointer is NULL, else the rule that is busy.
 * The pointers' addresses are resolved once (a scan of every symbol per
 * rule costs more than a millisecond, and a save's capture is on the
 * frame). */
#define MAX_QUIESCE 16
static unsigned long g_quiesce_addr[MAX_QUIESCE];
static const char *g_quiesce_rule[MAX_QUIESCE];
static int g_nquiesce = -1;

static const char *busy_static(void) {
    int i, k;
    if (g_nquiesce < 0) {
        g_nquiesce = 0;
        for (i = 0; i < g_nsyms; i++) {
            const DbgImgSym *s = &g_syms[i];
            for (k = 0; QUIESCE_SYMS[k]; k++) {
                unsigned long off, len;
                if (!rule_matches_range(QUIESCE_SYMS[k], s, &off, &len) || len != 8 ||
                    off + 8 > (s->size ? s->size : 8) || g_nquiesce == MAX_QUIESCE)
                    continue;
                g_quiesce_addr[g_nquiesce] = s->addr + off;
                g_quiesce_rule[g_nquiesce++] = QUIESCE_SYMS[k];
            }
        }
    }
    for (i = 0; i < g_nquiesce; i++) {
        unsigned long long v = 0;
        memcpy(&v, (const void *)g_quiesce_addr[i], 8);
        if (v) return g_quiesce_rule[i];
    }
    return NULL;
}

/* PC_STATE_INVENTORY=PATH: the classification of every data symbol of the
 * executable's writable sections, one line each (tab-separated: class,
 * key, address, size, source file -- a global's from the build's
 * inventory, "" when not the project's), then the bytes of the writable
 * sections no symbol covers; and one `# machine-file NAME` line per
 * MACHINE_FILES entry.  pc/tools/test_dbg_toolkit_platforms.py and
 * test_state_machines.py (StateOwnershipIsListed) read it. */
static void inventory_dump(const char *path) {
    static const char *const cl[] = { "saved", "partial", "keep-file", "keep-sym", "foreign",
                                      "alias" };
    Out o;
    unsigned long r[8][2], gap = 0, gap_nz = 0, a;
    unsigned int gap_hash = 2166136261u;
    int i, j, nr = dbg_plat_data_ranges(r, 8);
    char line[600];
    memset(&o, 0, sizeof o);
    o.ok = 1;
    snprintf(line, sizeof line, "# platform %s build %s records %d natv_bytes %lu\n",
             dbg_plat_name(), dbg_plat_build_id(), g_nrecs, g_natv_bytes);
    out_put(&o, line, strlen(line));
    for (j = 0; j < nr; j++) {
        snprintf(line, sizeof line, "# section 0x%lx-0x%lx\n", r[j][0], r[j][1]);
        out_put(&o, line, strlen(line));
    }
    for (j = 0; MACHINE_FILES[j]; j++) {
        snprintf(line, sizeof line, "# machine-file %s\n", MACHINE_FILES[j]);
        out_put(&o, line, strlen(line));
    }
    for (j = 0; j < nr; j++) {
        unsigned long end = r[j][0];
        for (i = 0; i < g_nsyms; i++) {
            const DbgImgSym *s = &g_syms[i];
            int n;
            if (s->addr < r[j][0] || s->addr >= r[j][1]) continue;
            int gi = s->file ? -1 : str_find(pc_state_inv_globals, pc_state_inv_nglobals, s->name);
            n = snprintf(line, sizeof line, "%s\t%s%s%s\t0x%lx\t%lu\t%s\n", cl[g_class[i]],
                         s->file ? s->file : "", s->file ? ":" : "", s->name, s->addr, s->size,
                         s->file ? s->file : gi >= 0 ? pc_state_inv_global_files[gi] : "");
            out_put(&o, line, (unsigned long)n);
            /* bytes no symbol names (alignment padding, foreign blobs) */
            for (a = end; a < s->addr; a++) {
                unsigned char b = *(const unsigned char *)a;
                gap++;
                gap_nz += b != 0;
                gap_hash = (gap_hash ^ b) * 16777619u;
            }
            if (s->addr + s->size > end) end = s->addr + s->size;
        }
        for (a = end; a < r[j][1]; a++) {
            unsigned char b = *(const unsigned char *)a;
            gap++;
            gap_nz += b != 0;
            gap_hash = (gap_hash ^ b) * 16777619u;
        }
    }
    /* the bytes no symbol names: their content hash, which stays the same
     * from moment to moment when nothing live is there */
    snprintf(line, sizeof line, "# unnamed_bytes %lu nonzero %lu hash %08x\n", gap, gap_nz, gap_hash);
    out_put(&o, line, strlen(line));
    if (o.ok) dbg_write_file(path, o.p, o.n);
    free(o.p);
    printf("[DBG] PC_STATE_INVENTORY %s: %d records, %lu bytes saved\n", path, g_nrecs, g_natv_bytes);
}

/* ------------------------------------------------------------------ */
/* file I/O                                                            */
/* ------------------------------------------------------------------ */

#define FLAG_PAGED 1u

static unsigned long paged_size(const unsigned char *data, unsigned long size) {
    unsigned long stored = 0, off;
    for (off = 0; off < size; off += PAGE) {
        unsigned long len = size - off < PAGE ? size - off : PAGE, j;
        int zero = 1;
        for (j = 0; j < len && zero; j++) zero = data[off + j] == 0;
        stored += 1 + (zero ? 0 : len);
    }
    return stored;
}

static void put_paged(Out *o, const unsigned char *data, unsigned long size) {
    unsigned long off;
    for (off = 0; off < size; off += PAGE) {
        unsigned long len = size - off < PAGE ? size - off : PAGE, j;
        int zero = 1;
        unsigned char tagb;
        for (j = 0; j < len && zero; j++) zero = data[off + j] == 0;
        tagb = zero ? 0 : 1;
        out_put(o, &tagb, 1);
        if (!zero) out_put(o, data + off, len);
    }
}

static void section_head(Out *o, const char *tag, unsigned flags, unsigned long addr,
                         unsigned long size, unsigned long stored) {
    u32 fl = flags;
    unsigned long long a = addr, z = size, st = stored;
    out_put(o, tag, 4);
    out_put(o, &fl, 4);
    out_put(o, &a, 8);
    out_put(o, &z, 8);
    out_put(o, &st, 8);
}

static void write_section(Out *o, const char *tag, unsigned flags, unsigned long addr,
                          const unsigned char *data, unsigned long size) {
    section_head(o, tag, flags, addr, size, (flags & FLAG_PAGED) ? paged_size(data, size) : size);
    if (flags & FLAG_PAGED) put_paged(o, data, size);
    else out_put(o, data, size);
}

/* A state in two halves (pc/hal/quicksave.c writes the second one on a
 * worker thread so a save does not stall the frame):
 *   dbg_state_capture  at the boundary: copies of PS1 RAM, the register
 *                      window, the native records and the game stack, and
 *                      the INFO text -- all of the machine the file holds,
 *                      so the machine may run on at once;
 *   dbg_state_write    anywhere, any thread: encodes the copies, writes
 *                      PATH.tmp and replaces PATH with it, frees the copy.
 * Only dbg_mem buffers and the platform's file calls are used, which are
 * safe off the game thread. */
struct DbgStateSnap {
    unsigned long stlo, stsz, maps_len, natv_bytes;
    unsigned char *natv, *ram, *mmio, *stak;
    char *maps;
    char info[1536];
    char msg[256];
};

/* One finished snapshot is kept as a spare: its buffers are already
 * faulted in, so the next capture's copy runs at memory speed (measured:
 * 7 ms into fresh pages).
 * The writer hands it back from its thread. */
static DbgStateSnap *g_spare;

static void snap_release(DbgStateSnap *s) {
    free(s->natv); free(s->ram); free(s->mmio); free(s->stak); free(s->maps);
    free(s);
}

void dbg_state_snap_free(DbgStateSnap *s) {
    if (!s) return;
    free(s->maps);
    s->maps = NULL;
    s = __atomic_exchange_n(&g_spare, s, __ATOMIC_ACQ_REL);
    if (s) snap_release(s);
}

/* A snapshot with buffers of the sizes a capture needs now: the spare
 * when there is one (buffers that do not fit are replaced). */
static DbgStateSnap *snap_alloc(void) {
    unsigned long stsz = stack_size();
    DbgStateSnap *s = __atomic_exchange_n(&g_spare, NULL, __ATOMIC_ACQ_REL);
    if (s && s->natv_bytes != g_natv_bytes) { snap_release(s); s = NULL; }
    if (!s) {
        s = malloc(sizeof *s);
        if (!s) return NULL;
        memset(s, 0, sizeof *s);
        s->natv_bytes = g_natv_bytes;
        s->natv = malloc(g_natv_bytes ? g_natv_bytes : 1);
        s->ram = malloc(PRAM_SIZE);
        s->mmio = malloc(MMIO_SIZE);
    }
    if (s->stsz != stsz || (stsz && !s->stak)) {
        free(s->stak);
        s->stsz = stsz;
        s->stak = stsz ? malloc(stsz) : NULL;
    }
    return s;
}

/* Prepare the first capture ahead of time: the executable's symbols, the
 * records and the QUIESCE rules (~12 ms once), and a spare snapshot whose
 * buffers are faulted in (~8 ms once).  Game thread; reads nothing of the
 * machine. */
void dbg_state_prepare(void) {
    DbgStateSnap *s;
    if (!build_records()) return;
    busy_static();
    s = snap_alloc();
    if (!s) return;
    if (s->natv) memset(s->natv, 0, g_natv_bytes);
    if (s->ram) memset(s->ram, 0, PRAM_SIZE);
    if (s->mmio) memset(s->mmio, 0, MMIO_SIZE);
    dbg_state_snap_free(s);
}

static double ms_between(const struct timespec *a, const struct timespec *b) {
    return (b->tv_sec - a->tv_sec) * 1e3 + (b->tv_nsec - a->tv_nsec) / 1e6;
}

int dbg_state_capture(DbgStateSnap **out, const char **why) {
    static char msg[300];
    const char *busy, *ovl;
    DbgStateSnap *s;
    int i;
    struct timespec T[6];

    *out = NULL;
    *why = msg;
    clock_gettime(CLOCK_MONOTONIC, &T[0]);
    if (!build_records()) { snprintf(msg, sizeof msg, "%s", g_unavail); return -1; }
    {
        static int dumped;
        const char *inv = getenv("PC_STATE_INVENTORY");
        if (inv && *inv && !dumped) { dumped = 1; inventory_dump(inv); }
    }
    clock_gettime(CLOCK_MONOTONIC, &T[1]);
    busy = busy_static();
    clock_gettime(CLOCK_MONOTONIC, &T[2]);
    if (busy) {
        snprintf(msg, sizeof msg, "%s holds a heap buffer (operation in flight)", busy);
        return 1;
    }
    s = snap_alloc();
    if (!s) { snprintf(msg, sizeof msg, "out of memory"); return -1; }
    s->stlo = stack_lo();
    if (!s->natv || !s->ram || !s->mmio || (s->stsz && !s->stak)) {
        snap_release(s);
        snprintf(msg, sizeof msg, "out of memory");
        return -1;
    }
    clock_gettime(CLOCK_MONOTONIC, &T[3]);
    /* the SDL audio thread mixes the SPU core outside parity runs */
    spu_hal_audio_lock();
    for (i = 0; i < g_nrecs; i++)
        memcpy(s->natv + g_recs[i].off, (const void *)g_recs[i].addr, g_recs[i].size);
    memcpy(s->ram, (const void *)PRAM_BASE, PRAM_SIZE);
    memcpy(s->mmio, (const void *)MMIO_BASE, MMIO_SIZE);
    spu_hal_audio_unlock();
    if (s->stsz) memcpy(s->stak, (const void *)s->stlo, s->stsz);
    /* the KEEP pieces inside saved objects: never in a file */
    for (i = 0; i < g_nrecs; i++) {
        int k;
        for (k = 0; k < g_recs[i].nkeep; k++)
            memset(s->natv + g_recs[i].off + g_recs[i].keep[k].lo, 0,
                   g_recs[i].keep[k].hi - g_recs[i].keep[k].lo);
    }
    clock_gettime(CLOCK_MONOTONIC, &T[4]);

    ovl = hal_overlay_loaded();
    snprintf(s->info, sizeof s->info,
             "format=2\nbuild_id=%s\nplatform=%s\nframe_counter=%ld\nvblanks=%lu\nticks=%llu\n"
             "now_step=%u\nnow_sub_step=%u\nnow_sub_sub_step=%u\nnow_girl=%u\n"
             "overlay=%s\nnatv_records=%d\nnatv_bytes=%lu\nboundary_sp=0x%lx\n"
             "stack_window=%lu\n",
             dbg_plat_build_id(), dbg_plat_name(), (long)frame_counter,
             (unsigned long)hal_clock_vblanks(), hal_clock_ticks(), now_step, now_sub_step,
             now_sub_sub_step, now_girl, ovl ? ovl : "", g_nrecs, g_natv_bytes,
             dbg_boundary_sp, s->stsz);
    clock_gettime(CLOCK_MONOTONIC, &T[5]);
    if (getenv("PC_QS_TIMING")) {
        int k;
        fprintf(stderr, "[DBG] capture:");
        for (k = 1; k <= 5; k++) fprintf(stderr, " %.2f", ms_between(&T[k - 1], &T[k]));
        fprintf(stderr, " ms (records, busy, alloc, copy, info); natv %lu stack %lu\n",
                g_natv_bytes, s->stsz);
    }
    snprintf(s->msg, sizeof s->msg, "frame %ld scene %u/%u/%u overlay %s",
             (long)frame_counter, now_step, now_sub_step, now_sub_sub_step,
             ovl ? ovl : "-");
    *out = s;
    return 0;
}

int dbg_state_write(DbgStateSnap *s, const char *path, const char **why) {
    static __thread char msg[1200];
    char tmp[1100];
    Out o;
    unsigned long stored = 4;
    u32 count = (u32)g_nrecs;
    int ok, i;

    *why = msg;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    /* the host's memory map (advisory, the audit's): read here, off the
     * frame -- a VirtualQuery walk costs ~30 ms on Windows */
    if (!s->maps) s->maps = dbg_plat_maps(&s->maps_len);
    memset(&o, 0, sizeof o);
    o.ok = 1;
    out_put(&o, "TOKISTv2", 8);
    write_section(&o, "INFO", 0, 0, (const unsigned char *)s->info, strlen(s->info));
    write_section(&o, "MAPS", 0, 0, (const unsigned char *)(s->maps ? s->maps : ""),
                  s->maps ? s->maps_len : 0);
    write_section(&o, "PRAM", FLAG_PAGED, PRAM_BASE, s->ram, PRAM_SIZE);
    write_section(&o, "MMIO", 0, MMIO_BASE, s->mmio, MMIO_SIZE);
    for (i = 0; i < g_nrecs; i++)
        stored += 2 + (unsigned long)strlen(g_recs[i].key) + 24 +
                  paged_size(s->natv + g_recs[i].off, g_recs[i].size);
    section_head(&o, "NATV", 0, 0, g_natv_bytes, stored);
    out_put(&o, &count, 4);
    for (i = 0; i < g_nrecs; i++) {
        const Rec *r = &g_recs[i];
        unsigned short kl = (unsigned short)strlen(r->key);
        unsigned long long a = r->addr, z = r->size,
                           st = paged_size(s->natv + r->off, r->size);
        out_put(&o, &kl, 2);
        out_put(&o, r->key, kl);
        out_put(&o, &a, 8);
        out_put(&o, &z, 8);
        out_put(&o, &st, 8);
        put_paged(&o, s->natv + r->off, r->size);
    }
    if (s->stsz) write_section(&o, "STAK", FLAG_PAGED, s->stlo, s->stak, s->stsz);
    /* this install's signature (pc/hal/state_sign.c): a load accepts only
     * a state written here */
    if (o.ok) {
        uint8_t trailer[STATE_TRAILER_LEN];
        if (state_sign_trailer(o.p, o.n, trailer)) {
            free(o.p);
            snprintf(msg, sizeof msg, "cannot sign the state: no key in %s", pc_asset_dir());
            dbg_state_snap_free(s);
            return -1;
        }
        out_put(&o, trailer, sizeof trailer);
    }
    ok = o.ok && dbg_write_file(tmp, o.p, o.n);
    free(o.p);
    if (!ok || dbg_replace_file(tmp, path)) {
        extern int remove(const char *);
        remove(tmp);
        snprintf(msg, sizeof msg, "write failed: %s", path);
        dbg_state_snap_free(s);
        return -1;
    }
    snprintf(msg, sizeof msg, "%s", s->msg);
    dbg_state_snap_free(s);
    return 0;
}

int dbg_state_save(const char *path, const char **why) {
    DbgStateSnap *s;
    int r = dbg_state_capture(&s, why);
    if (r) return r;
    return dbg_state_write(s, path, why);
}

/* ------------------------------------------------------------------ */
/* loading                                                             */
/* ------------------------------------------------------------------ */

/* a whole state file in memory, sections located */
typedef struct {
    char tag[5];
    unsigned flags;
    unsigned long addr, size, stored;
    const unsigned char *data;
} Section;

static int decode_paged(const unsigned char *p, unsigned long stored, unsigned long size,
                        unsigned char *out) {
    unsigned long off;
    const unsigned char *end = p + stored;
    for (off = 0; off < size; off += PAGE) {
        unsigned long len = size - off < PAGE ? size - off : PAGE;
        if (p >= end) return 0;
        if (*p++ == 0) { memset(out + off, 0, len); continue; }
        if ((unsigned long)(end - p) < len) return 0;
        memcpy(out + off, p, len);
        p += len;
    }
    return p == end;
}

static int decode_into(const Section *s, unsigned char *out) {
    if (!(s->flags & FLAG_PAGED)) {
        if (s->stored != s->size) return 0;
        memcpy(out, s->data, s->size);
        return 1;
    }
    return decode_paged(s->data, s->stored, s->size, out);
}

static const char *info_value(const Section *info, const char *key, char *buf, size_t cap) {
    size_t kl = strlen(key);
    const char *p = (const char *)info->data, *end = p + info->size;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (len > kl && !strncmp(p, key, kl) && p[kl] == '=') {
            size_t vl = len - kl - 1;
            if (vl >= cap) vl = cap - 1;
            memcpy(buf, p + kl + 1, vl);
            buf[vl] = 0;
            return buf;
        }
        p += len + 1;
    }
    return NULL;
}

/* PC_STATE_NEGATIVE (negative controls of the round-trip tests only): a
 * load leaves one component as the loading process has it --
 *   pram | mmio | stak | natv   a whole section
 *   KEY                         one native record (`spu.cpp:_ZN3SPUL5s_ramE`) */
static const char *negative_component(void) {
    static int done;
    static const char *v;
    if (!done) {
        done = 1;
        v = getenv("PC_STATE_NEGATIVE");
        if (v && !*v) v = NULL;
        if (v) fprintf(stderr, "[DBG] NEGATIVE CONTROL %s: a load leaves it out on purpose\n", v);
    }
    return v;
}

static int force_load(void) {
    const char *f = getenv("PC_LOADSTATE_FORCE");
    return f && *f == '1';
}

/* the parsed NATV records of a file (pointing into it) */
typedef struct {
    int rec;                    /* this binary's record, -1 if none */
    const unsigned char *data;
    unsigned long size, stored;
} InRec;

int dbg_state_load(const char *path, const char **why) {
    static char msg[1200];
    unsigned long n = 0;
    unsigned char *file = NULL, *natv = NULL, *ram = NULL, *mmio = NULL, *stak = NULL;
    const unsigned char *p, *end;
    Section secs[12];
    int nsec = 0, i, nin = 0, missing = 0, extra = 0;
    const Section *sinfo = NULL, *snatv = NULL, *sram = NULL, *smmio = NULL, *sstak = NULL;
    char bid[80], plat[40], ovl[64], fc[32], bsp[32];
    const char *live_ovl, *neg = negative_component();
    InRec *in = NULL;
    unsigned char *seen = NULL;
    unsigned long stsz;

    *why = msg;
#define FAIL(...) do { snprintf(msg, sizeof msg, __VA_ARGS__); goto fail; } while (0)
    if (!build_records()) FAIL("%s", g_unavail);
    file = dbg_read_file(path, &n);
    if (!file || n < 8) FAIL("cannot read %s", path);
    /* nothing of the file is parsed before its signature checks out: a
     * state is restored byte for byte over native memory */
    {
        unsigned long body;
        int v = state_verify(file, n, &body);
        if (v == -2 && !force_load())
            FAIL("cannot check %s: no state key in %s", path, pc_asset_dir());
        if (v && !force_load())
            FAIL("%s was not saved by this installation (save states are machine snapshots and "
                 "only load where they were made; share the memory card instead)", path);
        n = body;
    }
    if (!memcmp(file, "TOKISTv1", 8))
        FAIL("%s is a state of an older build (format 1: a process image)", path);
    if (memcmp(file, "TOKISTv2", 8)) FAIL("%s is not a state file", path);
    p = file + 8;
    end = file + n;
    while (p + 32 <= end && nsec < 12) {
        Section *s = &secs[nsec];
        unsigned long long a, z, st;
        u32 fl;
        memcpy(s->tag, p, 4); s->tag[4] = 0;
        memcpy(&fl, p + 4, 4); memcpy(&a, p + 8, 8); memcpy(&z, p + 16, 8); memcpy(&st, p + 24, 8);
        s->flags = fl; s->addr = (unsigned long)a; s->size = (unsigned long)z;
        s->stored = (unsigned long)st;
        s->data = p + 32;
        if ((unsigned long)(end - s->data) < s->stored) FAIL("%s: truncated section %s", path, s->tag);
        p = s->data + s->stored;
        if (!strcmp(s->tag, "INFO")) sinfo = s;
        else if (!strcmp(s->tag, "NATV")) snatv = s;
        else if (!strcmp(s->tag, "PRAM")) sram = s;
        else if (!strcmp(s->tag, "MMIO")) smmio = s;
        else if (!strcmp(s->tag, "STAK")) sstak = s;
        nsec++;
    }
    if (!sinfo || !snatv || !sram || !smmio) FAIL("%s: truncated or missing sections", path);
    if (sinfo->size != sinfo->stored || (sinfo->flags & FLAG_PAGED))
        FAIL("%s: corrupt INFO section", path);
    if (!info_value(sinfo, "platform", plat, sizeof plat)) plat[0] = 0;
    if (!info_value(sinfo, "build_id", bid, sizeof bid)) bid[0] = 0;
    if (strcmp(bid, dbg_plat_build_id()) && !force_load())
        FAIL("%s was saved by another build (%s build-id %s, this binary %s %s); "
             "PC_LOADSTATE_FORCE=1 overrides", path, plat, bid, dbg_plat_name(),
             dbg_plat_build_id());
    if (sram->addr != PRAM_BASE || sram->size != PRAM_SIZE || smmio->addr != MMIO_BASE ||
        smmio->size != MMIO_SIZE)
        FAIL("%s: the PS1 memory sections have the wrong size", path);

    /* the native records: every one must name an object of this binary
     * with the same size, and every object of this binary must be there */
    {
        const unsigned char *q = snatv->data, *qe = snatv->data + snatv->stored;
        u32 count;
        if (qe - q < 4) FAIL("%s: corrupt NATV section", path);
        memcpy(&count, q, 4);
        q += 4;
        /* every record takes at least its 26-byte head */
        if (count > (unsigned long)(qe - q) / 26) FAIL("%s: corrupt NATV section", path);
        in = malloc((unsigned long)(count ? count : 1) * sizeof *in);
        seen = malloc((unsigned long)(g_nrecs ? g_nrecs : 1));
        if (!in || !seen) FAIL("out of memory");
        memset(seen, 0, (size_t)(g_nrecs ? g_nrecs : 1));
        for (i = 0; i < (int)count; i++) {
            unsigned short kl;
            char key[400];
            unsigned long long a, z, st;
            if (qe - q < 2) FAIL("%s: corrupt NATV section", path);
            memcpy(&kl, q, 2);
            q += 2;
            if ((unsigned long)(qe - q) < (unsigned long)kl + 24 || kl >= sizeof key)
                FAIL("%s: corrupt NATV section", path);
            memcpy(key, q, kl);
            key[kl] = 0;
            q += kl;
            memcpy(&a, q, 8); memcpy(&z, q + 8, 8); memcpy(&st, q + 16, 8);
            q += 24;
            if ((unsigned long long)(qe - q) < st) FAIL("%s: corrupt NATV section", path);
            in[nin].rec = rec_find(key);
            in[nin].data = q;
            in[nin].size = (unsigned long)z;
            in[nin].stored = (unsigned long)st;
            q += st;
            if (in[nin].rec < 0) {
                if (!extra++) snprintf(msg, sizeof msg, "%s", key);
            } else if (g_recs[in[nin].rec].size != in[nin].size) {
                FAIL("%s does not describe this binary's data: %s is %lu bytes in the state, "
                     "%lu here", path, key, in[nin].size, g_recs[in[nin].rec].size);
            } else {
                seen[in[nin].rec] = 1;
            }
            nin++;
        }
        for (i = 0; i < g_nrecs; i++) missing += !seen[i];
        /* never a partial load, PC_LOADSTATE_FORCE or not: a machine with
         * some objects from the state and some from this process is no
         * machine the game ever was */
        if (extra || missing) {
            char first_extra[400];
            snprintf(first_extra, sizeof first_extra, "%s", extra ? msg : "-");
            for (i = 0; i < g_nrecs && seen[i]; i++) {}
            FAIL("%s does not describe this binary's data: %d records unknown here (first %s), "
                 "%d objects missing (first %s)", path, extra, first_extra, missing,
                 i < g_nrecs ? g_recs[i].key : "-");
        }
    }
    /* decode everything before anything is written: a corrupt file must
     * leave the machine as it was */
    natv = malloc(g_natv_bytes ? g_natv_bytes : 1);
    ram = malloc(PRAM_SIZE);
    mmio = malloc(MMIO_SIZE);
    if (!natv || !ram || !mmio) FAIL("out of memory");
    for (i = 0; i < nin; i++) {
        if (in[i].rec < 0) continue;
        if (!decode_paged(in[i].data, in[i].stored, in[i].size, natv + g_recs[in[i].rec].off))
            FAIL("%s: corrupt record %s", path, g_recs[in[i].rec].key);
    }
    if (!decode_into(sram, ram) || !decode_into(smmio, mmio)) FAIL("%s: corrupt PS1 memory", path);
    stsz = stack_size();
    if (sstak && sstak->size == stsz && stsz) {
        stak = malloc(stsz);
        if (!stak || !decode_into(sstak, stak)) FAIL("%s: corrupt STAK section", path);
    } else if (sstak) {
        fprintf(stderr, "[DBG] load: the state's stack window is %lu bytes, this process has %lu: "
                "the game stack's leftovers are not restored\n", sstak->size, stsz);
    }
    if (!info_value(sinfo, "boundary_sp", bsp, sizeof bsp)) bsp[0] = 0;
    if (stak && strtoul(bsp, NULL, 0) != dbg_boundary_sp)
        printf("[DBG] load: the game stack sits elsewhere in this process (boundary 0x%lx, the "
               "saver's %s); its leftovers are written at the same depth\n", dbg_boundary_sp, bsp);

    spu_hal_audio_lock();
    if (!neg || strcmp(neg, "natv")) {
        for (i = 0; i < g_nrecs; i++) {
            const Rec *r = &g_recs[i];
            unsigned long cur = 0;
            int k;
            if (!seen[i] || (neg && !strcmp(neg, r->key))) continue;
            for (k = 0; k <= r->nkeep; k++) {
                unsigned long stop = k < r->nkeep ? r->keep[k].lo : r->size;
                if (stop > cur) memcpy((void *)(r->addr + cur), natv + r->off + cur, stop - cur);
                if (k < r->nkeep && r->keep[k].hi > cur) cur = r->keep[k].hi;
            }
        }
    }
    if (!neg || strcmp(neg, "pram")) memcpy((void *)PRAM_BASE, ram, PRAM_SIZE);
    if (!neg || strcmp(neg, "mmio")) memcpy((void *)MMIO_BASE, mmio, MMIO_SIZE);
    if (stak && (!neg || strcmp(neg, "stak"))) memcpy((void *)stack_lo(), stak, stsz);
    spu_hal_audio_unlock();

    /* the machine is the state's now, whatever the overlay check below
     * says: every observer hears of it (before 2026-11-03 the overlay
     * mismatch returned before the callers' own hooks ran) */
    dbg_state_after_restore(DBG_RESTORE_FILE);
    live_ovl = hal_overlay_loaded();
    if (!info_value(sinfo, "overlay", ovl, sizeof ovl)) ovl[0] = 0;
    if (!info_value(sinfo, "frame_counter", fc, sizeof fc)) strcpy(fc, "?");
    free(file); free(natv); free(ram); free(mmio); free(stak); free(in); free(seen);
    if (strcmp(ovl, live_ovl ? live_ovl : "")) {
        snprintf(msg, sizeof msg, "loaded, but the overlay is %s (state says %s)",
                 live_ovl ? live_ovl : "-", ovl[0] ? ovl : "-");
        return -1;
    }
    snprintf(msg, sizeof msg, "frame %s scene %u/%u/%u overlay %s", fc,
             now_step, now_sub_step, now_sub_sub_step, ovl[0] ? ovl : "-");
    return 0;
fail:
    free(file); free(natv); free(ram); free(mmio); free(stak); free(in); free(seen);
    return -1;
#undef FAIL
}

/* ------------------------------------------------------------------ */
/* in-memory states (pc/hal/rewind.c)                                  */
/* ------------------------------------------------------------------ */

static int range_put(DbgRange *out, int cap, int n, unsigned long lo, unsigned long hi) {
    if (hi <= lo) return n;
    if (n && n <= cap && out[n - 1].addr + out[n - 1].size == lo) {
        out[n - 1].size += hi - lo;
        return n;
    }
    if (n < cap) {
        out[n].addr = lo;
        out[n].size = hi - lo;
    }
    return n + 1;
}

int dbg_state_ranges(DbgRange *out, int cap, const char *const *exclude) {
    Range ex[32];
    int nex = 0, i, k, n = 0;
    if (!build_records()) return -1;
    for (i = 0; exclude && exclude[i] && nex < 32; i++) {
        unsigned long sz = 0;
        unsigned char *a = dbg_sym_lookup(exclude[i], &sz);
        if (!a || !sz) {
            fprintf(stderr, "[DBG] ranges: %s not found\n", exclude[i]);
            continue;
        }
        ex[nex].lo = (unsigned long)a;
        ex[nex].hi = (unsigned long)a + sz;
        nex++;
    }
    /* each record minus its KEEP pieces minus the excluded symbols */
    for (i = 0; i < g_nrecs; i++) {
        const Rec *r = &g_recs[i];
        unsigned long cur = r->addr, e = r->addr + r->size;
        Range holes[8 + 32];
        int nh = 0, a, b;
        for (k = 0; k < r->nkeep; k++) {
            holes[nh].lo = r->addr + r->keep[k].lo;
            holes[nh++].hi = r->addr + r->keep[k].hi;
        }
        for (k = 0; k < nex; k++)
            if (ex[k].lo < e && ex[k].hi > cur) holes[nh++] = ex[k];
        for (a = 0; a < nh; a++)          /* a handful: a selection sort */
            for (b = a + 1; b < nh; b++)
                if (holes[b].lo < holes[a].lo) { Range t = holes[a]; holes[a] = holes[b]; holes[b] = t; }
        for (a = 0; a < nh && cur < e; a++) {
            if (holes[a].lo > cur) n = range_put(out, cap, n, cur, holes[a].lo < e ? holes[a].lo : e);
            if (holes[a].hi > cur) cur = holes[a].hi;
        }
        if (cur < e) n = range_put(out, cap, n, cur, e);
    }
    n = range_put(out, cap, n, PRAM_BASE, PRAM_BASE + PRAM_SIZE);
    n = range_put(out, cap, n, MMIO_BASE, MMIO_BASE + MMIO_SIZE);
    if (stack_size()) n = range_put(out, cap, n, stack_lo(), stack_lo() + stack_size());
    return n;
}

const char *dbg_state_busy(void) {
    if (!build_records()) return g_unavail;
    return busy_static();
}

/* The one place a restored machine is announced (docs/pc-port/
 * STATE-MACHINES.md, "After a restore").  Every way the machine is
 * replaced -- a state file (quickload, the save browser, the resume, the
 * toolkit's `load`, PC_LOADSTATE, F9) or a rewind point -- ends here, so
 * every session-side machine that watches the game is told the same way.
 * Until 2026-11-03 each path called its own subset: the toolkit's load
 * skipped the pacing resync (a paced session slept away the emulated gap
 * between the two moments) and the autosave's week; a rewind skipped the
 * extras.  All weak: harnesses link this file without them. */
void dbg_state_after_restore(int how) {
    /* pc/hal/trace.c: the input's VBlank clock follows the loaded one */
    extern void trace_after_load(void) __attribute__((weak));
    /* pc/hal/extras.c: the extras' detectors start over from the loaded
     * moment (a jump is not an ending or a picture seen) */
    extern void extras_after_load(void) __attribute__((weak));
    /* pc/hal/gpu_hires.c: the shadow VRAM follows the restored VRAM */
    extern void gpu_hires_after_restore(void) __attribute__((weak));
    /* pc/hal/system.c: wall pacing starts a new schedule from the loaded
     * clock */
    extern void hal_pacing_resync(void) __attribute__((weak));
    /* pc/hal/save_auto.c: the loaded week counts as saved */
    extern void sa_after_load(void) __attribute__((weak));
    /* pc/hal/rewind.c: the buffer described another time line */
    extern void rw_after_load(void) __attribute__((weak));
    /* pc/hal/enhance.c: an auto press in flight belongs to the old moment */
    extern void text_assist_after_load(void) __attribute__((weak));
    /* pc/hal/spu.c: the real-time register queue holds the old time line's
     * writes (dropped), and a SoundFont's notes (silenced) */
    extern void spu_hal_after_restore(void) __attribute__((weak));
    /* pc/hal/music_pack.c: the songs are identified again; the dump's take ends */
    extern void music_pack_after_load(void) __attribute__((weak));
    static const char *const names[] = { "file", "rewind" };
    char told[160];
    size_t n = 0;
#define TELL(fn, label) \
    do { if (fn) { fn(); n += (size_t)snprintf(told + n, sizeof told - n, " %s", label); } } while (0)

    told[0] = 0;
    TELL(trace_after_load, "trace");
    TELL(extras_after_load, "extras");
    TELL(gpu_hires_after_restore, "hires");
    TELL(text_assist_after_load, "assist");
    TELL(hal_pacing_resync, "pacing");
    TELL(sa_after_load, "autosave");
    TELL(spu_hal_after_restore, "audio");
    TELL(music_pack_after_load, "music");
    if (how != DBG_RESTORE_REWIND) TELL(rw_after_load, "rewind");
#undef TELL
    /* PC_RESTORE_LOG=1: which observers heard of it (pc/tools/
     * test_state_machines.py checks every path tells all of them) */
    {
        const char *e = getenv("PC_RESTORE_LOG");
        if (e && *e == '1') {
            printf("[RESTORE] %s:%s\n", how >= 0 && how <= DBG_RESTORE_REWIND ? names[how] : "?", told);
            fflush(stdout);
        }
    }
}
