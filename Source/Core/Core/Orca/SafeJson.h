// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace picojson
{
class value;
}

namespace Orca::Json
{
enum class NumberPolicy
{
  Room,
  Finite,
};

std::optional<std::string> SanitizeJson(std::string_view text,
                                      NumberPolicy numbers = NumberPolicy::Room);
bool ParseJson(const std::string& text, picojson::value* value,
               NumberPolicy numbers = NumberPolicy::Room);
std::string ParseWholeJson(std::string_view text, picojson::value* value);
}
