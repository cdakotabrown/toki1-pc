/* SPDX-License-Identifier: GPL-3.0-or-later */
// Minimal StateWrapper stub. Save-state serialization is not supported in the
// standalone library; all Do* calls are no-ops that neither read nor write.
#pragma once
#include "types.h"
#include <cstring>
#include <string>

class StateWrapper
{
public:
  enum class Mode { Read, Write };

  bool HasError() const { return false; }
  bool IsReading() const { return m_mode == Mode::Read; }
  bool IsWriting() const { return m_mode == Mode::Write; }
  u32 GetVersion() const { return 0; }

  template <typename T> void Do(T*) {}
  template <typename T> void DoPOD(T*) {}
  template <typename T, size_t N> void DoArray(T (*)[N]) {}
  template <typename T> void DoArray(T*, size_t) {}
  void DoBytes(void*, size_t) {}
  template <typename T, typename F> void DoEx(T*, u32, F) {}

  // spu.cpp uses sw.Do(&fifo_queue) and sw.Do(&string) -- catch-alls above.

private:
  Mode m_mode = Mode::Read;
};
