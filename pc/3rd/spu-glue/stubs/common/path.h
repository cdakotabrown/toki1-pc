/* SPDX-License-Identifier: GPL-3.0-or-later */
// Minimal Path namespace stub (only ReplaceExtension is called by spu.cpp).
#pragma once
#include "types.h"
#include <string>
#include <string_view>

namespace Path {

inline std::string ReplaceExtension(std::string_view path, std::string_view new_ext)
{
  std::string out(path);
  const size_t dot = out.find_last_of('.');
  if (dot != std::string::npos)
    out.resize(dot);
  out.push_back('.');
  out.append(new_ext);
  return out;
}

} // namespace Path
