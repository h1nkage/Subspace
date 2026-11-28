// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include <imgui.h>

#include "Core/Orca/UX/Kit.h"
#include "Core/Orca/UX/AppOrb.h"

namespace Orca::UX::AppOrb
{
Kit::Box ButtonBox(const Model::View& view);
void DrawButton(ImDrawList* dl, const Model::View& view);
void DrawHostControls(ImDrawList* dl, const Model::View& view);
struct PillRow
{
  enum class Type
  {
    Chat,
    Toast,
    Notice,
    Talk,
  };
  Type type = Type::Chat;
  std::string_view name;
  std::string_view text;
  std::string_view key;
  std::string_view verb;
  Kind kind = Kind::Join;
  float alpha = 1;
  double at = 0;
  Talk::Phase talk = Talk::Phase::Off;
};
inline constexpr std::size_t PILL_ROWS = 3;

Kit::Box DrawPill(ImDrawList* dl, const Model::View& view, std::span<const PillRow> rows,
                  double now_ms, float right_limit);

template <typename Measure>
std::size_t FitPrefix(std::string_view text, float width, Measure&& measure)
{
  if (measure(text) <= width)
    return text.size();
  const auto continuation = [&](std::size_t i) {
    return i < text.size() && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80;
  };
  std::size_t lo = 0, hi = text.size();
  while (true)
  {
    std::size_t mid = lo + (hi - lo) / 2;
    while (mid > lo && continuation(mid))
      --mid;
    if (mid == lo)
    {
      mid = lo + 1;
      while (mid < hi && continuation(mid))
        ++mid;
    }
    if (mid >= hi)
      break;
    if (measure(text.substr(0, mid)) <= width)
      lo = mid;
    else
      hi = mid;
  }
  if (lo < text.size() && text[lo] == ' ')
    return lo;
  const std::size_t space = text.substr(0, lo).rfind(' ');
  if (space != std::string_view::npos && space > lo / 2)
    return space;
  return lo;
}

template <typename Measure>
std::size_t FitSuffix(std::string_view text, float width, Measure&& measure)
{
  if (measure(text) <= width)
    return 0;
  const auto continuation = [&](std::size_t i) {
    return i < text.size() && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80;
  };
  std::size_t lo = 0, hi = text.size();
  while (true)
  {
    std::size_t mid = lo + (hi - lo) / 2;
    while (mid > lo && continuation(mid))
      --mid;
    if (mid == lo)
    {
      mid = lo + 1;
      while (mid < hi && continuation(mid))
        ++mid;
    }
    if (mid >= hi)
      break;
    if (measure(text.substr(mid)) <= width)
      hi = mid;
    else
      lo = mid;
  }
  if (hi > 0 && text[hi - 1] == ' ')
    return hi;
  const std::size_t space = text.find(' ', hi);
  if (space != std::string_view::npos && space + 1 < text.size() &&
      space - hi < (text.size() - hi) / 2)
    return space + 1;
  return hi;
}
}
