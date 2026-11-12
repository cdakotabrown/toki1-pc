/* SPDX-License-Identifier: GPL-3.0-or-later */
// spu_shim.cpp -- C-ABI dispatch wrapper around SPU::*.
//
// The shim translates the C-friendly spu_psx_* API into the C++ SPU namespace
// provided by the fetched upstream core. It also synthesizes the "CPU
// ticks" input for sample generation: the PSX SPU runs at 44100Hz while the
// CPU clock is 33.8688MHz, so each output frame corresponds to
//   MASTER_CLOCK / 44100 = 0x300 = 768 CPU ticks.
#include "spu.h"
#include "settings.h"
#include "timing_event.h"
#include "util/audio_stream.h"
#include "types.h"
#include <cstring>
#include <memory>

// Re-enter the anonymous namespace of spu.cpp via its public accessors.
// We need raw access to the SPU::Execute tick event so we can drive it
// with synthesized ticks; expose a small internal hook via a friendly
// free-function added to this translation unit.

namespace SPU {
// spu.cpp defines this as an internal file-static -- we can't touch it
// directly. Instead, we pretend to be "time passing" by calling
// GeneratePendingSamples which in turn forces the tick_event early.
// GeneratePendingSamples is already the documented entry point.
}

// --- The shim forwards a staging PCM buffer out to the host caller. ---
//
// Our AudioStream stub hands spu.cpp a per-instance staging buffer via
// BeginWrite(). Frames land there; we copy them into the caller's output
// in ds_spu_mix(). A small custom AudioStream subclass lets us inject a
// host-owned sink so we don't need to copy twice.

class HostSinkStream final : public AudioStream
{
public:
  HostSinkStream() : AudioStream(44100) {}

  // The sink points at the caller's output buffer for the current mix()
  // call. Set before driving SPU::GeneratePendingSamples, cleared after.
  void SetSink(s16* out, u32 capacity_frames)
  {
    m_sink = out;
    m_sink_cap = capacity_frames;
    m_sink_pos = 0;
  }

  u32 GetProducedFrames() const { return m_sink_pos; }

  void BeginWrite(s16** frame_ptr, u32* num_frames) override
  {
    if (m_sink && m_sink_pos < m_sink_cap)
    {
      *frame_ptr = m_sink + (m_sink_pos * CHANNELS);
      *num_frames = m_sink_cap - m_sink_pos;
    }
    else
    {
      // Fall back to staging so spu.cpp can keep going without overflowing.
      *frame_ptr = m_staging;
      *num_frames = STAGING_FRAMES;
    }
  }

  void EndWrite(u32 num_frames) override
  {
    if (m_sink && m_sink_pos < m_sink_cap)
      m_sink_pos += std::min(num_frames, m_sink_cap - m_sink_pos);
  }

private:
  s16* m_sink = nullptr;
  u32 m_sink_cap = 0;
  u32 m_sink_pos = 0;
};

// A single HostSinkStream instance owned by the SPU core via
// SPU::SetExternalOutputStream (added in our spu.cpp/spu.h patch). We keep a
// raw pointer so ds_spu_mix can configure the sink before driving samples.
static HostSinkStream* g_sink = nullptr;

extern "C" {

void ds_spu_init(void)
{
  SPU::Initialize();
  SPU::Reset();
  auto sink = std::make_unique<HostSinkStream>();
  g_sink = sink.get();
  SPU::SetExternalOutputStream(std::move(sink));
}

void ds_spu_reset(void)
{
  SPU::Reset();
}

void ds_spu_shutdown(void)
{
  SPU::Shutdown();
}

void ds_spu_write_reg(u32 addr, u16 value)
{
  // Convert PSX physical address (0x1F801C00..) to register offset.
  const u32 offset = addr - 0x1F801C00u;
  SPU::WriteRegister(offset, value);
}

u16 ds_spu_read_reg(u32 addr)
{
  const u32 offset = addr - 0x1F801C00u;
  return SPU::ReadRegister(offset);
}

void ds_spu_dma_write(const u8* data, u32 dest, u32 size)
{
  // Set the transfer address register, then feed bytes via DMAWrite.
  // Transfer address is in 8-byte units shifted right 3; the core takes
  // the raw byte address in register 0x1A6/0x1A8. Simpler path: write
  // directly into GetWritableRAM().
  auto& ram = SPU::GetWritableRAM();
  const u32 mask = static_cast<u32>(ram.size()) - 1u;
  for (u32 i = 0; i < size; ++i)
    ram[(dest + i) & mask] = data[i];
}

void ds_spu_dma_read(u8* out, u32 src, u32 size)
{
  // The data side of a DMA4 read (the HAL owns its timing): SPU RAM as the
  // core holds it, including the capture buffers it writes while mixing.
  const auto& ram = SPU::GetWritableRAM();
  const u32 mask = static_cast<u32>(ram.size()) - 1u;
  for (u32 i = 0; i < size; ++i)
    out[i] = ram[(src + i) & mask];
}

void ds_spu_mix(s16* out, int num_samples)
{
  if (num_samples <= 0 || !out)
    return;

  std::memset(out, 0, static_cast<size_t>(num_samples) * 2 * sizeof(s16));

  if (!g_sink)
    return;

  // Point the sink at the caller's output buffer, drive the SPU forward by
  // exactly num_samples frames, then detach. SPU::GenerateSamples() calls
  // SPU::Execute internally with num_samples * 768 CPU ticks, which runs the
  // full voice mixer + ADSR + reverb + CD-audio + capture-buffer pipeline.
  g_sink->SetSink(out, static_cast<u32>(num_samples));
  SPU::GenerateSamples(static_cast<u32>(num_samples));

  // If Execute produced fewer frames than requested (e.g. audio_output_muted
  // routed to null_audio_stream), the caller's buffer was already zeroed.
  g_sink->SetSink(nullptr, 0);
}

// Advance the SPU's internal tick clock by N CPU ticks. This is a legacy
// entry point kept for anyone who might have been calling it explicitly;
// ds_spu_mix now drives the core directly via SPU::GenerateSamples so this
// is a no-op. Left in place to keep the ABI stable.
// tokimeki-pc music split (upstream/spu.cpp "PC patches"): like ds_spu_mix,
// and while the split is on `music` receives the masked voices' share
// (zeros otherwise).  `music` may be null.
void ds_spu_music_capture(s16* music_frames);
void ds_spu_mix_split(s16* out, s16* music, int num_samples)
{
  if (num_samples <= 0 || !out)
    return;
  if (music)
    std::memset(music, 0, static_cast<size_t>(num_samples) * 2 * sizeof(s16));
  ds_spu_music_capture(music);
  ds_spu_mix(out, num_samples);
  ds_spu_music_capture(nullptr);
}

void ds_spu_advance_ticks(u32 cpu_ticks)
{
  (void)cpu_ticks;
}

// Direct generate-N-frames entry point for hosts that prefer explicit
// control (same contract as ds_spu_mix but without memset / sink-swap).
void ds_spu_generate_samples(int num_sample_frames)
{
  if (num_sample_frames <= 0)
    return;
  SPU::GenerateSamples(static_cast<u32>(num_sample_frames));
}

} // extern "C"
