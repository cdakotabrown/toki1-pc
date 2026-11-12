/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Real clock, VBlank/SDK adapters and unchanged trapIntrVSync/CheckCallback. */
#define main irq_fixture_main
#include "irq_harness.c"
#undef main
#include "gpu_queue.h"
/* Compile the original setter; only its public name/getter are adapted. */
#define set_alarm gpu_sdk_set_alarm
#define VSync gpu_alarm_vsync
#include "matched/set_alarm.c"   /* -iquote <toki1-decomp>/src/main (test_sdk_irq.py) */
#undef VSync
#undef set_alarm
extern s32 VSync(s32), libgpu_alarm_deadline_vcount, libgpu_alarm_spin_count;
s32 libgpu_alarm_deadline_vcount, libgpu_alarm_spin_count;
u8 graph_env[128];
extern u16 CheckCallback(void);
extern void trapIntrVSync(void);
/* VSync(-1) must not reach host presentation or input. */
#include <SDL2/SDL.h>
void debug_handle_input(SDL_Event *e) { (void)e; assert(0); }
void pad_handle_event(const SDL_Event *e) { (void)e; assert(0); }
void debug_frame(void) { assert(0); }
void gpu_display(void) { assert(0); }
int trace_turbo(void) { assert(0); return 0; }
void pc_watchdog_presented(void) { assert(0); }
int SDL_PollEvent(SDL_Event *e) { (void)e; assert(0); return 0; }
Uint32 SDL_GetTicks(void) { assert(0); return 0; }
void SDL_Delay(Uint32 ms) { (void)ms; assert(0); }
void SDL_RenderPresent(SDL_Renderer *r) { (void)r; assert(0); }

static int a_calls, b_calls, raw_calls, seen, overrun, abort_hook;
static int remove_irq, bios_mask, bios_ack;

static void bios_probe(void)
{
    assert(CheckCallback() == 0);
    seen = Vcount;
    if (bios_mask) SetIntrMask(0);
    if (bios_ack) hal_irq_ack(1u);
}

static void hook_a(void)
{
    assert(CheckCallback() == 1 && hal_irq_status() == 0);
    a_calls++;
    if (remove_irq) InterruptCallback(0, 0);
    if (overrun && a_calls == 1)
        hal_cpu_spend(hal_clock_ticks_per_frame() + 100u);
    if (abort_hook) {
        hal_cpu_abort_work();
        hal_irq_abort();
        hal_sdk_irq_abort();
        abort_hook = 0;
        longjmp(recovery, 1);
    }
}

static void hook_b(void)
{
    assert(CheckCallback() == 1);
    b_calls++;
}

static void raw_irq(void)
{
    assert(CheckCallback() == 1 && hal_irq_status() == 0);
    raw_calls++;
}

int main(int argc, char **argv)
{
    s32 event;
    u32 frame, physical;
    int which;
    HalIrqHandler old;
    (void)argc;
    which = atoi(argv[1]);
    frame = hal_clock_ticks_per_frame();
    event = OpenEvent(0xf2000003u, 2, 0x1000, bios_probe);
    assert(event != -1 && EnableEvent(event) == 1);
    if (which == 0) {
        hal_cpu_spend(3u * frame + 1u);
        assert(Vcount == 0 && VSync(-1) == 0 && edges == 3);
        assert(CheckCallback() == 0 && libapi_intr_env == 0);
        return 0;
    }
    ResetCallback();
    assert(libapi_intr_env == 1 && libapi_vsync_intr_enabled == 1);
    if (which == 1) {
        hal_cpu_spend(3u * frame + 1u);
        assert(VSync(-1) == 3 && Vcount == 3 && seen == 2);
    } else if (which == 2) {
        assert(VSyncCallback(hook_a) == 0);
        hal_cpu_spend(frame + 1u);
        assert(a_calls == 1 && Vcount == 1);
        assert(VSyncCallback(hook_b) == hook_a);
        hal_cpu_spend(frame);
        assert(a_calls == 1 && b_calls == 1 && Vcount == 2);
        assert(VSyncCallback(0) == hook_b);
        hal_cpu_spend(frame);
        assert(b_calls == 1 && Vcount == 3);
    } else if (which == 3) {
        VSyncCallback(hook_a);
        hal_cpu_spend(frame + 1u);
        ResetCallback();
        hal_cpu_spend(frame);
        assert(Vcount == 2 && a_calls == 2 && seen == 1);
    } else if (which == 4) {
        VSyncCallback(hook_a);
        hal_cpu_spend(frame + 1u);
        StopCallback();
        assert(GetIntrMask() == 0 && libapi_vsync_intr_enabled == 0);
        assert(libapi_intr_env == 0 && Vcount == 1);
        hal_cpu_spend(3u * frame);
        assert(Vcount == 1 && hal_irq_status() == 1);
        ResetCallback();
        assert(Vcount == 2 && a_calls == 1 && GetIntrMask() == 9);
        hal_cpu_spend(frame);
        assert(Vcount == 3 && a_calls == 1);
    } else if (which == 5) {
        old = InterruptCallback(0, raw_irq);
        assert(old == trapIntrVSync);
        hal_cpu_spend(3u * frame + 1u);
        assert(raw_calls == 3 && Vcount == 0 && seen == 0);
        assert(InterruptCallback(0, old) == raw_irq);
        hal_cpu_spend(frame);
        assert(Vcount == 1 && raw_calls == 3);
    } else if (which == 6) {
        old = InterruptCallback(0, 0);
        assert(old == trapIntrVSync && GetIntrMask() == 12);
        hal_cpu_spend(3u * frame + 1u);
        /* Removing the libetc handler stops VBlank delivery to libetc only:
         * the kernel's VBlank events and the pad poll are unaffected. */
        assert(Vcount == 0 && pads == 3 && hal_irq_status() == 1);
        physical = hal_clock_vblanks();
        assert(InterruptCallback(0, old) == 0);
        assert(Vcount == 1 && hal_clock_vblanks() == physical);
        assert(pads == 3 && GetIntrMask() == 13);
    } else if (which == 7) {
        SetIntrMask(0);
        hal_cpu_spend(3u * frame + 1u);
        assert(VSync(-1) == 0 && hal_clock_vblanks() == 3);
        gpu_queue_alarm_set();
        assert(libgpu_alarm_deadline_vcount == 240 && libgpu_alarm_spin_count == 0);
        SetIntrMask(13);
        assert(VSync(-1) == 1 && hal_clock_vblanks() == 3);
        gpu_queue_alarm_set();
        assert(libgpu_alarm_deadline_vcount == 241);
    } else if (which == 8) {
        VSyncCallback(hook_a);
        overrun = 1;
        hal_cpu_spend(frame + 1u);
        /* The overrun edge cannot take its exception, so it is delivered to
         * libetc once on return and its kernel events are not re-run. */
        assert(Vcount == 2 && a_calls == 2 && seen == 0 && pads == 1);
        /* plus the first edge's libetc exception time, charged once */
        assert(hal_clock_ticks() == 2u * frame + 101u + 24449u);
    } else if (which == 9) {
        VSyncCallback(hook_a);
        abort_hook = 1;
        if (setjmp(recovery) == 0) {
            hal_cpu_spend(frame + 1u);
            assert(0);
        }
        assert(CheckCallback() == 0 && Vcount == 1);
        hal_cpu_spend(frame + 1u);
        assert(a_calls == 2 && Vcount == 2 && CheckCallback() == 0);
    } else if (which == 10) {
        SetIntrMask(0);
        hal_cpu_spend(frame + 1u);
        StopCallback();
        assert(hal_irq_status() == 0);
        ResetCallback();
        assert(Vcount == 0 && GetIntrMask() == 9);
    } else if (which == 11) {
        InterruptCallback(0, 0);
        VSyncCallback(hook_a);
        ResetCallback();
        hal_cpu_spend(3u * frame + 1u);
        assert(Vcount == 0 && a_calls == 0 && GetIntrMask() == 12);
    } else if (which == 12) {
        EnterCriticalSection();
        hal_cpu_spend(3u * frame + 1u);
        gpu_queue_alarm_set();
        assert(libgpu_alarm_deadline_vcount == 240 && Vcount == 0);
        ExitCriticalSection();
        assert(Vcount == 1 && seen == 0);
    } else if (which == 13) {
        SetIntrMask(0);
        hal_cpu_spend(300u * frame + 1u);
        assert(Vcount == 0 && hal_clock_vblanks() == 300u);
        gpu_queue_work(5000u);
        assert(gpu_queue_sync(0) == 0);
        assert(libgpu_alarm_deadline_vcount == 240 && Vcount == 0);
    } else if (which == 14) {
        remove_irq = 1;
        VSyncCallback(hook_a);
        hal_cpu_spend(3u * frame + 1u);
        assert(Vcount == 1 && a_calls == 1 && GetIntrMask() == 12);
        remove_irq = 0;
        InterruptCallback(0, trapIntrVSync);
        assert(Vcount == 2 && a_calls == 2 && GetIntrMask() == 13);
    } else if (which == 15) {
        bios_mask = 1;
        hal_cpu_spend(frame + 1u);
        assert(Vcount == 0 && pads == 1 && GetIntrMask() == 0);
        assert(hal_irq_status() == 1);
        bios_mask = 0;
        SetIntrMask(13);
        assert(Vcount == 1 && pads == 1 && hal_irq_status() == 0);
    } else if (which == 16) {
        bios_ack = 1;
        hal_cpu_spend(frame + 1u);
        assert(Vcount == 0 && pads == 1 && hal_irq_status() == 0);
        bios_ack = 0;
        hal_cpu_spend(frame);
        assert(Vcount == 1 && pads == 2);
    } else assert(0);
    assert(CheckCallback() == 0);
    return 0;
}
