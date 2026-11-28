// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdlib>
#include <string_view>

namespace InputCommon
{
inline bool VirtualPadOnly()
{
  const char* value = std::getenv("ORCA_VIRTUAL_PAD_ONLY");
  return value && std::string_view(value) == "1";
}

inline bool LocalInputDisabled()
{
  const char* value = std::getenv("ORCA_NO_LOCAL_INPUT");
  return (value && std::string_view(value) == "1") || VirtualPadOnly();
}
}
