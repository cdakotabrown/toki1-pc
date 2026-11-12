/* SPDX-License-Identifier: GPL-3.0-or-later */
// Minimal InlineFIFOQueue stub for SPU transfer_fifo.
// Only provides the API surface that spu.cpp uses.
#pragma once
#include "types.h"
#include "assert.h"
#include <array>
#include <cstring>
#include <algorithm>

template <typename T, u32 CAPACITY>
class InlineFIFOQueue
{
public:
  InlineFIFOQueue() { Clear(); }

  void Clear()
  {
    m_head = 0;
    m_tail = 0;
    m_size = 0;
  }

  u32 GetSize() const { return m_size; }
  u32 GetSpace() const { return CAPACITY - m_size; }
  bool IsEmpty() const { return m_size == 0; }
  bool IsFull() const { return m_size == CAPACITY; }

  void Push(const T& v)
  {
    if (m_size >= CAPACITY) return;
    m_buffer[m_tail] = v;
    m_tail = (m_tail + 1) % CAPACITY;
    ++m_size;
  }

  T Pop()
  {
    if (m_size == 0) return T{};
    T v = m_buffer[m_head];
    m_head = (m_head + 1) % CAPACITY;
    --m_size;
    return v;
  }

  void PushRange(const T* src, u32 count)
  {
    for (u32 i = 0; i < count && m_size < CAPACITY; ++i)
      Push(src[i]);
  }

  void PopRange(T* dst, u32 count)
  {
    for (u32 i = 0; i < count; ++i)
      dst[i] = Pop();
  }

private:
  std::array<T, CAPACITY> m_buffer{};
  u32 m_head = 0;
  u32 m_tail = 0;
  u32 m_size = 0;
};

// Full FIFOQueue alias for generic serializer paths (unused at runtime,
// but StateWrapper::Do<FIFOQueue<...>> template may require the name).
template <typename T, u32 CAPACITY>
using FIFOQueue = InlineFIFOQueue<T, CAPACITY>;
