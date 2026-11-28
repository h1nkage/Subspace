// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <picojson.h>

namespace Orca::Engine
{
std::optional<picojson::value> ReadStrictJson(std::string_view text, size_t max_bytes,
                                              int max_depth, std::string* error);
}
