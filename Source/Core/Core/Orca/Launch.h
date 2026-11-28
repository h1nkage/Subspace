// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace Orca
{
inline constexpr std::string_view LAUNCH_INI = "orca-launch.ini";

struct LaunchSpec
{
  std::string profile;
  std::string executable;
};

std::optional<LaunchSpec> ParseLaunchIni(std::string_view text, std::string* error);

std::optional<LaunchSpec> ReadLaunchIni(const std::string& folder, std::string* error);
}
