// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Engine/MatchTypes.h"
#include "Core/Orca/Games/Brawl/SetBlock.h"
#include "Core/Orca/Mod.h"

namespace Core
{
class CPUThreadGuard;
}

namespace Orca::UX
{
class GuestMemory;

struct PortResult
{
  bool present = false;
  bool human = false;
  int character = -1;
  int stocks = -1;
  int place = -1;
  int team = -1;
  bool operator==(const PortResult&) const = default;
};

struct ResultBlock
{
  int mode = -1;
  int stage = -1;
  int end = 0;
  int decision = 0;
  std::array<PortResult, 4> ports{};
  std::string_view differing_rule;
  bool operator==(const ResultBlock&) const = default;
};

struct Reading
{
  using Scene = Orca::Engine::SceneKind;
  Scene scene = Scene::Other;
  ResultBlock block;
  std::optional<SetBlock::SetState> set;
  bool operator==(const Reading&) const = default;
};

Reading ReadResults(const GuestMemory& memory,
                    const std::optional<Orca::Mod::RankedRules>& ranked_rules = std::nullopt);

using Orca::Engine::GameResult;

GameResult FromRecord(const SetBlock::SetState& set, int index);

GameResult Judge(const ResultBlock& block, int frame);

class ResultsTracker
{
public:
  void Store(int frame, const Reading& reading, u64 resyncs);
  std::vector<GameResult> Confirm(int confirmed, int plug_frame,
                                  std::vector<int>* starts = nullptr);
  void Reset();
  void Rebase(int frame, const Reading& reading, u64 resyncs);
  std::optional<Reading::Scene> FinalScene() const;
  std::optional<Reading::Scene> LatestScene() const;

private:
  std::map<int, Reading> m_pending;
  u64 m_resyncs = 0;
  bool m_started = false;
  bool m_have_last = false;
  int m_last_frame = -1;
  Reading m_last;
  bool m_in_game = false;
  bool m_after_fight = false;
  int m_scene_start = -1;
  bool m_have_set = false;
  int m_set_games = 0;
  bool m_have_fight = false;
  int m_fight_start = -1;
};

ResultsTracker& Tracker();

void ReadResultsFrame(const Core::CPUThreadGuard& guard, int frame);

bool ResultsVerified();

void RequestQueueRules();
void ApplyQueueRules(const Core::CPUThreadGuard& guard, bool alone);
int WriteQueueRules(GuestMemory& memory);
}
