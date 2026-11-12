/* SPDX-License-Identifier: GPL-3.0-or-later */
// Minimal Error stub -- just the class name so "Error* error = nullptr" style params type-check.
#pragma once
#include "types.h"
#include <string>
#include <string_view>

class Error
{
public:
  Error() = default;
  ~Error() = default;

  void Clear() {}
  bool IsValid() const { return false; }

  void SetString(std::string_view msg) { (void)msg; }
  void SetStringView(std::string_view msg) { (void)msg; }
  template <typename... Args>
  void SetStringFmt(Args&&...) {}

  const std::string& GetDescription() const
  {
    static const std::string empty;
    return empty;
  }
};
