/* SPDX-License-Identifier: GPL-3.0-or-later */
// Minimal Settings stub. spu.cpp reads a handful of fields off the global
// g_settings object; we provide them with sensible defaults.
#pragma once
#include "types.h"
#include "util/audio_stream.h"
#include <string>

struct Settings
{
  // CPU overclock (standalone = disabled).
  bool cpu_overclock_active = false;
  u32 cpu_overclock_numerator = 1;
  u32 cpu_overclock_denominator = 1;

  // Audio configuration.
  AudioBackend audio_backend = AudioBackend::Null;
  std::string audio_driver;
  std::string audio_output_device;
  AudioStreamParameters audio_stream_parameters;
};

// Global settings instance, defined in spu_stubs.cpp.
extern Settings g_settings;
