/* SPDX-License-Identifier: GPL-3.0-or-later */
// Minimal Host stub. Only ReportErrorAsync is invoked by spu.cpp.
#pragma once
#include "types.h"
#include <string_view>

namespace Host {

inline void ReportErrorAsync(std::string_view /*title*/, std::string_view /*message*/) {}

} // namespace Host
