/* SPDX-License-Identifier: GPL-3.0-or-later */
// Minimal fmt::format stub. Returns an empty string -- spu.cpp only uses it
// to compose an error message for Host::ReportErrorAsync, which is a no-op
// in the standalone library.
#pragma once
#include <string>
#include <string_view>

namespace fmt {

template <typename... Args>
inline std::string format(std::string_view /*fmt*/, Args&&... /*args*/)
{
  return std::string();
}

} // namespace fmt
