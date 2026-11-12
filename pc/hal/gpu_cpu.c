/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libgpu CPU time on the shared clock: the SDK submission phases that
 * PutDrawEnv, DrawOTag and ResetGraph(1) spend around their native device
 * effects. Fixtures that do not model the clock stub these two calls. */
#include "gpu_cpu.h"
#include "gpu_queue.h"
#include "pc_irq.h"

extern void hal_cpu_spend(u32 ticks);

/* libgpu's own CPU work around one SDK call, in two measured phases.
 * An _addque2 submission runs its entry work, set_alarm, then
 * SetIntrMask(0); the dispatch and the device kick
 * run masked; the saved mask is restored and the call returns. The
 * native device effects are applied by the caller between the two
 * phases, at the point retail masks interrupts. Only CPU time is
 * modeled: no GPU device work, DMA bus steal or DMA IRQ cost is added.
 * A VBlank that falls in the masked span runs the kernel events at once
 * and reaches libetc's chain when the mask is restored, as on retail. */
void gpu_cpu_submit_entry(u32 to_alarm, u32 to_mask)
{
    hal_cpu_spend(to_alarm);
    gpu_queue_alarm_set();
    hal_cpu_spend(to_mask);
}

void gpu_cpu_masked(u32 masked, u32 to_return)
{
    u32 saved;
    saved = SetIntrMask(0);
    hal_cpu_spend(masked);
    SetIntrMask((s32)saved);
    hal_cpu_spend(to_return);
}
