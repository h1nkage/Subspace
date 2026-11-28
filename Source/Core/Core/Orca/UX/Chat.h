// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace Orca::UX::Chat
{
inline constexpr std::size_t NAME_POINTS = 24;
inline constexpr std::size_t TEXT_POINTS = 140;
inline constexpr std::size_t NAME_ENCODED = NAME_POINTS * 12;
inline constexpr std::size_t TEXT_ENCODED = TEXT_POINTS * 12;

struct Line
{
  std::string name;
  std::string text;
};

std::optional<std::string> DecodeField(std::string_view field, std::size_t max_encoded,
                                       std::size_t max_points);

std::optional<Line> Parse(std::string_view line);
}
