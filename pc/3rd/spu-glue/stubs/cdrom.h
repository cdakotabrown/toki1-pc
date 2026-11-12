/* SPDX-License-Identifier: GPL-3.0-or-later */
// CDROM coupling point. spu.cpp calls CDROM::GetAudioFrame() once per
// 44.1 kHz output sample to fetch the CD-ROM controller's audio output (the
// XA-ADPCM / CD-DA stream after the controller's ATV volume matrix); the SPU
// mixes it with its CD volume and writes it to the capture buffers.
//
// The CD-ROM controller itself is the PC port's (pc/hal/cd_audio.c). The host
// registers it with spu_core_set_cd_audio_source (spu_stubs.cpp);
// without one the drive outputs silence, as an idle drive does.
#pragma once
#include "types.h"
#include <tuple>

extern "C" int (*spu_core_cd_audio_source)(s16* left, s16* right);

namespace CDROM {

inline std::tuple<s16, s16> GetAudioFrame()
{
  s16 left = 0, right = 0;
  if (spu_core_cd_audio_source && !spu_core_cd_audio_source(&left, &right))
    left = right = 0;
  return {left, right};
}

} // namespace CDROM
