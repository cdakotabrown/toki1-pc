/* SPDX-License-Identifier: GPL-3.0-or-later */
// Minimal System stub. spu.cpp pulls constants and a handful of helpers.
#pragma once
#include "types.h"
#include "settings.h"
#include "util/media_capture.h"
#include <string>
#include <string_view>

namespace System {

enum : TickCount
{
  // PSX clock: 44100Hz * 0x300 SPU cycles = 33.8688 MHz.
  MASTER_CLOCK = 44100 * 0x300
};

// Standalone overclock scaling = identity.
ALWAYS_INLINE_RELEASE TickCount ScaleTicksToOverclock(TickCount ticks) { return ticks; }

inline bool IsPaused() { return false; }
inline float GetAudioNominalRate() { return 1.0f; }
inline s32 GetAudioOutputVolume() { return 100; }

inline const std::string& GetGameTitle()
{
  static const std::string title;
  return title;
}

inline std::string GetNewMediaCapturePath(std::string_view /*title*/, std::string_view /*container*/)
{
  return std::string();
}

inline MediaCapture* GetMediaCapture() { return nullptr; }
inline void StopMediaCapture() {}

} // namespace System
