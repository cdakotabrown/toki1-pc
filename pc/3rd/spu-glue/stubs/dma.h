/* SPDX-License-Identifier: GPL-3.0-or-later */
// Minimal DMA stub. spu.cpp signals DMA readiness via DMA::SetRequest.
// The standalone library has no real DMA controller; calls are ignored.
#pragma once
#include "types.h"

namespace DMA {

enum class Channel : u8
{
  MDECin = 0,
  MDECout = 1,
  GPU = 2,
  CDROM = 3,
  SPU = 4,
  PIO = 5,
  OTC = 6,
  NumChannels = 7
};

inline void SetRequest(Channel /*channel*/, bool /*request*/) {}

} // namespace DMA
