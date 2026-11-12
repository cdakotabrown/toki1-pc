/* SPDX-License-Identifier: GPL-3.0-or-later */
// Re-export upstream common/bitutils.h (vendored) and inject our scalar
// GSVector4i stand-in so spu.cpp's reverb code links. The upstream SIMD
// header (common/gsvector.h and its arch-specific variants) is not
// vendored; the scalar replacement is functionally equivalent for the
// 4-lane s32 convolution the SPU uses.
#pragma once
#include "upstream/bitutils.h"
#include "common/gsvector.h"
