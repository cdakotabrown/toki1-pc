/* SPDX-License-Identifier: GPL-3.0-or-later */
// Minimal AudioStream stub. NOTE: the upstream file is CC-BY-NC-ND-4.0 so we
// cannot vendor it; instead we provide just enough API for spu.cpp to link.
// The shim (spu_psx.c) hands out a staging buffer via BeginWrite/EndWrite and
// drains it into the caller's PCM buffer.
#pragma once
#include "types.h"
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

enum class AudioBackend : u8 { Null, Cubeb, SDL, OpenSLES, AAudio };
enum class AudioStretchMode : u8 { Off, Resample, TimeStretch, Count };

struct AudioStreamParameters
{
  u32 buffer_ms = 50;
  u32 output_latency_ms = 20;
  bool output_latency_minimal = false;
  AudioStretchMode stretch_mode = AudioStretchMode::Off;
};

class Error;

class AudioStream
{
public:
  static constexpr u32 CHANNELS = 2;

  AudioStream(u32 sample_rate) : m_sample_rate(sample_rate) {}
  virtual ~AudioStream() = default;

  u32 GetSampleRate() const { return m_sample_rate; }
  u32 GetBufferSize() const { return m_buffer_frames; }

  void SetOutputVolume(s32) {}
  void SetNominalRate(float) {}
  void SetPaused(bool) {}

  // SPU writes frames into this buffer. Shim holds a ring + drain hook.
  virtual void BeginWrite(s16** frame_ptr, u32* num_frames)
  {
    *frame_ptr = m_staging;
    *num_frames = STAGING_FRAMES;
  }

  virtual void EndWrite(u32 /*num_frames*/) {}

  static const char* GetStretchModeName(AudioStretchMode) { return "Off"; }

  // Factory hooks -- produce a no-op null stream.
  static std::unique_ptr<AudioStream> CreateNullStream(u32 sample_rate, u32 /*buffer_ms*/)
  {
    return std::make_unique<AudioStream>(sample_rate);
  }
  static std::unique_ptr<AudioStream> CreateStream(
      AudioBackend, u32 sample_rate, const AudioStreamParameters&,
      const char* /*driver*/ = nullptr, const char* /*device*/ = nullptr,
      Error* = nullptr)
  {
    return std::make_unique<AudioStream>(sample_rate);
  }

protected:
  static constexpr u32 STAGING_FRAMES = 4096;
  u32 m_sample_rate = 44100;
  u32 m_buffer_frames = STAGING_FRAMES;
  s16 m_staging[STAGING_FRAMES * CHANNELS] = {};
};
