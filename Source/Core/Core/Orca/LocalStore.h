// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace Orca::LocalStore
{
bool SeenIn(std::string_view text, std::string_view key);
std::string SeenWith(std::string text, std::string_view key);
std::string SeenPath();
bool Seen(std::string_view key);
bool MarkSeen(std::string_view key);

std::string ControlsPath();
std::optional<std::string> KeptControls();
bool KeepControls(std::string_view text);
}
