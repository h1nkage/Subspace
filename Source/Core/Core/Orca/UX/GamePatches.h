// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

namespace Core
{
class CPUThreadGuard;
}

namespace Orca::UX
{
class GuestMemory;

struct PatchCondition
{
  u32 address = 0;
  u32 mask = 0xFFFFFFFF;
  u32 value = 0;
  bool equal = true;

  bool operator==(const PatchCondition&) const = default;
};

struct GamePatch
{
  u32 address = 0;
  std::optional<u32> original;
  u32 value = 0;
  bool joins_previous = false;
  std::vector<PatchCondition> when;
};

std::optional<std::vector<GamePatch>> ParseGamePatches(std::string_view text, std::string* error);

bool GroupApplies(std::span<const GamePatch> group, std::span<const u32> now);

bool ConditionsHold(std::span<const PatchCondition> when, const GuestMemory& memory);

enum class Toggle
{
  Leave,
  On,
  Off,
};
Toggle ToggleGroup(std::span<const GamePatch> group, std::span<const u32> now, bool holds);

struct PatchWrite
{
  u32 address = 0;
  u32 value = 0;
  bool code = false;
};

struct PatchPlan
{
  struct Group
  {
    u32 address = 0;
    u32 words = 0;
    bool operator==(const Group&) const = default;
  };
  std::vector<PatchWrite> writes;
  std::vector<Group> landed;
  std::vector<Group> on;
  std::vector<Group> off;
};

PatchPlan PlanGamePatches(std::span<const GamePatch> patches, const GuestMemory& memory,
                          bool toggled);

void ApplyGamePatches(const Core::CPUThreadGuard& guard);
void ApplyToggledGamePatches(const Core::CPUThreadGuard& guard);
}
