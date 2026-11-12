/* SPDX-License-Identifier: GPL-3.0-or-later */
// Minimal TimingEvent stub. The standalone library does not schedule events
// against a real CPU clock; the shim drives the SPU::Execute callback directly
// with synthesized tick counts. TimingEvent is reduced to a dumb record that
// remembers its callback/param/interval so spu.cpp's bookkeeping still works.
#pragma once
#include "types.h"
#include <string_view>

class StateWrapper;

using TimingEventCallback = void (*)(void* param, TickCount ticks, TickCount ticks_late);

class TimingEvent
{
public:
  TimingEvent(std::string_view /*name*/, TickCount period, TickCount interval,
              TimingEventCallback callback, void* callback_param)
    : m_callback(callback), m_callback_param(callback_param),
      m_period(period), m_interval(interval) {}
  ~TimingEvent() = default;

  bool IsActive() const { return m_active; }
  TickCount GetPeriod() const { return m_period; }
  TickCount GetInterval() const { return m_interval; }
  TickCount GetTicksSinceLastExecution() const { return 0; }
  TickCount GetTicksUntilNextExecution() const { return m_interval; }

  void Delay(TickCount /*ticks*/) {}
  void Schedule(TickCount /*ticks*/) { m_active = true; }
  void SetIntervalAndSchedule(TickCount ticks) { m_interval = ticks; m_active = true; }
  void SetPeriodAndSchedule(TickCount ticks) { m_period = ticks; m_active = true; }

  // Run the work pending since the last execution.  The host drives the
  // core explicitly (SPU::GenerateSamples from ds_spu_mix), so nothing is
  // ever pending here: GetTicksSinceLastExecution() is 0.  Until 2026-10-30
  // this fired the callback with a whole interval on every call, and
  // spu.cpp calls GeneratePendingSamples() on most register reads and
  // writes -- so each register access silently ran one SPU sample into the
  // staging buffer (discarded): key-ons were consumed and voices advanced
  // outside the mixed output, and a burst of register traffic (a libsnd
  // voice flush) aged every voice by tens of samples before it was heard.
  void InvokeEarly(bool /*force*/ = false) {}

  void Activate() { m_active = true; }
  void Deactivate() { m_active = false; }
  void SetState(bool active) { m_active = active; }

  void SetInterval(TickCount interval) { m_interval = interval; }
  void SetPeriod(TickCount period) { m_period = period; }

  // Shim entry point: synthesize `ticks` worth of execution.
  void DriveTicks(TickCount ticks)
  {
    if (m_callback && m_active)
      m_callback(m_callback_param, ticks, 0);
  }

  TimingEventCallback m_callback = nullptr;
  void* m_callback_param = nullptr;
  TickCount m_period = 1;
  TickCount m_interval = 1;
  bool m_active = false;
};

namespace TimingEvents {

inline GlobalTicks GetGlobalTickCounter() { return 0; }
inline GlobalTicks GetEventRunTickCounter() { return 0; }

} // namespace TimingEvents
