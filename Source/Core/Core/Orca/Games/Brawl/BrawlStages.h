// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/UX/RankedSteps.h"
#include "Core/Orca/Games/Brawl/StageCursors.h"
#include "Core/Rollback/InputGate.h"

namespace Core
{
class CPUThreadGuard;
}

namespace Orca::UX
{
class GuestMemory;
}

namespace Orca::UX::BrawlStages
{
enum Stage : u8
{
  Battlefield,
  PokemonStadium,
  LylatCruise,
  Smashville,
  YoshisIsland,
  FinalDestination,
  DelfinoPlaza,
  kStages,
};
constexpr u8 FrigateOrpheon = kStages;
constexpr u8 kRankedStages = kStages + 1;

constexpr u32 SCENE_MANAGER = 0x805A0060;
constexpr u32 PAD_SYSTEM = 0x805A0040;
constexpr u32 RNG_SEED = 0x805A00BC;
constexpr u32 GAME_GLOBAL = 0x805A00E0;
constexpr u32 PAD_SYSTEM_PADS = 0x40;
constexpr u32 PAD_STRIDE = 0x40;
constexpr u16 GF_X = 0x0400;
constexpr u16 GF_Y = 0x0800;
constexpr u32 SCENE_SSS_TASK = 0x3AC;
constexpr u32 TASK_ICONS = 0x8C;
constexpr u32 TASK_HOVER = 0x244;
constexpr u32 TASK_SELECTED = 0x248;
constexpr u32 TASK_PORT = 0x278;
constexpr u32 PORT_ANY = 0xF0;

constexpr u32 kBlock = 0x806F2380;
constexpr u32 kHeaderMagic = 0x53534D31;
constexpr u8 kHeaderVersion = 1;
constexpr u8 kModeCasual = 1;
constexpr u8 kModeRanked = 2;
constexpr u8 kRulesetBrawl = 1;
constexpr u32 kResultsSet = kBlock + 0x210;
constexpr u32 kRegion = kBlock + 0x80;
constexpr u32 kRegionSize = 0x40;
constexpr u32 kMagic = 0x53534253;
constexpr u8 kVersion = 1;

enum class Phase : u8
{
  Idle = 0,
  Select = 1,
  Fight = 2,
  Recorded = 3,
};

struct State
{
  Ranked::SetView set;
  std::array<u8, 2> score{};
  u8 coin = 0xFF;
  Phase phase = Phase::Idle;
  Ranked::Progress progress;
  std::array<s8, 2> characters{-1, -1};
  std::array<u16, 2> buttons{};
  u32 clock = 0;
  s8 hovered = -1;
  u8 hovered_for = 0;
  u8 hover_key = 0xFF;
  u32 seed = 0;
  bool operator==(const State&) const = default;
};
std::optional<State> ReadState(const GuestMemory& memory);
void WriteState(GuestMemory& memory, const State& state);
void ClearState(GuestMemory& memory);

struct Tile
{
  u8 page;
  u8 slot;
  float x;
  float y;
};
const Tile& TileFor(int stage);

int StageForStageId(int stage_id, bool casual = false);

struct View
{
  bool present = false;
  u32 task = 0;
  u32 hovered_item = 0xFFFFFFFF;
  s32 selected = -1;
  u32 clock = 0;
};
View ReadView(const GuestMemory& memory);

bool RankedBrawlMatch(const GuestMemory& memory);
bool CasualBrawlMatch(const GuestMemory& memory);
Ranked::SetView SetOf(const GuestMemory& memory, const State& state,
                           std::array<u8, 2>* score = nullptr);
void SetRankedForTests(std::optional<bool> ranked);
void SetCasualForTests(std::optional<bool> casual);

void Frame(GuestMemory& memory, bool two_players = true);
void Masks(const GuestMemory& memory, Rollback::InputGate::Masks* masks);
std::pair<std::string, std::string> Lines(const GuestMemory& memory,
                                          const std::vector<Orca::Events::PortInfo>& ports);

std::optional<Ranked::Turn> CurrentTurn(const GuestMemory& memory);

StageCursors::View CursorView(const GuestMemory& memory,
                              const std::vector<Orca::Events::PortInfo>& ports);

void FrameHook(const Core::CPUThreadGuard& guard, bool resimulating,
               const std::vector<Orca::Events::PortInfo>& ports);
void GateMasks(const Core::CPUThreadGuard& guard, Rollback::InputGate::Masks* masks);
}
