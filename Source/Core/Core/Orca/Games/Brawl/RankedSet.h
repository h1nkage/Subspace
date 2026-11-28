// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "Common/CommonTypes.h"
#include "Core/Orca/Engine/Display.h"
#include "Core/Orca/Games/Brawl/Results.h"
#include "Core/Orca/Games/Brawl/SetBlock.h"

namespace Core
{
class CPUThreadGuard;
}

namespace Orca::UX::Ranked
{
struct Ruleset;
}

namespace Orca::UX::RankedSet
{

struct Live
{
  bool valid = false;
  bool time_up = false;
  std::array<u8, 2> stocks{0, 0};
  std::array<u16, 2> percent{0, 0};
  std::array<bool, 2> on_ledge{false, false};
  std::array<u32, 2> status{0, 0};
  bool operator==(const Live&) const = default;
};

struct Facts
{
  Reading::Scene scene = Reading::Scene::Other;
  ResultBlock block;
  u8 decision = 0;
  Live live;
  bool live_read = false;
};

std::optional<SetBlock::GameRecord> Judge(SetBlock::Ruleset ruleset, const SetBlock::SetState& set,
                                          const ResultBlock& block, u8 decision);

SetBlock::SetState Step(SetBlock::SetState set, SetBlock::Ruleset ruleset, const Facts& facts,
                        u32 frame);

int WriteSetRules(GuestMemory& memory, const SetBlock::SetState& set, const Ranked::Ruleset& rules,
                  std::optional<std::pair<u8, u8>> stocks_minutes = std::nullopt);

Live ReadLiveFight(const GuestMemory& memory);

Facts ReadFacts(const GuestMemory& memory, bool live);

void Frame(const Core::CPUThreadGuard& guard, int frame, bool resimulating);

std::string ScoreLine(const SetBlock::SetState& set, int local_port, bool results = false);
std::string ResultToast(std::string_view out, int mine, int theirs);
std::string CurrentScoreLine();
void SetLocalPort(int port);

using Dots = Orca::Engine::SetDots;
Dots DotsOf(const SetBlock::SetState& set);
Dots CurrentDots();

u8 ReadHudPorts(const GuestMemory& memory);
std::optional<u32> ReadFightClock(const GuestMemory& memory);
std::array<float, 4> ReadHudFade(const GuestMemory& memory);
void HudFrame(const Core::CPUThreadGuard& guard, bool resimulating);
u8 HudPorts();
std::array<u8, 4> HudFade();
}
