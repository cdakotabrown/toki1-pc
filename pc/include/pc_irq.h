/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Native I_MASK/CPU-enable and VBlank I_STAT latch. */
#ifndef PC_IRQ_H
#define PC_IRQ_H
#include "types.h"
typedef void (*HalIrqHandler)(void);
u32 SetIntrMask(s32 mask);
u32 GetIntrMask(void);
s32 hal_irq_enter_critical(void);
void hal_irq_exit_critical(void);
void hal_irq_edge(HalIrqHandler handler);
void hal_irq_vblank(HalIrqHandler handler);
u16 hal_irq_status(void);
int hal_irq_accepts(u16 bits);
int hal_irq_vblank_in_exception(HalIrqHandler handler);
void hal_irq_ack(u16 bits);
void hal_irq_abort(void);
void ResetCallback(void);
void StopCallback(void);
HalIrqHandler InterruptCallback(s32 index, HalIrqHandler handler);
HalIrqHandler VSyncCallback(HalIrqHandler handler);
void hal_sdk_irq_vblank(void);
void hal_sdk_irq_abort(void);
s32 hal_sdk_vcount(void);
#endif
