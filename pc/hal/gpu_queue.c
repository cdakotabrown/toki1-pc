/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Pixels are consumed eagerly; SDK dispatch and device readiness differ.
 * _exeque can notify the caller before the dispatched drawing is idle.
 * Native callbacks live outside the 32-bit PS1 metadata: its callback word
 * holds an opaque registration cookie, never a truncated host pointer.
 * Only decoded SDK clear commands currently contribute device work.
 * The unmasked clear entry and polling costs use measured CPU prefixes;
 * masked submission, raw queues, DMA/IRQ and arbitration remain unmodeled.
 * Poll costs approximate warm Interpreter observations; see
 * docs/pc-port/GPU-TIMING.md. They are not hardware cycle-exact timings.
 */
#include "hal.h"
#include "gpu_queue.h"
#include "profile.h"
#include <string.h>
#include <stdlib.h>

extern u8 graph_env[128];
/* Weak: the drawing harnesses link gpu.c, which reports every primitive's
 * drawing time (gpu_queue_work), without the system clock. */
extern u64 hal_clock_ticks(void) __attribute__((weak));
extern s32 hal_sdk_vcount(void);
extern void hal_cpu_spend(u32 ticks);
extern void gpu_sdk_set_alarm(void);
extern s32 libgpu_alarm_deadline_vcount, libgpu_alarm_spin_count;

/* The unchanged set_alarm calls only VSync(-1). Preserve its end state
 * using the same counter as the current native VSync HAL. Its two native
 * stores are atomic relative to the cooperative clock, not cycle-timed. */
s32 gpu_alarm_vsync(s32 mode)
{
    (void)mode;
    return hal_sdk_vcount();
}

void gpu_queue_alarm_set(void)
{
    gpu_sdk_set_alarm();
}

static int alarm_expired(void)
{
    s32 count;
    /* get_alarm tests the deadline before touching the poll count. The
     * two globals are shared with nested SDK calls; do not save/restore
     * an outer wait's private copy. Raw MMIO recovery remains unmodeled. */
    if (libgpu_alarm_deadline_vcount < hal_sdk_vcount()) return 1;
    count = libgpu_alarm_spin_count;
    libgpu_alarm_spin_count = (s32)((u32)count + 1u);
    return count >= 0x780001;
}
static GpuSyncCallback completion;
static u32 cookie, next_cookie;
static u32 generation;
static u32 active;
static int notifying, deferred;
/* DuckStation's command accounting uses two work ticks per CPU tick. This
 * is distinct from the GPU's CRTC clock. Keep half-tick carry across work. */
static u64 ready_work_tick;

/* GPU_DRAW_TIME=0 (the enhanced profile, pc/hal/profile.c): the GPU draws
 * in no time, so DrawSync never waits behind it. */
static int draw_time_on(void)
{
    static int on = -1;
    if (on < 0) on = hal_model_on("GPU_DRAW_TIME");
    return on;
}

void gpu_queue_work(u32 gpu_ticks)
{
    u64 now;
    if (!hal_clock_ticks || !draw_time_on()) return;
    now = hal_clock_ticks() * 2u;
    if (ready_work_tick < now) ready_work_tick = now;
    ready_work_tick += gpu_ticks;
}

/* A DMA that never ends: DrawOTag over a cyclic ordering table (gpu.c
 * ot_cycle_entry).  The console's GPU walks the cycle until a reset; a
 * DrawSync(0) before one waits out libgpu's 240-VBlank alarm (the loop
 * below: GPU timeout, then the queue reset), and a ResetGraph first aborts
 * it (gpu_queue_reset).  On the reference a TACO sub-step-10 frame lasted
 * 242 VBlanks there (seeded start taco-s1, SEEDED-STARTS.md); the port
 * finished the list at once.  GPU_OT_CYCLE_HANG=0 is the negative control
 * (the device is idle after the list, as before). */
static int ot_cycle_hang_on(void)
{
    static int on = -1;
    if (on < 0) on = hal_model_on("GPU_OT_CYCLE_HANG");
    return on;
}

void gpu_queue_hang(void)
{
    if (ot_cycle_hang_on()) ready_work_tick = ~(u64)0 >> 1;
}

static int device_busy(void)
{
    return hal_clock_ticks && ready_work_tick > hal_clock_ticks() * 2u;
}

int gpu_queue_device_busy(void) { return device_busy(); }

static u32 read_word(s32 offset)
{
    u32 value;
    memcpy(&value, graph_env + offset, 4);
    return value;
}

static void write_word(s32 offset, u32 value)
{
    memcpy(graph_env + offset, &value, 4);
}

GpuSyncCallback DrawSyncCallback(GpuSyncCallback callback)
{
    GpuSyncCallback previous;
    previous = read_word(12) == cookie ? completion : 0;
    completion = callback;
    if (!callback) deferred = 0;
    cookie = 0;
    if (callback) {
        next_cookie++;
        if (!next_cookie) next_cookie++;
        cookie = next_cookie;
    }
    write_word(12, cookie);
    return previous;
}

u32 gpu_queue_begin(void)
{
    /* _addque2 sets w68 even on its immediate and queue-disabled paths. */
    write_word(8, 1);
    active++;
    return generation;
}

static void notify_completion(void)
{
    GpuSyncCallback callback;
    if (!graph_env[1] || !read_word(8) || !completion ||
        !cookie || read_word(12) != cookie) {
        deferred = 0;
        return;
    }
    if (notifying) {
        deferred = 1;
        return;
    }
    deferred = 0;
    callback = completion;
    /* _exeque clears activity before calling out. A callback may register
     * a replacement or reset and submit work. Do not overwrite that work. */
    write_word(8, 0);
    notifying = 1;
    callback();
    notifying = 0;
}

void gpu_queue_end(u32 ticket)
{
    /* A reset invalidates all old completion tickets. */
    if (ticket != generation || !active) return;
    active--;
    if (!active) notify_completion();
}

/* DRAWSYNC_BUSY_POLL: the blocking wait's iterations as the reference
 * runs them behind a busy GPU (the reference emulator, the boot's 64-strip
 * vram_area_clear: probes on _sync's two GPUSTAT reads 0x800A2754 /
 * 0x800A27B0 and its exit; 65 waits, all alike): the first busy read 195
 * ticks after DrawSync's entry, the next 162 later, then 154, then 150
 * each; from the read that sees the GPU idle to the caller's next call 88
 * (32 to _sync's return, 14 to DrawSync's, 42 of vram_area_clear's own
 * code: like the other HAL CPU models, to the caller's next call, since
 * retail's probes inside DrawSync close that gap, not the caller's
 * entry).  The port's 181 / 150 / 42
 * put its reads 26 ticks early from the second one on, which moved the
 * strip's end by one iteration.  The first read keeps 181 (an idle
 * DrawSync returns from it; in game that path also runs the queue), so
 * the first iteration is 357 - 181 = 176.  DRAWSYNC_BUSY_POLL=0 is the
 * negative control. */
static int busy_poll_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("DRAWSYNC_BUSY_POLL");
        on = !(e && e[0] == '0');
    }
    return on;
}
static u32 poll_tail(int iteration)
{
    /* each iteration is 112 ticks to get_alarm's count store, then this */
    return iteration == 0 ? 176u - 112u : iteration == 1 ? 154u - 112u : 38u;
}

s32 gpu_queue_sync(s32 mode)
{
    int busy, iterations;
    /* No asynchronous worker can unwind an active native call. */
    if (active) return mode ? 1 : -1;
    /* Service at most one dispatch notification per external fence. */
    if (deferred && !notifying) notify_completion();
    if (mode) {
        /* Sample after entry work, then preserve that observation even
         * if the GPU finishes during the return path. IRQ callbacks may
         * reset or submit while this CPU work advances the shared clock. */
        hal_cpu_spend(114u);
        busy = active || device_busy();
        hal_cpu_spend(busy ? 55u : 44u);
        return busy ? 1 : 0;
    }
    /* Preserve the measured first-read budget while placing the SDK's
     * alarm stores at its read/return phase within that entry work. */
    hal_cpu_spend(131u);
    gpu_queue_alarm_set();
    hal_cpu_spend(50u);
    iterations = 0;
    while (active || device_busy()) {
        if (active) return -1;
        /* The SDK alarm is 240 VBlanks, with a separate poll limit.
         * Its raw diagnostic/MMIO recovery path is not reproduced. */
        /* Warm traces put get_alarm's count store about 112 ticks
         * after the busy status read, within the 150-tick iteration. */
        hal_cpu_spend(112u);
        if (alarm_expired()) {
            gpu_queue_reset(0);
            return -1;
        }
        hal_cpu_spend(busy_poll_on() ? poll_tail(iterations) : 38u);
        iterations++;
    }
    hal_cpu_spend(iterations && busy_poll_on() ? 88u : 42u);
    return 0;
}

void gpu_queue_reset(s32 full)
{
    generation++;
    active = 0;
    deferred = 0;
    ready_work_tick = 0;
    if (full) {
        completion = 0;
        cookie = 0;
        write_word(12, 0);
    }
}
