// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"
#include "Core/Orca/UX/RankedSteps.h"

namespace Orca::UX::Rulesets
{
const Ranked::Ruleset& Brawl2025();
const Ranked::Ruleset& Brawl2026();
const Ranked::Ruleset& RulesFor(bool casual);
const Ranked::Ruleset& PPlus2024();
const Ranked::Ruleset* ByBlock(u8 block);
bool Loaded();
}
