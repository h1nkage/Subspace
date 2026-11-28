// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "Core/Orca/UX/RankedSteps.h"

namespace Orca::Engine
{
std::string_view RulesetSchemaText();
std::optional<std::string> RulesetSchemaProblem();

std::optional<UX::Ranked::Ruleset> ReadRuleset(std::string_view json, std::string* error);
}
