// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Games/Brawl/BrawlGame.h"

#include <array>
#include <string_view>

#include "Core/Orca/Games/Brawl/FreeSpace.h"
#include "Core/Orca/Mod.h"
#include "Core/Orca/Profile.h"

namespace Orca::UX::BrawlGame
{
namespace
{
constexpr std::string_view BRAWL_DISC = "RSBE01";
constexpr u16 BRAWL_REVISION = 2;

bool IsPPlusProfile(std::string_view id)
{
  return id == "PPLUS32" || id == "PPLUS321";
}

Orca::Engine::Manifest Game(std::string_view id, Orca::Engine::Ruleset ruleset, int compat)
{
  Orca::Engine::Manifest game;
  game.id = id;
  game.ruleset = ruleset;
  game.results = true;
  game.compat = compat;
  return game;
}
}

Orca::Engine::Manifest Identify(const Orca::Profile* profile)
{
  Orca::Engine::Manifest game;
  if (profile && profile->mod)
  {
    switch (profile->mod->manifest.builtin)
    {
    case Orca::Mod::Builtin::BrawlRev2:
      game = Game(BRAWL_REV2, Orca::Engine::Ruleset::Brawl, BRAWL_REV2_COMPAT);
      break;
    case Orca::Mod::Builtin::BrawlRev2PPlus:
      game = Game(BRAWL_REV2_PPLUS, Orca::Engine::Ruleset::PPlus, BRAWL_REV2_PPLUS_COMPAT);
      break;
    case Orca::Mod::Builtin::BrawlRev2Results:
      game = Game(BRAWL_REV2_RESULTS, Orca::Engine::Ruleset::None, BRAWL_REV2_RESULTS_COMPAT);
      game.results = profile->mod->manifest.ranked_rules.has_value();
      break;
    default:
      break;
    }
  }
  else if (profile && profile->revision == BRAWL_REVISION)
  {
    if (!profile->IsLauncher() && profile->game_id == BRAWL_DISC)
      game = Game(BRAWL_REV2, Orca::Engine::Ruleset::Brawl, BRAWL_REV2_COMPAT);
    else if (profile->IsLauncher() && profile->disc == BRAWL_DISC)
    {
      game = IsPPlusProfile(profile->game_id) ?
                 Game(BRAWL_REV2_PPLUS, Orca::Engine::Ruleset::PPlus, BRAWL_REV2_PPLUS_COMPAT) :
                 Game(BRAWL_REV2_LAUNCHER, Orca::Engine::Ruleset::None, BRAWL_REV2_LAUNCHER_COMPAT);
    }
  }
  game.music =
      profile && (profile->IsLauncher() ? profile->disc : profile->game_id) == BRAWL_DISC;
  return game;
}

bool ScenesReadable(const Orca::Profile* profile, std::string_view running_game,
                    u16 running_revision)
{
  if (running_game == BRAWL_DISC && running_revision == BRAWL_REVISION)
    return true;
  if (profile && profile->mod && !Identify(profile).id.empty())
    return true;
  return profile && profile->IsLauncher() && profile->disc == BRAWL_DISC &&
         profile->revision == BRAWL_REVISION;
}

std::span<const Orca::Engine::Region> Regions()
{
  using Orca::Engine::RegionKind;
  static constexpr std::array<Orca::Engine::Region, 12> REGIONS{{
      {0x80002810, 0x80002814, RegionKind::Data, "pplus-music-step"},
      {0x80002818, 0x8000282C, RegionKind::Data, "pplus-music-selection"},
      {0x80002830, 0x80002840, RegionKind::Data, "pplus-music-return"},
      {0x8042C4E8, 0x8042C840, RegionKind::Data, "pplus-stage-data"},
      {0x804E0000, 0x804E2520, RegionKind::Data, "pplus-code-menu"},
      {0x805A00B8, 0x805A00C0, RegionKind::Data, "rng-default"},
      {0x805A0420, 0x805A0428, RegionKind::Data, "rng-menu"},
      {0x805B8A08, 0x805B8A0C, RegionKind::Data, "pplus-game-pause"},
      {0x805B8E24, 0x805B8E2C, RegionKind::Data, "scene-manager-exit"},
      {FreeSpace::kMatchBlock.begin, FreeSpace::kMatchBlockLimit, RegionKind::Scratch,
       "match-block"},
      {0x81200000, 0x817C0000, RegionKind::Data, "mem1-heap"},
      {0x90000800, 0x93600000, RegionKind::Data, "mem2-heap"},
  }};
  return REGIONS;
}

bool UsesFreeSpace(const Orca::Engine::Manifest& game)
{
  return !game.id.empty() && game.id != BRAWL_REV2_RESULTS;
}

bool On(const Orca::Engine::Manifest& game)
{
  if (game.id.empty())
    return false;
  if (Orca::Mod::ActiveMod())
    return Orca::Mod::BuiltinOn(Orca::Mod::DeclaredBuiltin());
  return true;
}
}
