// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Games/Brawl/RankedSet.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <utility>

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Engine/GuestMemory.h"
#include "Core/Orca/Games/Brawl/OnlineMenu.h"
#include "Core/Orca/Games/Brawl/OnlineRules.h"
#include "Core/Orca/Games/Brawl/Rulesets.h"
#include "Core/Orca/UX/Probe.h"
#include "Core/Orca/UX/QueueApp.h"
#include "Core/Orca/UX/SetEnd.h"

namespace Orca::UX::RankedSet
{
namespace
{
using SetBlock::GameRecord;
using SetBlock::How;
using SetBlock::NO_PORT;
using SetBlock::Ruleset;
using SetBlock::SetState;
using Scene = Reading::Scene;

constexpr u32 GAME_GLOBAL = 0x805A00E0;
constexpr u32 GLOBAL_SET_RULE = 0x1C;
constexpr u32 RULE_MODE = 0x02;
constexpr u32 RULE_STOCKS = 0x04;
constexpr u32 RULE_STOCK_MINUTES = 0x08;
constexpr u8 DECISION_TIME_UP = 1;
constexpr u8 DECISION_NO_CONTEST = 9;

u32 SetRule(const GuestMemory& m)
{
  if (!m.Valid(GAME_GLOBAL))
    return 0;
  const u32 global = m.Read32(GAME_GLOBAL);
  if (global % 4 || !m.Valid(global + GLOBAL_SET_RULE))
    return 0;
  const u32 rule = m.Read32(global + GLOBAL_SET_RULE);
  return rule % 4 == 0 && m.Valid(rule) && m.Valid(rule + RULE_STOCK_MINUTES) ? rule : 0;
}

u8 Byte(int v)
{
  return v < 0 || v > 254 ? 0xFF : static_cast<u8>(v);
}

const Ranked::Ruleset* RulesOf(Ruleset ruleset)
{
  return Rulesets::ByBlock(static_cast<u8>(ruleset));
}
constexpr int NO_RULESET_WINS = 2;
constexpr int NO_RULESET_VOID_AFTER_TIES = 5;

bool Judged(const Ranked::Ruleset* rules)
{
  return rules && rules->time_out == Ranked::TimeOut::Judged;
}

std::atomic<u32> s_published{0};
std::atomic<int> s_local_port{0};
std::atomic<u32> s_dots{0};
}

std::optional<GameRecord> Judge(Ruleset ruleset, const SetState& set, const ResultBlock& block,
                                u8 decision)
{
  const auto& p = block.ports;
  if (decision == DECISION_NO_CONTEST || !p[0].present || !p[1].present || !p[0].human ||
      !p[1].human || p[2].present || p[3].present)
  {
    return std::nullopt;
  }
  GameRecord r;
  r.start = set.fight_start;
  r.stage = Byte(block.stage);
  r.tiebreak = set.tiebreak;
  r.ledge = set.ledge;
  for (int i = 0; i < 2; ++i)
  {
    r.characters[i] = Byte(p[i].character);
    r.stocks[i] = Byte(p[i].stocks);
  }
  const bool snapped = set.fight_flags & SetBlock::FIGHT_TIMEUP;
  const bool sudden = set.fight_flags & SetBlock::FIGHT_SUDDEN;
  r.timeout = snapped || decision == DECISION_TIME_UP;
  const bool first0 = p[0].place == 0, first1 = p[1].place == 0;
  const auto win = [&](int port, How how) {
    r.winner = static_cast<u8>(port);
    r.how = how;
    return r;
  };
  const auto tie = [&] {
    r.winner = NO_PORT;
    r.how = How::Tie;
    return r;
  };
  const Ranked::Ruleset* rules = RulesOf(ruleset);
  if (!Judged(rules))
  {
    if (first0 != first1)
      return win(first0 ? 0 : 1, How::Results);
    if (first0 && first1)
      return tie();
    return std::nullopt;
  }
  if (sudden && !snapped)
    return tie();
  if (r.timeout)
  {
    const bool over0 = set.ledge[0] > rules->ledge.For(set.tiebreak, r.characters[0]);
    const bool over1 = set.ledge[1] > rules->ledge.For(set.tiebreak, r.characters[1]);
    if (over0 != over1)
      return win(over0 ? 1 : 0, How::Ledge);
    std::array<int, 2> stocks{p[0].stocks, p[1].stocks};
    if (snapped)
      stocks = {set.snap_stocks[0], set.snap_stocks[1]};
    if (stocks[0] >= 0 && stocks[1] >= 0 && stocks[0] != stocks[1])
      return win(stocks[0] > stocks[1] ? 0 : 1, How::Stocks);
    if (snapped && set.snap_percent[0] != set.snap_percent[1])
      return win(set.snap_percent[0] < set.snap_percent[1] ? 0 : 1, How::Percent);
    if (!snapped && first0 != first1)
      return win(first0 ? 0 : 1, How::Results);
    return tie();
  }
  if (first0 != first1)
    return win(first0 ? 0 : 1, How::Results);
  if (first0 && first1)
    return tie();
  return std::nullopt;
}

SetState Step(SetState s, Ruleset ruleset, const Facts& f, u32 frame)
{
  const Ranked::Ruleset* rules = RulesOf(ruleset);
  const bool judged = Judged(rules);
  if (s.done)
  {
    if (s.fight)
    {
      s.fight = false;
      s.fight_flags = 0;
    }
    return s;
  }
  switch (f.scene)
  {
  case Scene::Fight:
    if (!s.fight)
    {
      s.fight = true;
      s.fight_flags = 0;
      s.fight_start = frame;
      s.ledge = {0, 0};
      s.snap_stocks = {0, 0};
      s.snap_percent = {0, 0};
      s.on_ledge = {0, 0};
    }
    else if (s.fight_flags & SetBlock::FIGHT_BETWEEN)
    {
      s.fight_flags =
          static_cast<u8>((s.fight_flags & ~SetBlock::FIGHT_BETWEEN) | SetBlock::FIGHT_SUDDEN);
    }
    if (f.live.valid && !judged)
    {
      s.fight_flags |= SetBlock::FIGHT_SEEN;
    }
    else if (f.live.valid)
    {
      if (s.fight_flags & SetBlock::FIGHT_GONE)
      {
        s.fight_flags = static_cast<u8>((s.fight_flags & ~SetBlock::FIGHT_GONE) |
                                        SetBlock::FIGHT_SUDDEN);
      }
      s.fight_flags |= SetBlock::FIGHT_SEEN;
      if (!(s.fight_flags & (SetBlock::FIGHT_SUDDEN | SetBlock::FIGHT_TIMEUP)))
      {
        for (int i = 0; i < 2; ++i)
        {
          if (f.live.on_ledge[i] && !s.on_ledge[i] && s.ledge[i] < 255)
            ++s.ledge[i];
          s.on_ledge[i] = f.live.on_ledge[i] ? 1 : 0;
        }
        if (f.live.time_up)
        {
          s.fight_flags |= SetBlock::FIGHT_TIMEUP;
          s.snap_stocks = f.live.stocks;
          s.snap_percent = f.live.percent;
        }
      }
    }
    else if (judged && f.live_read && (s.fight_flags & SetBlock::FIGHT_SEEN))
    {
      s.fight_flags |= SetBlock::FIGHT_GONE;
    }
    break;
  case Scene::Between:
    if (s.fight)
      s.fight_flags |= SetBlock::FIGHT_BETWEEN;
    break;
  case Scene::Results:
  {
    if (!s.fight)
      break;
    const std::optional<GameRecord> r = Judge(ruleset, s, f.block, f.decision);
    s.fight = false;
    s.fight_flags = 0;
    s.on_ledge = {0, 0};
    if (!r)
      break;
    if (s.games < SetBlock::MAX_RECORDS)
      s.records[s.games] = *r;
    ++s.games;
    if (r->winner != NO_PORT)
    {
      const int w = r->winner;
      ++s.wins[w];
      s.last_winner = r->winner;
      s.tiebreak = false;
      s.tb_streak = 0;
      if (s.wins[w] >= (rules ? rules->wins_needed : NO_RULESET_WINS))
      {
        s.done = 1;
        s.winner = r->winner;
      }
    }
    else
    {
      ++s.tb_streak;
      s.tiebreak = rules && rules->tiebreak;
      if (s.tb_streak >= (rules ? rules->void_after_ties : NO_RULESET_VOID_AFTER_TIES))
        s.done = 2;
    }
    if (!s.done && s.games >= SetBlock::MAX_RECORDS)
      s.done = 2;
    if (s.done == 2)
    {
      s.winner = NO_PORT;
      s.tiebreak = false;
    }
    break;
  }
  case Scene::Other:
    if (s.fight)
    {
      s.fight = false;
      s.fight_flags = 0;
      s.on_ledge = {0, 0};
    }
    break;
  }
  return s;
}

int WriteSetRules(GuestMemory& memory, const SetState& set, const Ranked::Ruleset& rules,
                  std::optional<std::pair<u8, u8>> stocks_minutes)
{
  const u8 stocks = stocks_minutes ? stocks_minutes->first : rules.stocks;
  const u8 minutes = stocks_minutes ? stocks_minutes->second : rules.minutes;
  const u32 rule = SetRule(memory);
  if (!rule)
    return 0;
  int changed = 0;
  const auto put = [&](u32 offset, u8 value) {
    if (memory.Read8(rule + offset) == value)
      return;
    memory.Write8(rule + offset, value);
    ++changed;
  };
  put(RULE_MODE, 1);
  put(RULE_STOCKS, set.tiebreak ? rules.tiebreak_stocks : stocks);
  put(RULE_STOCK_MINUTES, set.tiebreak ? rules.tiebreak_minutes : minutes);
  return changed;
}

namespace
{
constexpr u32 OS_MODULE_LIST = 0x800030C8;
constexpr u32 SORA_MELEE = 27;
constexpr u32 SORA_MELEE_BSS_SECTION = 6;
constexpr u32 G_FT_ENTRY_MANAGER = 0x2E88;
constexpr u32 ENTRY_SIZE = 0x244;
constexpr u32 ENTRY_ACTIVE = 0x0A;
constexpr u32 ENTRY_OWNER = 0x28;
constexpr u32 ENTRY_INSTANCES = 0x30;
constexpr u32 ENTRY_PLAYER_NO = 0x58;
constexpr u32 FIGHTER_MODULES = 0x60;
constexpr u32 MODULES_LIST = 0xD8;
constexpr u32 LIST_STATUS = 0x70;
constexpr u32 OWNER_DATA_STOCKS = 0x34;
constexpr u32 OWNER_DATA_DAMAGE = 0x24;
constexpr u32 STATUS_KIND = 0x34;
constexpr u32 GLOBAL_RESULT_INFO = 0x18;
constexpr u32 RESULT_DECISION = 0x1378;
constexpr u32 STATUS_CLIFF_FIRST = 0x73;
constexpr u32 STATUS_CLIFF_LAST = 0x75;

u32 Ptr(const GuestMemory& m, u32 at)
{
  if (at % 4 || !m.Valid(at) || !m.Valid(at + 3))
    return 0;
  const u32 p = m.Read32(at);
  return p % 4 == 0 && p >= 0x80000000 && m.Valid(p) ? p : 0;
}

u32 SoraMeleeBss(const GuestMemory& m)
{
  u32 module = Ptr(m, OS_MODULE_LIST);
  for (int i = 0; module && i < 64; ++i)
  {
    if (m.Read32(module) == SORA_MELEE && m.Valid(module + 0x10))
    {
      if (m.Read32(module + 0x0C) <= SORA_MELEE_BSS_SECTION)
        return 0;
      const u32 sections = Ptr(m, module + 0x10);
      const u32 at = sections + 8 * SORA_MELEE_BSS_SECTION;
      if (!sections || !m.Valid(at))
        return 0;
      const u32 bss = m.Read32(at) & ~1u;
      return bss && m.Valid(bss) ? bss : 0;
    }
    module = Ptr(m, module + 4);
  }
  return 0;
}
}

Live ReadLiveFight(const GuestMemory& m)
{
  Live live;
  const u32 global = Ptr(m, GAME_GLOBAL);
  const u32 info = global ? Ptr(m, global + GLOBAL_RESULT_INFO) : 0;
  if (!info || !m.Valid(info + RESULT_DECISION))
    return live;
  live.time_up = m.Read8(info + RESULT_DECISION) == DECISION_TIME_UP;
  const u32 bss = SoraMeleeBss(m);
  const u32 manager = bss ? Ptr(m, bss + G_FT_ENTRY_MANAGER) : 0;
  const u32 entries = manager ? Ptr(m, manager) : 0;
  if (!entries || !m.Valid(manager + 4))
    return live;
  const u32 count = std::min<u32>(m.Read32(manager + 4), 8);
  std::array<bool, 2> seen{false, false};
  for (u32 i = 0; i < count; ++i)
  {
    const u32 e = entries + ENTRY_SIZE * i;
    if (!m.Valid(e + ENTRY_SIZE - 1))
      break;
    const u32 port = m.Read32(e + ENTRY_PLAYER_NO);
    if (port > 1 || seen[port])
      continue;
    const u32 owner = Ptr(m, e + ENTRY_OWNER);
    const u32 data = owner ? Ptr(m, owner) : 0;
    if (!data || !m.Valid(data + OWNER_DATA_STOCKS) || !m.Valid(data + OWNER_DATA_DAMAGE + 3))
      return {};
    live.stocks[port] = static_cast<u8>(std::min<u32>(m.Read32(data + OWNER_DATA_STOCKS), 255));
    const u32 bits = m.Read32(data + OWNER_DATA_DAMAGE);
    float damage;
    std::memcpy(&damage, &bits, sizeof(damage));
    live.percent[port] =
        damage > 0 && damage < 10000 ? static_cast<u16>(static_cast<int>(damage)) : u16(0);
    const u32 active = std::min<u32>(m.Read8(e + ENTRY_ACTIVE), 3);
    const u32 fighter = Ptr(m, e + ENTRY_INSTANCES + 8 * active + 4);
    const u32 modules = fighter ? Ptr(m, fighter + FIGHTER_MODULES) : 0;
    const u32 list = modules ? Ptr(m, modules + MODULES_LIST) : 0;
    const u32 status = list ? Ptr(m, list + LIST_STATUS) : 0;
    if (status && m.Valid(status + STATUS_KIND + 3))
    {
      const u32 kind = m.Read32(status + STATUS_KIND);
      live.on_ledge[port] = kind >= STATUS_CLIFF_FIRST && kind <= STATUS_CLIFF_LAST;
      live.status[port] = kind;
    }
    seen[port] = true;
  }
  live.valid = seen[0] && seen[1];
  return live;
}

namespace
{
constexpr u32 SCENE_MANAGER = 0x805A0060;
constexpr u32 SCENE_CURRENT = 0x04;
constexpr u32 MELEE_RULE = 0x54;
constexpr u32 RULE_FRAMES_RUN = 0xE4;
constexpr int PAUSED_AFTER = 2;
constexpr u32 MELEE_HUD = 0x28;
constexpr u32 HUD_PORTS = 0xEC;
constexpr u32 HUD_PORT_FADE = 0x558;
}

std::array<float, 4> ReadHudFade(const GuestMemory& m)
{
  std::array<float, 4> fade{};
  const u32 manager = Ptr(m, SCENE_MANAGER);
  const u32 scene = manager ? Ptr(m, manager + SCENE_CURRENT) : 0;
  const u32 hud = scene ? Ptr(m, scene + MELEE_HUD) : 0;
  if (!hud || !m.Valid(hud + HUD_PORTS + 15))
    return fade;
  for (u32 port = 0; port < 4; ++port)
  {
    const u32 display = Ptr(m, hud + HUD_PORTS + 4 * port);
    if (!display || !m.Valid(display + HUD_PORT_FADE + 3))
      continue;
    const float w = std::bit_cast<float>(m.Read32(display + HUD_PORT_FADE));
    fade[port] = w > 0.0f ? std::min(w, 1.0f) : 0.0f;
  }
  return fade;
}

std::optional<u32> ReadFightClock(const GuestMemory& m)
{
  const u32 manager = Ptr(m, SCENE_MANAGER);
  const u32 scene = manager ? Ptr(m, manager + SCENE_CURRENT) : 0;
  const u32 rule = scene ? Ptr(m, scene + MELEE_RULE) : 0;
  if (!rule || !m.Valid(rule + RULE_FRAMES_RUN + 3))
    return std::nullopt;
  return m.Read32(rule + RULE_FRAMES_RUN);
}

u8 ReadHudPorts(const GuestMemory& m)
{
  if (ReadResults(m).scene != Scene::Fight)
    return 0;
  const u32 bss = SoraMeleeBss(m);
  const u32 manager = bss ? Ptr(m, bss + G_FT_ENTRY_MANAGER) : 0;
  const u32 entries = manager ? Ptr(m, manager) : 0;
  if (!entries || !m.Valid(manager + 4))
    return 0;
  const u32 count = std::min<u32>(m.Read32(manager + 4), 8);
  u8 ports = 0;
  for (u32 i = 0; i < count; ++i)
  {
    const u32 e = entries + ENTRY_SIZE * i;
    if (!m.Valid(e + ENTRY_SIZE - 1))
      break;
    const u32 port = m.Read32(e + ENTRY_PLAYER_NO);
    if (port < 4 && Ptr(m, e + ENTRY_OWNER))
      ports |= static_cast<u8>(1u << port);
  }
  return ports;
}

namespace
{
std::atomic<u8> s_hud_ports{0};
std::atomic<u32> s_hud_fade{0};
}

void HudFrame(const Core::CPUThreadGuard& guard, bool resimulating)
{
  if (resimulating)
    return;
  GuardMemory memory(guard);
  u8 ports = ReadHudPorts(memory);
  static std::optional<u32> s_clock;
  static int s_still = 0;
  const std::optional<u32> clock = ports ? ReadFightClock(memory) : std::nullopt;
  s_still = clock && *clock > 0 && s_clock && *clock == *s_clock ? s_still + 1 : 0;
  s_clock = clock;
  if (s_still >= PAUSED_AFTER)
    ports |= 0x10;
  if (!ports && ReadSceneName(memory) == "scSelctCharacter")
    ports = 0x20;
  s_hud_ports.store(ports, std::memory_order_relaxed);
  u32 packed = 0;
  if (ports & 0x0F)
  {
    const std::array<float, 4> fade = ReadHudFade(memory);
    for (int port = 0; port < 4; ++port)
      packed |= static_cast<u32>(fade[port] * 255.0f + 0.5f) << (8 * port);
  }
  s_hud_fade.store(packed, std::memory_order_relaxed);
}

u8 HudPorts()
{
  return s_hud_ports.load(std::memory_order_relaxed);
}

std::array<u8, 4> HudFade()
{
  const u32 packed = s_hud_fade.load(std::memory_order_relaxed);
  return {static_cast<u8>(packed), static_cast<u8>(packed >> 8), static_cast<u8>(packed >> 16),
          static_cast<u8>(packed >> 24)};
}

Facts ReadFacts(const GuestMemory& memory, bool live)
{
  Facts f;
  const Reading r = ReadResults(memory);
  f.scene = r.scene;
  f.block = r.block;
  f.decision = static_cast<u8>(r.block.decision);
  if (live && f.scene == Scene::Fight)
  {
    f.live = ReadLiveFight(memory);
    f.live_read = true;
  }
  return f;
}

void Frame(const Core::CPUThreadGuard& guard, int frame, bool resimulating)
{
  if (!ResultsVerified() || Rules::ProfileRuleset() == Rules::Ruleset::None)
    return;
  GuardMemory memory(guard);
  const std::optional<SetBlock::Header> header = SetBlock::ReadHeader(memory);
  if (!header || header->mode != SetBlock::Mode::Ranked)
  {
    s_published.store(0, std::memory_order_relaxed);
    s_dots.store(0, std::memory_order_relaxed);
    return;
  }
  const Facts facts = ReadFacts(memory, true);
  const SetState before = SetBlock::ReadSet(memory);
  const SetState after = Step(before, header->ruleset, facts, static_cast<u32>(frame));
  if (after != before)
  {
    SetBlock::WriteSet(memory, after);
    if ((after.fight_flags & SetBlock::FIGHT_SEEN) &&
        !(before.fight_flags & SetBlock::FIGHT_SEEN) && !resimulating)
    {
      NOTICE_LOG_FMT(ROLLBACK, "Ranked set: game {}'s fighters are in at frame {} (fight from {})",
                     after.GameNumber(), frame, after.fight_start);
    }
    if (after.games != before.games && !resimulating)
    {
      const GameRecord& r = after.records[std::min(after.games, u8(SetBlock::MAX_RECORDS)) - 1];
      NOTICE_LOG_FMT(ROLLBACK,
                     "Ranked set: game {} at frame {} (fight from {}): winner {}, how {}, "
                     "ledge {}/{}, set {}-{}{}",
                     after.games, frame, r.start, r.winner == NO_PORT ? 0 : r.winner + 1,
                     static_cast<int>(r.how), r.ledge[0], r.ledge[1], after.wins[0], after.wins[1],
                     after.done == 1 ? " (over)" :
                     after.done == 2 ? " (void)" :
                     after.tiebreak  ? " (tiebreak next)" :
                                       "");
    }
  }
  const Ranked::Ruleset* rules = RulesOf(header->ruleset);
  if (rules && rules->SetWritesGameRules() && facts.scene != Scene::Fight)
  {
    static const std::optional<std::pair<u8, u8>> s_rules = []() -> std::optional<std::pair<u8, u8>> {
      const char* v = std::getenv("ORCA_TEST_SET_RULES");
      char* end = nullptr;
      const long stocks = v ? std::strtol(v, &end, 10) : 0;
      const long minutes = end && *end == ',' ? std::strtol(end + 1, &end, 10) : 0;
      if (stocks >= 1 && stocks <= 99 && minutes >= 1 && minutes <= 99 && end && *end == '\0' &&
          TestKnobsAllowed())
      {
        return std::pair<u8, u8>(static_cast<u8>(stocks), static_cast<u8>(minutes));
      }
      return std::nullopt;
    }();
    WriteSetRules(memory, after, *rules, s_rules);
  }
  if (!resimulating && after.done == 1 && facts.scene == Scene::Results && after.games > 0 &&
      after.winner != NO_PORT)
  {
    const int me = s_local_port.load(std::memory_order_relaxed) == 1 ? 1 : 0;
    const GameRecord& last = after.records[std::min(after.games, u8(SetBlock::MAX_RECORDS)) - 1];
    SetEnd::Current().BlockSetOver(last.start, after.winner == me, after.wins[me],
                                   after.wins[1 - me], QueueApp::OwnRating(), SetEnd::NowMs());
  }
  const bool fighting = facts.scene == Scene::Fight || facts.scene == Scene::Between;
  const bool results = facts.scene == Scene::Results;
  const u32 packed = 0x80000000u | (fighting ? 1u << 30 : 0) | (results ? 1u << 29 : 0) |
                     (u32(after.done & 3) << 24) |
                     (after.tiebreak ? 1u << 23 : 0) | (u32(after.wins[0]) << 16) |
                     (u32(after.wins[1]) << 8) | u32(std::min(after.GameNumber(), 255));
  s_published.store(packed, std::memory_order_relaxed);
  u32 dots = 0;
  if (!fighting)
  {
    const Dots d = DotsOf(after);
    dots = 0x80000000u | (u32(d.current < 0 ? 3 : d.current) << 8);
    for (int i = 0; i < 3; ++i)
      dots |= u32(d.games[static_cast<std::size_t>(i)] + 1) << (2 * i);
  }
  s_dots.store(dots, std::memory_order_relaxed);
}

Dots DotsOf(const SetState& set)
{
  Dots d;
  d.on = true;
  std::size_t decided = 0;
  const int records = std::min<int>(set.games, SetBlock::MAX_RECORDS);
  for (int i = 0; i < records && decided < d.games.size(); ++i)
  {
    const u8 winner = set.records[static_cast<std::size_t>(i)].winner;
    if (winner == 0 || winner == 1)
      d.games[decided++] = winner;
  }
  d.current = set.done || decided >= d.games.size() ? -1 : static_cast<int>(decided);
  return d;
}

Dots CurrentDots()
{
  const u32 packed = s_dots.load(std::memory_order_relaxed);
  Dots d;
  if (!(packed & 0x80000000u))
    return d;
  d.on = true;
  for (int i = 0; i < 3; ++i)
    d.games[static_cast<std::size_t>(i)] = static_cast<int>((packed >> (2 * i)) & 3) - 1;
  const int current = static_cast<int>((packed >> 8) & 3);
  d.current = current == 3 ? -1 : current;
  return d;
}

std::string ScoreLine(const SetState& set, int local_port, bool results)
{
  const int me = local_port == 1 ? 1 : 0;
  const int mine = set.wins[me], theirs = set.wins[1 - me];
  if (set.done == 2)
    return fmt::format("Set void · {}–{}", mine, theirs);
  if (set.done)
    return fmt::format("Set over · {}–{}", mine, theirs);
  if (set.tiebreak)
    return fmt::format("Tiebreak game · {}–{}", mine, theirs);
  const int played = set.wins[0] + set.wins[1];
  if (results && played > 0)
    return fmt::format("Game {} · {}–{}", played, mine, theirs);
  return "";
}

std::string ResultToast(std::string_view out, int mine, int theirs)
{
  if (out == "won")
    return fmt::format("You won · {}–{}", mine, theirs);
  if (out == "lost")
    return fmt::format("You lost · {}–{}", mine, theirs);
  if (out == "draw")
    return "Draw · played again";
  return "That game didn't count";
}

std::string CurrentScoreLine()
{
  const u32 packed = s_published.load(std::memory_order_relaxed);
  if (!(packed & 0x80000000u) || (packed & (1u << 30)))
    return "";
  SetState s;
  s.done = static_cast<u8>((packed >> 24) & 3);
  s.tiebreak = packed & (1u << 23);
  s.wins = {static_cast<u8>((packed >> 16) & 0x7F), static_cast<u8>((packed >> 8) & 0xFF)};
  return ScoreLine(s, s_local_port.load(std::memory_order_relaxed), packed & (1u << 29));
}

void SetLocalPort(int port)
{
  s_local_port.store(port, std::memory_order_relaxed);
}
}
