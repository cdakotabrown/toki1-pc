/* SPDX-License-Identifier: GPL-3.0-or-later */
// Minimal WAVWriter stub. Voice-dump capture is disabled in the standalone
// library; Open() always fails so the unique_ptr slots stay reset.
#pragma once
#include "types.h"
#include <string>

class WAVWriter
{
public:
  WAVWriter() = default;
  ~WAVWriter() = default;

  bool Open(const char* /*path*/, u32 /*sample_rate*/, u32 /*channels*/) { return false; }
  void WriteFrames(const s16* /*frames*/, u32 /*count*/) {}
  void Close() {}
};
