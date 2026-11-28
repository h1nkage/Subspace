// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <picojson.h>

namespace Orca::Engine
{
std::span<const std::string_view> SchemaKeywords();
std::optional<std::string> SchemaProblem(const picojson::value& schema);
std::optional<std::string> SchemaMismatch(const picojson::value& schema,
                                          const picojson::value& document);
}
