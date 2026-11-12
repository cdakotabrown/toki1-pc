/* SPDX-License-Identifier: GPL-3.0-or-later */
// Minimal InterruptController stub. spu.cpp calls
// InterruptController::SetLineState(IRQ::SPU, state). The shim forwards the
// request to an optional host callback; if unregistered it is a no-op.
#pragma once
#include "types.h"

namespace InterruptController {

enum class IRQ : u8
{
  VBLANK = 0,
  GPU = 1,
  CDROM = 2,
  DMA = 3,
  TMR0 = 4,
  TMR1 = 5,
  TMR2 = 6,
  PAD_MEMCARD = 7,
  SIO = 8,
  SPU = 9,
  Controller = 10,
  MaxCount = 11
};

// Implemented in spu_stubs.cpp so the shim can register a hook.
void SetLineState(IRQ irq, bool state);

} // namespace InterruptController
