/* SPDX-License-Identifier: GPL-3.0-or-later */
// Minimal MediaCapture stub. System::GetMediaCapture() always returns null
// in the standalone library, so DeliverAudioFrames is never invoked.
#pragma once
#include "types.h"

class MediaCapture
{
public:
  bool DeliverAudioFrames(const s16* /*frames*/, u32 /*count*/) { return true; }
};
