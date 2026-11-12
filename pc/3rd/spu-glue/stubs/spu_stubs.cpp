/* SPDX-License-Identifier: GPL-3.0-or-later */
// Standalone implementations for the tiny surface that spu.cpp links against:
//   - g_settings instance
//   - InterruptController::SetLineState hook (delegated to a C callback so the
//     host app can wire up its own IRQ handling if it wants to)
#include "settings.h"
#include "interrupt_controller.h"
#include "cdrom.h"

// Global settings instance. Defaults declared in Settings struct are fine.
Settings g_settings;

namespace InterruptController {

// The host can register a callback via the C shim (spu_psx_set_irq_hook).
static void (*s_irq_hook)(int irq, int state) = nullptr;

void SetLineState(IRQ irq, bool state)
{
  if (s_irq_hook)
    s_irq_hook(static_cast<int>(irq), state ? 1 : 0);
}

// Called from the C shim to install an IRQ hook.
extern "C" void spu_core_set_irq_hook(void (*hook)(int, int))
{
  s_irq_hook = hook;
}

} // namespace InterruptController

// The CD-ROM controller's audio output, fetched once per SPU sample
// (stubs/cdrom.h). The host's source returns 0 when it has nothing.
extern "C" {
int (*spu_core_cd_audio_source)(s16* left, s16* right) = nullptr;

void spu_core_set_cd_audio_source(int (*source)(s16*, s16*))
{
  spu_core_cd_audio_source = source;
}
}
