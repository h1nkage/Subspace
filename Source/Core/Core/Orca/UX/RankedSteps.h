// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

namespace Orca::UX::Ranked
{
using StageMask = u16;
constexpr int MAX_STAGES = 16;

constexpr s8 PREF_NONE = -1;
constexpr s8 PREF_RANDOM = -2;
constexpr bool Named(s8 pref)
{
  return pref >= 0 || pref == PREF_RANDOM;
}

enum class StepKind : u8
{
  Strike = 1,
  Ban = 2,
  Pick = 3,
  Prefer = 4,
};

constexpr u8 BOTH = 2;

struct Step
{
  StepKind kind = StepKind::Pick;
  u8 port = 0;
  u8 count = 1;
  bool optional = false;
  u16 seconds = 30;
  bool operator==(const Step&) const = default;
};

struct Turn
{
  StepKind kind = StepKind::Pick;
  u8 port = 0;
  int left = 1;
  bool operator==(const Turn&) const = default;
};

enum class TimeOut : u8
{
  Results,
  Judged,
};

struct LedgeLimits
{
  struct Character
  {
    int character = -1;
    u8 limit = 0;
    u8 tiebreak_limit = 0;
    bool operator==(const Character&) const = default;
  };
  u8 limit = 0;
  u8 tiebreak_limit = 0;
  std::array<Character, 4> characters{};
  u8 character_count = 0;

  int For(bool tiebreak, int character) const
  {
    for (u8 i = 0; i < character_count && i < characters.size(); ++i)
    {
      if (characters[i].character == character)
        return tiebreak ? characters[i].tiebreak_limit : characters[i].limit;
    }
    return tiebreak ? tiebreak_limit : limit;
  }
  bool operator==(const LedgeLimits&) const = default;
};

struct Ruleset
{
  u8 id = 0;
  std::string slug;
  std::string name;
  int stage_count = 0;
  StageMask starters = 0;
  StageMask counterpicks = 0;
  int wins_needed = 2;
  StepKind later_kind = StepKind::Strike;
  u8 later_count = 2;
  bool later_optional = false;
  bool dsr = true;
  int ic_banned_stage = -1;
  int ice_climbers = -1;
  int meta_knight = -1;
  std::array<u16, 3> game1_seconds{30, 30, 10};
  u16 later_seconds = 30;
  u16 pick_seconds = 30;
  u16 grace_frames = 180;
  u16 casual_seconds = 15;
  u16 casual_grace_frames = 30;
  std::array<std::string, MAX_STAGES> stage_names{};
  std::array<u8, MAX_STAGES> stage_kinds{};

  u8 stocks = 3;
  u8 minutes = 8;
  TimeOut time_out = TimeOut::Results;
  LedgeLimits ledge{};
  bool tiebreak = false;
  u8 tiebreak_stocks = 1;
  u8 tiebreak_minutes = 3;
  u8 void_after_ties = 5;
  u16 css_ready_seconds = 60;
  u16 queue_ready_seconds = 30;
  u16 queue_skip_frames = 90;
  u16 order_pick_seconds = 45;

  bool SetWritesGameRules() const { return tiebreak; }

  StageMask All() const { return static_cast<StageMask>((1u << stage_count) - 1); }
  bool operator==(const Ruleset&) const = default;
};

constexpr u8 RULESET_NONE = 0;
constexpr u8 RULESET_BRAWL_SUPERNOVA_2025 = 1;
constexpr u8 RULESET_PPLUS_2024 = 2;

struct SetView
{
  int game = 1;
  int last_winner = -1;
  int coin = 0;
  std::array<StageMask, 2> won_on{};
  std::array<int, 2> characters{-1, -1};
  bool operator==(const SetView&) const = default;
};

struct Plan
{
  std::vector<Step> steps;
  StageMask available = 0;
  StageMask pick_excluded = 0;
  bool mk_clause = false;
  bool casual = false;
  u32 seed = 0;
  StageMask agreeable = 0;
};

Plan PlanGame(const Ruleset& rules, const SetView& set);

u32 CasualSeed(u32 room, u8 coin, int game);
Plan PlanCasual(const Ruleset& rules, const SetView& set, u32 seed);
int CasualDefault(const Plan& plan, int port);
int CasualChoice(const Plan& plan);
int CasualResult(const Plan& plan, std::array<s8, 2> prefs);

StageMask Allowed(const Plan& plan, int step, StageMask struck);
StageMask DefaultStrikes(const Plan& plan, int step, StageMask struck, int done);
int DefaultPick(const Plan& plan, int step, StageMask struck);
u16 GraceFrames(const Ruleset& rules, const Step& step);
u32 StepFrames(const Ruleset& rules, const Step& step);

int CountBits(StageMask mask);
StageMask FirstBits(StageMask mask, int n);

struct Progress
{
  u8 step = 0;
  u8 done = 0;
  StageMask struck = 0;
  u16 frames = 0;
  s8 picked = -1;
  std::array<s8, 2> prefs{PREF_NONE, PREF_NONE};
  bool operator==(const Progress&) const = default;
};

Progress Begin(const Ruleset& rules, const Plan& plan);
const Step* Current(const Plan& plan, const Progress& progress);
StageMask Allowed(const Plan& plan, const Progress& progress);
bool Strike(const Ruleset& rules, const Plan& plan, Progress* progress, int stage);
bool Skip(const Ruleset& rules, const Plan& plan, Progress* progress);
bool Pick(const Plan& plan, Progress* progress, int stage);
bool Propose(const Plan& plan, Progress* progress, int port, int stage);
bool Withdraw(Progress* progress, int port);
StageMask Proposable(const Plan& plan, const Progress& progress);
void AutoPick(const Plan& plan, Progress* progress);
void Tick(const Ruleset& rules, const Plan& plan, Progress* progress);
int SecondsShown(const Ruleset& rules, u16 frames);
int SecondsShown(const Ruleset& rules, const Step& step, u16 frames);
}
