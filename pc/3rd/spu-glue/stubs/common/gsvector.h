/* SPDX-License-Identifier: GPL-3.0-or-later */
// Scalar stand-in for the upstream GSVector4i SIMD type.
// spu.cpp uses a tiny subset: load/loadl, mul32l, add32, sll32, sra32,
// uph64, s16to32, and addv_s32. We implement all of them scalar-style.
// Matches the s32x4 lane semantics of the original exactly for signed values.
#pragma once
#include "types.h"
#include <algorithm>
#include <array>
#include <cstring>

#ifndef VECTOR_ALIGNMENT
#define VECTOR_ALIGNMENT 16
#endif

struct alignas(16) GSVector4i
{
  s32 v[4];

  GSVector4i() { v[0] = v[1] = v[2] = v[3] = 0; }
  GSVector4i(s32 a, s32 b, s32 c, s32 d) { v[0] = a; v[1] = b; v[2] = c; v[3] = d; }

  // load<aligned>(const s32* src) -- 4x s32 load.
  template <bool Aligned>
  static GSVector4i load(const s32* src)
  {
    (void)Aligned;
    GSVector4i r;
    std::memcpy(r.v, src, sizeof(r.v));
    return r;
  }

  // load<aligned>(const s16* src) -- used by spu.cpp to load 8 halfwords; the
  // code immediately does sll32(16).sra32(16) to re-extract the low s16 of
  // each s32 lane, so we simulate by loading the low 4 s16 into lanes.
  template <bool Aligned>
  static GSVector4i load(const s16* src)
  {
    (void)Aligned;
    GSVector4i r;
    // Two s16 per s32 lane: low=src[2i], high=src[2i+1].
    for (int i = 0; i < 4; ++i)
    {
      const u16 lo = static_cast<u16>(src[2 * i]);
      const u16 hi = static_cast<u16>(src[2 * i + 1]);
      r.v[i] = static_cast<s32>(static_cast<u32>(lo) | (static_cast<u32>(hi) << 16));
    }
    return r;
  }

  // loadl(const s16* src) -- loads 4 s16 into the low 2 s32 lanes
  // (each holding 2 packed s16), high lanes zero.
  static GSVector4i loadl(const s16* src)
  {
    GSVector4i r;
    for (int i = 0; i < 2; ++i)
    {
      const u16 lo = static_cast<u16>(src[2 * i]);
      const u16 hi = static_cast<u16>(src[2 * i + 1]);
      r.v[i] = static_cast<s32>(static_cast<u32>(lo) | (static_cast<u32>(hi) << 16));
    }
    r.v[2] = 0;
    r.v[3] = 0;
    return r;
  }

  // sll32(n): shift each lane left by n.
  GSVector4i sll32(int n) const
  {
    return GSVector4i(v[0] << n, v[1] << n, v[2] << n, v[3] << n);
  }

  // sra32(n): arithmetic shift right.
  GSVector4i sra32(int n) const
  {
    return GSVector4i(v[0] >> n, v[1] >> n, v[2] >> n, v[3] >> n);
  }

  // mul32l(other): lane-wise 32-bit multiply, keep low 32 bits.
  GSVector4i mul32l(const GSVector4i& o) const
  {
    return GSVector4i(
        static_cast<s32>(static_cast<u32>(v[0]) * static_cast<u32>(o.v[0])),
        static_cast<s32>(static_cast<u32>(v[1]) * static_cast<u32>(o.v[1])),
        static_cast<s32>(static_cast<u32>(v[2]) * static_cast<u32>(o.v[2])),
        static_cast<s32>(static_cast<u32>(v[3]) * static_cast<u32>(o.v[3])));
  }

  // add32(other): lane-wise add.
  GSVector4i add32(const GSVector4i& o) const
  {
    return GSVector4i(v[0] + o.v[0], v[1] + o.v[1], v[2] + o.v[2], v[3] + o.v[3]);
  }

  // uph64(): broadcast upper 64 bits to low (pack two high s32 lanes into
  // low lanes, then the low-half re-interpreted as 2x s32 each holding 2
  // packed s16 becomes available to s16to32()).
  GSVector4i uph64() const
  {
    return GSVector4i(v[2], v[3], v[2], v[3]);
  }

  // s16to32(): treat each of the low 4 packed s16 as an s32. In practice
  // spu.cpp follows load(s16) with s16to32() to finish sign-extending. Our
  // load already packed 2 s16 per lane; s16to32 unpacks the low halfword.
  GSVector4i s16to32() const
  {
    return GSVector4i(
        static_cast<s32>(static_cast<s16>(v[0] & 0xFFFF)),
        static_cast<s32>(static_cast<s16>((v[0] >> 16) & 0xFFFF)),
        static_cast<s32>(static_cast<s16>(v[1] & 0xFFFF)),
        static_cast<s32>(static_cast<s16>((v[1] >> 16) & 0xFFFF)));
  }

  // addv_s32(): horizontal reduction.
  s32 addv_s32() const { return v[0] + v[1] + v[2] + v[3]; }
};
