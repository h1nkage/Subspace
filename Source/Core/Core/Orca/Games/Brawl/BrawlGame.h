// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <string_view>

#include "Core/Orca/Engine/Module.h"

namespace Orca
{
struct Profile;
}

namespace Orca::UX::BrawlGame
{
inline constexpr std::string_view BRAWL_REV2 = "brawl-rev2";
inline constexpr std::string_view BRAWL_REV2_PPLUS = "brawl-rev2-pplus";
inline constexpr std::string_view BRAWL_REV2_RESULTS = "brawl-rev2-results";
inline constexpr std::string_view BRAWL_REV2_LAUNCHER = "brawl-rev2-launcher";

inline constexpr int BRAWL_REV2_COMPAT = 2;
inline constexpr int BRAWL_REV2_PPLUS_COMPAT = 2;
inline constexpr int BRAWL_REV2_RESULTS_COMPAT = 1;
inline constexpr int BRAWL_REV2_LAUNCHER_COMPAT = 0;

Orca::Engine::Manifest Identify(const Orca::Profile* profile);
bool ScenesReadable(const Orca::Profile* profile, std::string_view running_game,
                    u16 running_revision);
std::span<const Orca::Engine::Region> Regions();
bool UsesFreeSpace(const Orca::Engine::Manifest& game);
bool On(const Orca::Engine::Manifest& game);
}
