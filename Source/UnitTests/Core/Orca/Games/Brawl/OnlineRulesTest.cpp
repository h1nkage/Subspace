// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "Core/Orca/Games/Brawl/MatchBlock.h"
#include "Core/Orca/Engine/GuestMemory.h"
#include "Core/Orca/Games/Brawl/BrawlGame.h"
#include "Core/Orca/Games/Brawl/OnlineRules.h"
#include "Core/Orca/Games/Brawl/Rulesets.h"
#include "Core/Orca/Games/Brawl/SetBlock.h"
#include "Core/Orca/Games/Brawl/Queue.h"
#include "Core/Orca/Session/Replay.h"
#include "Core/Rollback/OnlineMatch.h"
#include "InputCommon/GCPadStatus.h"

using namespace Orca::UX;
using namespace Orca::UX::Rules;
namespace MB = Orca::UX::MatchBlock;

namespace
{
const int CSS_READY_FRAMES = Rulesets::Brawl2025().css_ready_seconds * 60;
int NextFrame()
{
  static int s_frame = 0;
  return ++s_frame;
}

class FakeMemory final : public GuestMemory
{
public:
  bool Valid(u32 a) const override { return bytes.contains(a); }
  u8 Read8(u32 a) const override { return bytes.at(a); }
  u16 Read16(u32 a) const override { return static_cast<u16>(Read8(a) << 8 | Read8(a + 1)); }
  u32 Read32(u32 a) const override { return u32(Read16(a)) << 16 | Read16(a + 2); }
  void Write8(u32 a, u8 v) override
  {
    bytes[a] = v;
    if (track_writes)
      recorded.push_back(a);
    ++writes;
  }
  void Write16(u32 a, u16 v) override
  {
    Write8(a, static_cast<u8>(v >> 8));
    Write8(a + 1, static_cast<u8>(v));
  }
  void Write32(u32 a, u32 v) override
  {
    Write16(a, static_cast<u16>(v >> 16));
    Write16(a + 2, static_cast<u16>(v));
  }
  void Fill(u32 a, u32 n, u8 v = 0)
  {
    for (u32 i = 0; i < n; ++i)
      bytes[a + i] = v;
  }
  void Text(u32 a, std::string_view s)
  {
    for (size_t i = 0; i < s.size(); ++i)
      bytes[a + static_cast<u32>(i)] = static_cast<u8>(s[i]);
    bytes[a + static_cast<u32>(s.size())] = 0;
  }
  bool track_writes = false;
  std::vector<u32> recorded;
  std::map<u32, u8> bytes;
  int writes = 0;
};

constexpr u32 SET_RULE = 0x9017F360;
constexpr u32 MELEE = 0x90180F20;
constexpr u32 MENU_DATA = 0x9017BE50;
constexpr u32 SCENE = 0x80910000;
constexpr u32 SCENE_NAME = 0x80920000;
constexpr u32 PPLUS_RSS = 0x8042C4E8;
constexpr u32 PPLUS_STRIKES = 0x8042C822;

void SetScene(FakeMemory& m, std::string_view scene)
{
  m.Fill(SCENE_NAME, 0x20);
  m.Text(SCENE_NAME, scene);
}

void SetPick(FakeMemory& m, int port, u8 type, u8 character)
{
  const u32 p = MELEE + 0x98 + static_cast<u32>(port) * 0x5C;
  m.Write8(p + 0, character);
  m.Write8(p + 1, type);
}
constexpr u8 HUMAN = 0, CPU = 1, NOBODY = 3, NONE = 0x3E;

constexpr u32 CSS_TASK = 0x80930000;
constexpr u32 CSS_AREA = 0x80940000;
constexpr u32 CSS_HAND = 0x80950000;

void SetHand(FakeMemory& m, int port, u32 target, float y, u32 button = 0, float x = 0)
{
  const u32 hand = CSS_HAND + static_cast<u32>(port) * 0x100;
  m.Write32(hand + 0x80, target);
  m.Write32(hand + 0x90, std::bit_cast<u32>(x));
  m.Write32(hand + 0x94, std::bit_cast<u32>(y));
  m.Write32(hand + 0xAC, button);
}

void BuildHands(FakeMemory& m)
{
  m.Write32(SCENE + 0x400, CSS_TASK);
  m.Fill(CSS_TASK, 0x60);
  for (u32 port = 0; port < 4; ++port)
  {
    const u32 area = CSS_AREA + port * 0x1000;
    const u32 hand = CSS_HAND + port * 0x100;
    m.Write32(CSS_TASK + 0x44 + 4 * port, area);
    m.Fill(area, 0x1B0);
    m.Write32(area + 0x1A8, hand);
    m.Fill(hand, 0xE0);
    SetHand(m, static_cast<int>(port), CSS_HAND_GRID, 5.0f);
  }
}

FakeMemory Game(std::string_view scene)
{
  FakeMemory m;
  m.Fill(0x805A0060, 4);
  m.Fill(0x80900000, 0x10);
  m.Fill(SCENE, 0x410);
  m.Write32(0x805A0060, 0x80900000);
  m.Write32(0x80900004, SCENE);
  m.Write32(SCENE, SCENE_NAME);
  SetScene(m, scene);
  m.Fill(0x805A00E0, 4);
  m.Fill(0x90181300, 0x40);
  m.Write32(0x805A00E0, 0x90181300);
  m.Write32(0x90181308, MELEE);
  m.Write32(0x9018131C, SET_RULE);
  m.Fill(MELEE, 0x98 + 4 * 0x5C);
  for (int port = 0; port < 4; ++port)
    SetPick(m, port, NOBODY, NONE);
  m.Fill(SET_RULE, 0x88);
  m.Write8(SET_RULE + 0x02, 0x00);
  m.Write8(SET_RULE + 0x03, 2);
  m.Write8(SET_RULE + 0x04, 3);
  m.Write8(SET_RULE + 0x06, 10);
  m.Write8(SET_RULE + 0x0A, 1);
  m.Write8(SET_RULE + 0x0C, 1);
  m.Fill(MENU_DATA, 0x30);
  m.Write32(MENU_DATA + 0x00, 0x02000000);
  m.Write32(MENU_DATA + 0x04, 0x4F524341);
  m.Write32(MENU_DATA + 0x08, 0xFFFFFFFF);
  m.Write32(MENU_DATA + 0x0C, 0xFFFFFFFF);
  m.Write32(MENU_DATA + 0x20, 0x0003FF00);
  m.Write32(MENU_DATA + 0x24, 0x7FFFFFFF);
  for (u32 i = 0; i < MB::FULL_SIZE; i += 4)
    m.Write32(MB::BASE + i, 0x3A200000 + i);
  m.Fill(PPLUS_RSS, 0x320, 0x11);
  m.Fill(PPLUS_STRIKES, 0x1E, 0x01);
  BuildHands(m);
  m.writes = 0;
  return m;
}

FakeMemory Locked(Mode mode, Ruleset ruleset, std::string_view scene = "scSelctCharacter")
{
  FakeMemory m = Game(scene);
  WriteHeader(m, mode, ruleset);
  m.writes = 0;
  return m;
}
}

TEST(OrcaOnlineRules, DoublesTeamsRequireExactlyFourSeatsAndTwoPerTeam)
{
  EXPECT_EQ(RoomTeamsMask(true, {{0, 0}, {1, 0}, {2, 1}, {3, 1}}), u8{12});
  EXPECT_EQ(RoomTeamsMask(true, {{0, 0}, {1, 1}, {2, 0}, {3, 1}}), u8{10});
  for (const auto& teams : std::vector<std::map<int, int>>{
           {}, {{0, 0}, {1, 1}}, {{0, 0}, {1, 0}, {2, 0}, {3, 1}},
           {{0, 0}, {1, 0}, {2, 1}, {4, 1}}, {{0, 0}, {1, 2}, {2, 1}, {3, 1}}})
    EXPECT_EQ(RoomTeamsMask(true, teams), u8{0});
  EXPECT_EQ(RoomTeamsMask(false, {{0, 0}, {1, 1}, {2, 0}, {3, 1}}), u8{0});
}

TEST(OrcaOnlineRules, DoublesPinNativeColoursOnlyWhenTeamsAreOnAndReplayRestoresPairing)
{
  auto m = Game("scSelctCharacter");
  m.Fill(CSS_TASK + 0x5C8, 1);
  for (u32 port = 0; port < 4; ++port)
    m.Fill(CSS_AREA + port * 0x1000 + 0x1C0, 4);
  Orca::Net::ReplayFrame event;
  {
    Orca::Net::ReplayScope scope(&event, false);
    EXPECT_GT(WriteRoomTeams(m, 12), 0);
  }
  EXPECT_EQ(event.room_teams, u8{12});
  EXPECT_FALSE(ReadHeader(m).Locked());
  bool began = true;
  EXPECT_EQ(ApplyRoomTeams(m, &began), 0);
  EXPECT_FALSE(began);
  EXPECT_EQ(m.Read8(CSS_TASK + 0x5C8), u8{0});
  m.Write8(CSS_TASK + 0x5C8, 1);
  const auto before = m;
  EXPECT_GT(ApplyRoomTeams(m, &began), 0);
  EXPECT_TRUE(began);
  for (u32 port = 0; port < 4; ++port)
    EXPECT_EQ(m.Read32(CSS_AREA + port * 0x1000 + 0x1C0), static_cast<u32>(port >= 2));
  const auto pinned = m.bytes;
  EXPECT_EQ(ApplyRoomTeams(m, &began), 0);
  EXPECT_FALSE(began);
  m = before;
  ApplyRoomTeams(m);
  EXPECT_EQ(m.bytes, pinned);
  m.Write32(CSS_AREA + 0x1C0, 2);
  ApplyRoomTeams(m);
  EXPECT_EQ(m.Read32(CSS_AREA + 0x1C0), u32{0});
  WriteRoomTeams(m, 10);
  EXPECT_EQ(ReadRoomTeams(m), u8{10});
  {
    Orca::Net::ReplayScope scope(&event, true);
    WriteRoomTeams(m, *event.room_teams);
  }
  EXPECT_EQ(ReadRoomTeams(m), u8{12});
  WriteRoomTeams(m, 0);
  m.Write32(CSS_AREA + 0x1C0, 2);
  EXPECT_EQ(ApplyRoomTeams(m), 0);
  EXPECT_EQ(m.Read32(CSS_AREA + 0x1C0), u32{2});
}

TEST(OrcaOnlineRules, DoublesStageSelectPinsTheFightSourceAndPairingInvalidatesKeyframes)
{
  auto m = Game("scSelStage");
  constexpr u32 select = 0x90180B40;
  m.Write32(0x90181310, select);
  m.Fill(select, 0x240);
  WriteRoomTeams(m, 10);
  EXPECT_EQ(ApplyRoomTeams(m), 0);
  EXPECT_EQ(m.Read8(select + 0x33), u8{0});
  m.Write8(select + 0x33, 1);
  ApplyRoomTeams(m);
  for (u32 port = 0; port < 4; ++port)
    EXPECT_EQ(m.Read8(select + 0xB8 + port * 0x5C + 0x0B), static_cast<u8>(port % 2));
  SetWanted(Mode::None, 0, {}, 10);
  const u64 generation = WantedGeneration();
  EXPECT_FALSE(SetWanted(Mode::None, 0, {}, 10));
  EXPECT_TRUE(SetWanted(Mode::None, 0, {}, 12));
  EXPECT_GT(WantedGeneration(), generation);
  EXPECT_FALSE(HeaderInPlace());
  SetWanted(Mode::None);
}

TEST(OrcaOnlineRules, AJoinTakesNoSetGamesItDidntSee)
{
  const SetBlock::SetState fresh;
  EXPECT_TRUE(SetFitsJoin(fresh, 0));
  EXPECT_TRUE(SetFitsJoin(fresh, -1));
  SetBlock::SetState planted = fresh;
  planted.games = 1;
  planted.wins = {1, 0};
  planted.last_winner = 0;
  planted.records[0].start = 4210;
  planted.records[0].winner = 0;
  planted.records[0].how = SetBlock::How::Results;
  EXPECT_FALSE(SetFitsJoin(planted, 0));
  SetBlock::SetState decided = planted;
  decided.games = 2;
  decided.wins = {2, 0};
  decided.done = 1;
  decided.winner = 0;
  EXPECT_FALSE(SetFitsJoin(decided, 0));
  EXPECT_FALSE(SetFitsJoin(decided, 1));
  SetBlock::SetState record = fresh;
  record.records[3].winner = 1;
  EXPECT_FALSE(SetFitsJoin(record, 0));
  SetBlock::SetState streak = fresh;
  streak.tb_streak = 2;
  EXPECT_FALSE(SetFitsJoin(streak, 0));
  EXPECT_TRUE(SetFitsJoin(planted, 1));
  EXPECT_TRUE(SetFitsJoin(decided, 2));
  EXPECT_TRUE(SetFitsJoin(decided, -1));
}

TEST(OrcaOnlineRules, NoHeaderInTheGamesDeadCode)
{
  FakeMemory m = Game("scSelctCharacter");
  EXPECT_FALSE(ReadHeader(m).present);
  EXPECT_FALSE(ReadHeader(m).Locked());
  EXPECT_EQ(WriteHeader(m, Mode::None, Ruleset::Brawl), 0);
  EXPECT_EQ(m.writes, 0);
  EXPECT_EQ(m.Read32(MB::BASE), 0x3A200000u);
}

TEST(OrcaOnlineRules, TheHeaderIsWrittenOnceAndSavesThePlayersRules)
{
  FakeMemory m = Game("scSelctCharacter");
  EXPECT_GT(WriteHeader(m, Mode::Casual, Ruleset::Brawl), 0);
  const Header h = ReadHeader(m);
  EXPECT_TRUE(h.Locked());
  EXPECT_EQ(h.mode, Mode::Casual);
  EXPECT_EQ(h.ruleset, Ruleset::Brawl);
  EXPECT_EQ(m.Read32(MB::MAGIC), MB::MAGIC_VALUE);
  EXPECT_EQ(m.Read32(MB::SET + 0x40), 0u);
  EXPECT_EQ(m.Read8(MB::SET + 0x03), MB::NONE);
  EXPECT_EQ(m.Read8(MB::SAVED_RULES + 1), 2);
  EXPECT_EQ(m.Read32(MB::SAVED_ITEMS), 0x02000000u);
  EXPECT_EQ(m.Read32(MB::SAVED_STAGES + 4), 0x7FFFFFFFu);
  m.writes = 0;
  EXPECT_EQ(WriteHeader(m, Mode::Casual, Ruleset::Brawl), 0);
  EXPECT_EQ(m.writes, 0);
}

TEST(OrcaOnlineRules, ClearingGivesBackThePlayersRules)
{
  FakeMemory m = Game("scSelctCharacter");
  WriteHeader(m, Mode::Ranked, Ruleset::Brawl);
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read8(SET_RULE + 0x0A), 0);
  EXPECT_EQ(m.Read32(MENU_DATA + 0x08), 0u);
  EXPECT_GT(WriteHeader(m, Mode::None, Ruleset::Brawl), 0);
  EXPECT_FALSE(ReadHeader(m).present);
  EXPECT_EQ(m.Read8(SET_RULE + 0x02), 0x00);
  EXPECT_EQ(m.Read8(SET_RULE + 0x0A), 1);
  EXPECT_EQ(m.Read32(MENU_DATA + 0x00), 0x02000000u);
  EXPECT_EQ(m.Read32(MENU_DATA + 0x08), 0xFFFFFFFFu);
  EXPECT_EQ(m.Read32(MENU_DATA + 0x24), 0x7FFFFFFFu);
  EXPECT_EQ(m.Read32(MB::BASE), 0x3A200000u);
  for (u32 i = 4; i < MB::FULL_SIZE; ++i)
    ASSERT_EQ(m.Read8(MB::BASE + i), 0) << i;
  EXPECT_EQ(ApplyLocks(m, NextFrame()), 0);
  m.writes = 0;
  EXPECT_EQ(WriteHeader(m, Mode::None, Ruleset::Brawl), 0);
  EXPECT_EQ(m.writes, 0);
  EXPECT_GT(WriteHeader(m, Mode::Casual, Ruleset::Brawl), 0);
  EXPECT_TRUE(ReadHeader(m).Locked());
}

TEST(OrcaOnlineRules, NoHeaderBeforeTheGameLoadsTheModule)
{
  FakeMemory m = Game("scSelctCharacter");
  for (u32 i = 0; i < MB::FULL_SIZE; ++i)
    m.Write8(MB::BASE + i, 0);
  m.writes = 0;
  EXPECT_EQ(WriteHeader(m, Mode::Ranked, Ruleset::Brawl, 1, 7), 0);
  EXPECT_EQ(m.writes, 0);
}

TEST(OrcaOnlineRules, AnotherQueueRoomStartsANewSetAndKeepsTheSavedRules)
{
  FakeMemory m = Game("scSelctCharacter");
  WriteHeader(m, Mode::Casual, Ruleset::Brawl);
  ApplyLocks(m, NextFrame());
  m.Write8(MB::SET, 0x55);
  m.Write16(MB::CSS_TIMER, 77);
  EXPECT_GT(WriteHeader(m, Mode::Ranked, Ruleset::Brawl), 0);
  EXPECT_EQ(ReadHeader(m).mode, Mode::Ranked);
  EXPECT_EQ(m.Read8(MB::SET), 0);
  EXPECT_EQ(m.Read16(MB::CSS_TIMER), 0);
  EXPECT_EQ(m.Read32(MB::SAVED_ITEMS), 0x02000000u);
  EXPECT_EQ(m.Read8(MB::SAVED_RULES + 8), 1);
}

TEST(OrcaOnlineRules, BrawlsCasualRules)
{
  FakeMemory m = Locked(Mode::Casual, Ruleset::Brawl);
  EXPECT_GT(ApplyLocks(m, NextFrame()), 0);
  EXPECT_EQ(m.Read8(SET_RULE + 0x02) & 7, 1);
  EXPECT_EQ(m.Read8(SET_RULE + 0x04), 3);
  EXPECT_EQ(m.Read8(SET_RULE + 0x05), 0);
  EXPECT_EQ(m.Read8(SET_RULE + 0x06), 10);
  EXPECT_EQ(m.Read8(SET_RULE + 0x07), 0);
  EXPECT_EQ(m.Read8(SET_RULE + 0x08), 8);
  EXPECT_EQ(m.Read8(SET_RULE + 0x09), 1);
  EXPECT_EQ(m.Read8(SET_RULE + 0x0A), 0);
  EXPECT_EQ(m.Read8(SET_RULE + 0x03), 2);
  EXPECT_EQ(m.Read8(SET_RULE + 0x0C), 1);
  EXPECT_EQ(m.Read8(MENU_DATA), 0);
  EXPECT_EQ(m.Read32(MENU_DATA + 0x04), 0x4F524341u);
  EXPECT_EQ(m.Read32(MENU_DATA + 0x08), 0u);
  EXPECT_EQ(m.Read32(MENU_DATA + 0x0C), 0u);
  EXPECT_EQ(m.Read32(MENU_DATA + 0x24), BRAWL_LEGAL_BRAWL_PAGE);
  EXPECT_EQ(m.Read32(MENU_DATA + 0x20), BRAWL_LEGAL_MELEE_PAGE);
  m.writes = 0;
  EXPECT_EQ(ApplyLocks(m, NextFrame()), 0);
  EXPECT_EQ(m.writes, 0);
  m.Write8(SET_RULE + 0x04, 5);
  m.Write8(MENU_DATA, 3);
  EXPECT_EQ(ApplyLocks(m, NextFrame()), 2);
  EXPECT_EQ(m.Read8(SET_RULE + 0x04), 3);
  EXPECT_EQ(m.Read8(MENU_DATA), 0);
}

TEST(OrcaOnlineRules, RankedChoosesTheStageAndProjectPlusPlaysFourStocks)
{
  FakeMemory ranked = Locked(Mode::Ranked, Ruleset::Brawl);
  ApplyLocks(ranked, NextFrame());
  EXPECT_EQ(ranked.Read8(SET_RULE + 0x07), 0);
  EXPECT_EQ(ranked.Read8(SET_RULE + 0x04), 3);
  FakeMemory pplus = Locked(Mode::Casual, Ruleset::PPlus);
  ApplyLocks(pplus, NextFrame());
  EXPECT_EQ(pplus.Read8(SET_RULE + 0x04), 4);
  EXPECT_EQ(pplus.Read8(SET_RULE + 0x07), 0);
  EXPECT_EQ(pplus.Read32(MENU_DATA + 0x24), 0x7FFFFFFFu);
}

TEST(OrcaOnlineRules, IceClimbersTakeFinalDestinationOutOfTheRandomStages)
{
  FakeMemory m = Locked(Mode::Casual, Ruleset::Brawl);
  SetPick(m, 0, HUMAN, 0x00);
  SetPick(m, 1, HUMAN, 0x02);
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read32(MENU_DATA + 0x24) & BRAWL_FINAL_DESTINATION_BIT, BRAWL_FINAL_DESTINATION_BIT);
  SetPick(m, 1, HUMAN, CHARACTER_ICE_CLIMBERS);
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read32(MENU_DATA + 0x24), BRAWL_LEGAL_BRAWL_PAGE & ~BRAWL_FINAL_DESTINATION_BIT);
  SetPick(m, 1, CPU, CHARACTER_ICE_CLIMBERS);
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read32(MENU_DATA + 0x24), BRAWL_LEGAL_BRAWL_PAGE);
  SetScene(m, "scSelStage");
  SetPick(m, 1, HUMAN, CHARACTER_ICE_CLIMBERS);
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read32(MENU_DATA + 0x24), BRAWL_LEGAL_BRAWL_PAGE & ~BRAWL_FINAL_DESTINATION_BIT);
  SetScene(m, "scMelee");
  SetPick(m, 1, HUMAN, 0x02);
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read32(MENU_DATA + 0x24), BRAWL_LEGAL_BRAWL_PAGE & ~BRAWL_FINAL_DESTINATION_BIT);
}

TEST(OrcaOnlineRules, ProjectPlusTakesThe2024ProposedListOnTheCharacterSelect)
{
  FakeMemory m = Locked(Mode::Casual, Ruleset::PPlus);
  EXPECT_EQ(m.Read8(MB::SAVED_RSS), 0x11);
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read32(PPLUS_RSS), 0x00000003u);
  EXPECT_EQ(m.Read8(PPLUS_RSS + 0x3C), 9);
  EXPECT_EQ(m.Read8(PPLUS_RSS + 0xA8), 0x11);
  EXPECT_EQ(m.Read8(PPLUS_STRIKES), 0);
  FakeMemory ranked = Locked(Mode::Ranked, Ruleset::PPlus);
  ApplyLocks(ranked, NextFrame());
  EXPECT_EQ(ranked.Read8(PPLUS_STRIKES), 0x01);
  WriteHeader(m, Mode::None, Ruleset::PPlus);
  EXPECT_EQ(m.Read8(PPLUS_RSS), 0x11);
  EXPECT_EQ(m.Read8(PPLUS_RSS + 0x3C), 0x11);
}

TEST(OrcaOnlineRules, TheGateLocksTheMenusOnlyUnderAHeader)
{
  FakeMemory m = Game("scSelctCharacter");
  for (const auto& mask : GateMasks(m))
    EXPECT_TRUE(mask.Empty());
  WriteHeader(m, Mode::Casual, Ruleset::Brawl);
  for (const auto& mask : GateMasks(m))
    EXPECT_EQ(mask.buttons, PAD_BUTTON_B);
  SetScene(m, "scSelStage");
  for (const auto& mask : GateMasks(m))
    EXPECT_EQ(mask.buttons, PAD_BUTTON_B);
  SetScene(m, "scMelee");
  for (const auto& mask : GateMasks(m))
    EXPECT_EQ(mask.buttons, PAD_BUTTON_START);
  SetScene(m, "scVsResult");
  for (const auto& mask : GateMasks(m))
    EXPECT_TRUE(mask.Empty());
  FakeMemory p = Locked(Mode::Ranked, Ruleset::PPlus);
  for (const auto& mask : GateMasks(p))
  {
    EXPECT_EQ(mask.buttons, PAD_BUTTON_B | PAD_BUTTON_UP | PAD_BUTTON_DOWN | PAD_BUTTON_LEFT |
                                PAD_BUTTON_RIGHT);
    EXPECT_FALSE(mask.main_stick);
  }
  SetScene(p, "scSelStage");
  for (const auto& mask : GateMasks(p))
    EXPECT_EQ(mask.buttons, PAD_BUTTON_B | PAD_TRIGGER_L | PAD_TRIGGER_R | PAD_TRIGGER_Z);
}

TEST(OrcaOnlineRules, RankedResultsNeverTakeB)
{
  for (Ruleset ruleset : {Ruleset::Brawl, Ruleset::PPlus})
  {
    FakeMemory ranked = Locked(Mode::Ranked, ruleset, "scVsResult");
    for (const auto& mask : GateMasks(ranked))
    {
      EXPECT_EQ(mask.buttons, PAD_BUTTON_B);
      EXPECT_EQ(mask.press, 0);
    }
    FakeMemory casual = Locked(Mode::Casual, ruleset, "scVsResult");
    for (const auto& mask : GateMasks(casual))
      EXPECT_EQ(mask.buttons & PAD_BUTTON_B, 0);
    SetScene(ranked, "scMelee");
    for (const auto& mask : GateMasks(ranked))
      EXPECT_EQ(mask.buttons, PAD_BUTTON_START);
  }
}

TEST(OrcaOnlineRules, TheResultsScreenNeverTakesZ)
{
  FakeMemory m = Game("scVsResult");
  for (const auto& mask : SessionMasks(m))
  {
    EXPECT_EQ(mask.buttons, PAD_TRIGGER_Z);
    EXPECT_EQ(mask.press, 0);
    EXPECT_FALSE(mask.main_stick);
    EXPECT_FALSE(mask.c_stick);
  }
  for (const auto& mask : GateMasks(m))
    EXPECT_TRUE(mask.Empty());
  for (const char* scene : {"scSelctCharacter", "scSelStage", "scMelee", "scMemoryChange",
                            "muMenuMain", ""})
  {
    SetScene(m, scene);
    for (const auto& mask : SessionMasks(m))
      EXPECT_TRUE(mask.Empty()) << scene;
  }
  for (Mode mode : {Mode::Casual, Mode::Ranked})
  {
    for (Ruleset ruleset : {Ruleset::Brawl, Ruleset::PPlus})
    {
      FakeMemory locked = Locked(mode, ruleset, "scVsResult");
      for (const auto& mask : SessionMasks(locked))
        EXPECT_EQ(mask.buttons, PAD_TRIGGER_Z);
      SetScene(locked, "scMelee");
      for (const auto& mask : SessionMasks(locked))
        EXPECT_TRUE(mask.Empty());
    }
  }
  EXPECT_EQ(m.writes, 0);
  FakeMemory empty;
  for (const auto& mask : SessionMasks(empty))
    EXPECT_TRUE(mask.Empty());
}

TEST(OrcaOnlineRules, TheCharacterSelectPassesAOnlyInTheGrid)
{
  constexpr u16 B = PAD_BUTTON_B, A = PAD_BUTTON_A;
  FakeMemory m = Game("scSelctCharacter");
  for (int port = 0; port < 4; ++port)
    SetHand(m, port, CSS_HAND_BUTTON, -19.8f, CSS_BUTTON_PLAYER_TYPE, -22.5f + 15.0f * port);
  for (const auto& mask : GateMasks(m))
    EXPECT_TRUE(mask.Empty());
  WriteHeader(m, Mode::Ranked, Ruleset::Brawl);
  for (const auto& mask : GateMasks(m))
    EXPECT_EQ(mask.buttons, B | A);
  const struct
  {
    u32 target, button;
  } blocked[] = {
      {CSS_HAND_NOTHING, 0}, {CSS_HAND_BUTTON, 0x1C},
      {CSS_HAND_BUTTON, 0x1B},
      {CSS_HAND_BUTTON, 0x03},
      {CSS_HAND_BUTTON, 0x05}, {CSS_HAND_BUTTON, 0x06}, {CSS_HAND_BUTTON, 0x07},
      {CSS_HAND_EXIT, 0x02}, {CSS_HAND_EXIT, 0x04},
      {5, 0x08},
  };
  for (const auto& b : blocked)
  {
    SetHand(m, 0, b.target, 5.0f, b.button);
    EXPECT_EQ(GateMasks(m)[0].buttons, B | A) << b.target << " " << b.button;
  }
  for (const u32 target : {CSS_HAND_GRID, CSS_HAND_OWN_TOKEN, CSS_HAND_TAKING,
                           CSS_HAND_GRID_HOLDING, CSS_HAND_PLACING})
  {
    SetHand(m, 0, target, 5.0f, target == CSS_HAND_OWN_TOKEN ? 1 : 0);
    EXPECT_EQ(GateMasks(m)[0].buttons, B) << target;
    EXPECT_EQ(GateMasks(m)[1].buttons, B | A);
  }
  SetHand(m, 0, CSS_HAND_GRID_HOLDING, 14.9f);
  EXPECT_EQ(GateMasks(m)[0].buttons, B);
  SetHand(m, 0, CSS_HAND_GRID_HOLDING, CSS_A_TOP);
  EXPECT_EQ(GateMasks(m)[0].buttons, B | A);
  SetHand(m, 0, CSS_HAND_GRID, 15.9f);
  EXPECT_EQ(GateMasks(m)[0].buttons, B | A);
  SetHand(m, 0, CSS_HAND_GRID, -2.9f);
  EXPECT_EQ(GateMasks(m)[0].buttons, B);
  SetHand(m, 0, CSS_HAND_GRID_HOLDING, -3.4f);
  EXPECT_EQ(GateMasks(m)[0].buttons, B);
  SetHand(m, 0, CSS_HAND_GRID_HOLDING, -4.07f);
  EXPECT_EQ(GateMasks(m)[0].buttons, B);
  SetHand(m, 0, CSS_HAND_GRID, -4.3f);
  EXPECT_EQ(GateMasks(m)[0].buttons, B | A);
  SetHand(m, 0, CSS_HAND_GRID, CSS_A_BOTTOM_BRAWL);
  EXPECT_EQ(GateMasks(m)[0].buttons, B | A);
  SetHand(m, 0, CSS_HAND_GRID, std::nanf(""));
  EXPECT_EQ(GateMasks(m)[0].buttons, B | A);
  SetHand(m, 0, CSS_HAND_GRID, 5.0f);
  m.Write32(CSS_TASK + 0x44, 0);
  EXPECT_FALSE(ReadCssHand(m, 0).valid);
  EXPECT_EQ(GateMasks(m)[0].buttons, B | A);
  SetScene(m, "scSelStage");
  for (const auto& mask : GateMasks(m))
    EXPECT_EQ(mask.buttons, B);
  FakeMemory p = Locked(Mode::Casual, Ruleset::PPlus);
  SetHand(p, 1, CSS_HAND_BUTTON, -19.8f, CSS_BUTTON_PLAYER_TYPE);
  constexpr u16 DPAD = PAD_BUTTON_UP | PAD_BUTTON_DOWN | PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT;
  EXPECT_EQ(GateMasks(p)[0].buttons, B | DPAD);
  EXPECT_EQ(GateMasks(p)[1].buttons, B | DPAD | A);
  SetHand(p, 0, CSS_HAND_GRID_HOLDING, 0.5f);
  EXPECT_EQ(GateMasks(p)[0].buttons, B | DPAD);
  SetHand(p, 0, CSS_HAND_GRID, -2.9f);
  EXPECT_EQ(GateMasks(p)[0].buttons, B | DPAD);
  SetHand(p, 0, CSS_HAND_GRID, -3.4f);
  EXPECT_EQ(GateMasks(p)[0].buttons, B | DPAD | A);
}

TEST(OrcaOnlineRules, AOnBackBacksOutOfTheQueuesOwnCharacterSelectOnly)
{
  constexpr u16 A = PAD_BUTTON_A;
  FakeMemory m = Game("scSelctCharacter");
  m.Write16(Queue::RAW, 0);
  m.Write16(Queue::RAW + 2, 0);
  for (int port = 0; port < 4; ++port)
    SetHand(m, port, CSS_HAND_EXIT, 19.5f, CSS_BUTTON_BACK, -28.0f);
  for (const auto& mask : GateMasks(m))
    EXPECT_TRUE(mask.Empty());
  WriteHeader(m, Mode::Casual, Ruleset::Brawl, 0, 0, MB::FLAG_SOLO | MB::FLAG_QUEUE2);
  Rollback::InputGate::Masks masks = GateMasks(m);
  EXPECT_FALSE(masks[0].buttons & A);
  EXPECT_TRUE(masks[0].a_centres_stick);
  for (int port = 1; port < 4; ++port)
  {
    EXPECT_TRUE(masks[port].buttons & A) << port;
    EXPECT_FALSE(masks[port].a_centres_stick) << port;
  }
  m.Write16(Queue::RAW, A);
  EXPECT_TRUE(GateMasks(m)[0].buttons & A);
  EXPECT_FALSE(GateMasks(m)[0].a_centres_stick);
  m.Write16(Queue::RAW, PAD_BUTTON_B);
  EXPECT_FALSE(GateMasks(m)[0].buttons & A);
  m.Write16(Queue::RAW, 0);
  WriteHeader(m, Mode::Ranked, Ruleset::Brawl, 0, 0, MB::FLAG_SOLO | MB::FLAG_QUEUE2);
  EXPECT_FALSE(GateMasks(m)[0].buttons & A);
  FakeMemory p = Game("scSelctCharacter");
  p.Write16(Queue::RAW, 0);
  p.Write16(Queue::RAW + 2, 0);
  SetHand(p, 0, CSS_HAND_EXIT, 19.5f, CSS_BUTTON_BACK, -28.0f);
  WriteHeader(p, Mode::Casual, Ruleset::PPlus, 0, 0, MB::FLAG_SOLO | MB::FLAG_QUEUE2);
  EXPECT_FALSE(GateMasks(p)[0].buttons & A);
  EXPECT_TRUE(GateMasks(p)[0].a_centres_stick);
  WriteHeader(m, Mode::Casual, Ruleset::Brawl, 0, 0, MB::FLAG_SOLO | MB::FLAG_QUEUE2);
  SetHand(m, 0, CSS_HAND_EXIT, 19.5f, 0x04, 20.0f);
  EXPECT_TRUE(GateMasks(m)[0].buttons & A);
  SetHand(m, 0, CSS_HAND_BUTTON, 19.5f, 0x03, -14.0f);
  EXPECT_TRUE(GateMasks(m)[0].buttons & A);
  SetHand(m, 0, CSS_HAND_NOTHING, 19.5f, 0, -22.0f);
  EXPECT_TRUE(GateMasks(m)[0].buttons & A);
  SetHand(m, 0, CSS_HAND_EXIT, 19.5f, CSS_BUTTON_BACK, -28.0f);
  for (const Mode mode : {Mode::Casual, Mode::Ranked})
  {
    for (const u8 flags : {u8{0}, u8{MB::FLAG_QUEUE2}})
    {
      WriteHeader(m, mode, Ruleset::Brawl, 0, 0, flags);
      EXPECT_TRUE(GateMasks(m)[0].buttons & A) << static_cast<int>(mode) << " " << int{flags};
      EXPECT_FALSE(GateMasks(m)[0].a_centres_stick);
    }
  }
  CssHand back;
  back.valid = true;
  back.target = CSS_HAND_EXIT;
  back.button = CSS_BUTTON_BACK;
  EXPECT_TRUE(CssBackTakesA(back, false));
  EXPECT_FALSE(CssBackTakesA(back, true));
  back.button = 0x04;
  EXPECT_FALSE(CssBackTakesA(back, false));
  back.button = CSS_BUTTON_BACK;
  back.valid = false;
  EXPECT_FALSE(CssBackTakesA(back, false));
  EXPECT_TRUE(CssHandOnBack(CSS_HAND_EXIT, CSS_BUTTON_BACK));
  EXPECT_FALSE(CssHandOnBack(CSS_HAND_BUTTON, CSS_BUTTON_BACK));
}

TEST(OrcaOnlineRules, TheQueuesOwnSelectLetsThePlayerPickTheirTagWhileNotSearching)
{
  constexpr u16 A = PAD_BUTTON_A;
  FakeMemory m = Game("scSelctCharacter");
  m.Write16(Queue::RAW, 0);
  m.Write16(Queue::RAW + 2, 0);
  SetHand(m, 0, CSS_HAND_BUTTON, -21.5f, CSS_BUTTON_NAME, -19.0f);
  m.Write32(CSS_HAND + 0xB0, 0);
  for (const Mode mode : {Mode::Casual, Mode::Ranked})
  {
    WriteHeader(m, mode, Ruleset::Brawl, 0, 0, MB::FLAG_SOLO | MB::FLAG_QUEUE2);
    Rollback::InputGate::Masks masks = GateMasks(m);
    EXPECT_FALSE(masks[0].buttons & A);
    EXPECT_TRUE(masks[0].a_centres_stick);
    SetHand(m, 0, CSS_HAND_BUTTON, -21.5f, 0, -19.0f);
    m.Write32(CSS_AREA + 0x200, 2);
    masks = GateMasks(m);
    EXPECT_FALSE(masks[0].buttons & A);
    EXPECT_TRUE(masks[0].buttons & PAD_TRIGGER_L);
    EXPECT_TRUE(masks[0].a_centres_stick);
    EXPECT_TRUE(ReadCssNameList(m, 0));
    EXPECT_FALSE(ReadCssNameList(m, 1));
    m.Write32(CSS_AREA + 0x200, 0);
    SetHand(m, 0, CSS_HAND_BUTTON, -21.5f, CSS_BUTTON_NAME, -19.0f);
  }
  Queue::State ready;
  ready.ready = 1;
  Queue::WriteState(m, ready);
  EXPECT_TRUE(GateMasks(m)[0].buttons & A);
  Queue::WriteState(m, Queue::State{});
  EXPECT_FALSE(GateMasks(m)[0].buttons & A);
  SetHand(m, 0, CSS_HAND_BUTTON, -21.5f, CSS_BUTTON_PLAYER_TYPE, -24.0f);
  EXPECT_TRUE(GateMasks(m)[0].buttons & A);
  SetHand(m, 0, CSS_HAND_BUTTON, -21.5f, CSS_BUTTON_NAME, -19.0f);
  for (const Mode mode : {Mode::Casual, Mode::Ranked})
  {
    WriteHeader(m, mode, Ruleset::Brawl, 0, 0, MB::FLAG_QUEUE2);
    EXPECT_TRUE(GateMasks(m)[0].buttons & A);
    m.Write32(CSS_AREA + 0x200, 1);
    EXPECT_TRUE(GateMasks(m)[0].buttons & A);
    m.Write32(CSS_AREA + 0x200, 0);
  }
}

TEST(OrcaOnlineRules, BReachesTheCharacterSelectOnlyToTakeTheTokenUp)
{
  constexpr u16 B = PAD_BUTTON_B;
  FakeMemory m = Game("scSelctCharacter");
  const auto token = [&m](int port, bool in_hand, bool flying, u32 character = 0x00) {
    const u32 area = CSS_AREA + static_cast<u32>(port) * 0x1000;
    m.Fill(area + 0x1B0, 0x50);
    m.Write32(area + 0x1B4, 1);
    m.Write32(area + 0x1B8, character);
    m.Write8(area + 0x1F8, in_hand ? 1 : 0);
    m.Write8(area + 0x1F9, flying ? 1 : 0);
  };
  for (int port = 0; port < 4; ++port)
    token(port, false, false);
  for (const auto& mask : GateMasks(m))
    EXPECT_TRUE(mask.Empty());
  WriteHeader(m, Mode::Casual, Ruleset::Brawl);
  m.Write16(Queue::RAW, 0);
  m.Write16(Queue::RAW + 2, 0);
  EXPECT_TRUE(ReadCssToken(m, 0).Down());
  EXPECT_FALSE(GateMasks(m)[0].buttons & B);
  EXPECT_FALSE(GateMasks(m)[1].buttons & B);
  EXPECT_TRUE(GateMasks(m)[2].buttons & B);
  EXPECT_TRUE(GateMasks(m)[3].buttons & B);
  m.Write16(Queue::RAW, B);
  EXPECT_TRUE(GateMasks(m)[0].buttons & B);
  EXPECT_FALSE(GateMasks(m)[1].buttons & B);
  m.Write16(Queue::RAW, PAD_BUTTON_A);
  EXPECT_FALSE(GateMasks(m)[0].buttons & B);
  token(0, true, false);
  EXPECT_FALSE(ReadCssToken(m, 0).Down());
  EXPECT_TRUE(GateMasks(m)[0].buttons & B);
  token(0, false, true);
  EXPECT_TRUE(GateMasks(m)[0].buttons & B);
  token(0, true, true);
  EXPECT_TRUE(GateMasks(m)[0].buttons & B);
  token(0, false, false, 0x28);
  EXPECT_TRUE(GateMasks(m)[0].buttons & B);
  token(0, false, false);
  m.Write32(CSS_AREA + 0x1B4, 0);
  EXPECT_TRUE(GateMasks(m)[0].buttons & B);
  m.Write32(CSS_AREA + 0x1B4, 1);
  EXPECT_FALSE(GateMasks(m)[0].buttons & B);
  CssToken down;
  down.valid = down.human = true;
  down.character = 0x05;
  down.in_hand = false;
  EXPECT_TRUE(CssBUnpicks(down, false));
  EXPECT_FALSE(CssBUnpicks(down, true));
  EXPECT_FALSE(CssBUnpicks(CssToken{}, false));
  FakeMemory solo = Game("scSelctCharacter");
  WriteHeader(solo, Mode::Casual, Ruleset::Brawl, 0, 0, MB::FLAG_SOLO | MB::FLAG_QUEUE2);
  for (const auto& mask : GateMasks(solo))
    EXPECT_FALSE(mask.buttons & B);
  SetScene(m, "scSelStage");
  for (const auto& mask : GateMasks(m))
    EXPECT_TRUE(mask.buttons & B);
}

TEST(OrcaOnlineRules, TheHandsAreReadPerPort)
{
  FakeMemory m = Game("scSelctCharacter");
  SetHand(m, 2, CSS_HAND_BUTTON, -19.8f, CSS_BUTTON_PLAYER_TYPE, 7.5f);
  const CssHand hand = ReadCssHand(m, 2);
  EXPECT_TRUE(hand.valid);
  EXPECT_EQ(hand.target, CSS_HAND_BUTTON);
  EXPECT_EQ(hand.button, CSS_BUTTON_PLAYER_TYPE);
  EXPECT_EQ(hand.x, 7.5f);
  EXPECT_EQ(hand.y, -19.8f);
  EXPECT_EQ(ReadCssHand(m, 1).target, CSS_HAND_GRID);
  EXPECT_FALSE(ReadCssHand(m, 4).valid);
  EXPECT_FALSE(ReadCssHand(m, -1).valid);
  SetScene(m, "scMelee");
  EXPECT_FALSE(ReadCssHand(m, 2).valid);
}

TEST(OrcaOnlineRules, FriendsGamesAreUntouched)
{
  FakeMemory m = Game("scSelctCharacter");
  SetPick(m, 0, HUMAN, CHARACTER_ICE_CLIMBERS);
  m.writes = 0;
  EXPECT_EQ(ApplyLocks(m, NextFrame()), 0);
  EXPECT_EQ(m.writes, 0);
  m.Write32(MB::MAGIC, MB::MAGIC_VALUE);
  m.Write8(MB::VERSION, MB::VERSION_VALUE);
  m.Write8(MB::MODE, 0);
  m.Write8(MB::RULESET, 1);
  m.writes = 0;
  EXPECT_EQ(ApplyLocks(m, NextFrame()), 0);
  EXPECT_EQ(m.writes, 0);
  m.Write8(MB::MODE, 1);
  m.Write8(MB::VERSION, 9);
  EXPECT_FALSE(ReadHeader(m).present);
}

TEST(OrcaOnlineRules, RankedsReadyTimerFindsTheNoShow)
{
  FakeMemory m = Locked(Mode::Ranked, Ruleset::Brawl);
  m.Write8(MB::PLUGGED, 3);
  SetPick(m, 0, HUMAN, 0x00);
  for (int f = 0; f < CSS_READY_FRAMES; ++f)
    ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read16(MB::CSS_TIMER), CSS_READY_FRAMES - 1);
  EXPECT_EQ(m.Read8(MB::CSS_NO_SHOW), 0);
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read8(MB::CSS_NO_SHOW), 2);
  SetPick(m, 1, HUMAN, 0x02);
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read8(MB::CSS_NO_SHOW), 2);
  SetScene(m, "scMemoryChange");
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read8(MB::CSS_NO_SHOW), 0);
}

TEST(OrcaOnlineRules, TheReadyTimerWaitsForBothPlayersAndStopsWhenBothPick)
{
  FakeMemory m = Locked(Mode::Ranked, Ruleset::Brawl);
  SetPick(m, 0, HUMAN, 0x00);
  m.Write8(MB::PLUGGED, 1);
  for (int f = 0; f < CSS_READY_FRAMES + 5; ++f)
    ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read16(MB::CSS_TIMER), 0);
  EXPECT_EQ(m.Read8(MB::CSS_NO_SHOW), 0);
  m.Write8(MB::PLUGGED, 3);
  for (int f = 0; f < 101; ++f)
    ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read16(MB::CSS_TIMER), 100);
  SetPick(m, 1, HUMAN, 0x02);
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read16(MB::CSS_TIMER), 0);
  SetPick(m, 0, NOBODY, NONE);
  SetPick(m, 1, NOBODY, NONE);
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read16(MB::CSS_TIMER), 0);
  SetScene(m, "scMelee");
  ApplyLocks(m, NextFrame());
  SetScene(m, "scSelctCharacter");
  SetPick(m, 0, HUMAN, 0x00);
  for (int f = 0; f < CSS_READY_FRAMES + 5; ++f)
    ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read8(MB::CSS_NO_SHOW), 0);
  FakeMemory c = Locked(Mode::Casual, Ruleset::Brawl);
  c.Write8(MB::PLUGGED, 3);
  SetPick(c, 0, HUMAN, 0x00);
  for (int f = 0; f < CSS_READY_FRAMES + 5; ++f)
    ApplyLocks(c, NextFrame());
  EXPECT_EQ(c.Read16(MB::CSS_TIMER), 0);
  EXPECT_EQ(c.Read8(MB::CSS_NO_SHOW), 0);
}

TEST(OrcaOnlineRules, TheReadyTimerIsTheSameWhenABoundaryRunsAgain)
{
  FakeMemory m = Locked(Mode::Ranked, Ruleset::Brawl);
  m.Write8(MB::PLUGGED, 3);
  SetPick(m, 0, HUMAN, 0x00);
  for (int f = 1000; f < 1100; ++f)
    ApplyLocks(m, f);
  const auto once = m.bytes;
  EXPECT_EQ(ApplyLocks(m, 1099), 0);
  EXPECT_EQ(m.bytes, once);
  EXPECT_EQ(m.Read16(MB::CSS_TIMER), 99);
}

TEST(OrcaOnlineRules, TheHeaderCarriesTheCoinAndANewRoomStartsANewSet)
{
  FakeMemory m = Game("scSelctCharacter");
  EXPECT_GT(WriteHeader(m, Mode::Ranked, Ruleset::Brawl, 1, 0x1234), 0);
  EXPECT_EQ(m.Read8(MB::COIN), 1);
  EXPECT_EQ(m.Read32(MB::ROOM), 0x1234u);
  const auto state = Orca::UX::MatchBlock::Read(m);
  ASSERT_TRUE(state.has_value());
  EXPECT_EQ(state->coin, 1);
  EXPECT_EQ(state->last_winner, Orca::UX::MatchBlock::NONE);
  EXPECT_EQ(Orca::UX::SetBlock::ReadSet(m).last_winner, Orca::UX::SetBlock::NO_PORT);
  EXPECT_EQ(WriteHeader(m, Mode::Ranked, Ruleset::Brawl, 1, 0x1234), 0);
  Orca::UX::SetBlock::SetState set;
  set.games = 1;
  set.wins = {1, 0};
  set.last_winner = 0;
  Orca::UX::SetBlock::WriteSet(m, set);
  EXPECT_GT(WriteHeader(m, Mode::Ranked, Ruleset::Brawl, 0, 0x5678), 0);
  EXPECT_EQ(Orca::UX::SetBlock::ReadSet(m), Orca::UX::SetBlock::SetState{});
  EXPECT_EQ(m.Read8(MB::COIN), 0);
}

TEST(OrcaOnlineRules, TheTestQueueKnobTakesACoin)
{
  using Orca::UX::Rules::ParseTestQueue;
  using Orca::UX::Rules::TestQueue;
  EXPECT_EQ(ParseTestQueue("ranked"), (TestQueue{Mode::Ranked, 0}));
  EXPECT_EQ(ParseTestQueue("ranked:1"), (TestQueue{Mode::Ranked, 1}));
  EXPECT_EQ(ParseTestQueue("casual:0"), (TestQueue{Mode::Casual, 0}));
  EXPECT_FALSE(ParseTestQueue("ranked:2").has_value());
  EXPECT_EQ(ParseTestQueue("casual:q2"), (TestQueue{Mode::Casual, 0, MB::FLAG_QUEUE2}));
  EXPECT_EQ(ParseTestQueue("ranked:1:q2"), (TestQueue{Mode::Ranked, 1, MB::FLAG_QUEUE2}));
  EXPECT_EQ(ParseTestQueue("casual:solo"),
            (TestQueue{Mode::Casual, 0, MB::FLAG_SOLO | MB::FLAG_QUEUE2}));
  EXPECT_FALSE(ParseTestQueue("casual:q2:1").has_value());
  EXPECT_FALSE(ParseTestQueue("casual:q2:solo").has_value());
  EXPECT_FALSE(ParseTestQueue("casual:0:1").has_value());
  EXPECT_FALSE(ParseTestQueue("brawl").has_value());
  EXPECT_FALSE(ParseTestQueue("").has_value());
}

TEST(OrcaOnlineRules, LocksArePureFunctionsOfMemory)
{
  FakeMemory a = Locked(Mode::Casual, Ruleset::Brawl);
  FakeMemory b = Locked(Mode::Casual, Ruleset::Brawl);
  SetPick(a, 0, HUMAN, CHARACTER_ICE_CLIMBERS);
  SetPick(b, 0, HUMAN, CHARACTER_ICE_CLIMBERS);
  EXPECT_EQ(ApplyLocks(a, 500), ApplyLocks(b, 500));
  EXPECT_EQ(a.bytes, b.bytes);
}

TEST(OrcaOnlineRules, TheQueuesFlagsAreTheHeaders)
{
  constexpr u16 B = PAD_BUTTON_B;
  FakeMemory m = Game("scSelctCharacter");
  EXPECT_GT(WriteHeader(m, Mode::Casual, Ruleset::Brawl, 0, 0, MB::FLAG_SOLO | MB::FLAG_QUEUE2),
            0);
  Header h = ReadHeader(m);
  EXPECT_TRUE(h.Locked());
  EXPECT_TRUE(h.Solo());
  EXPECT_TRUE(h.Queue2());
  EXPECT_EQ(m.Read8(MB::FLAGS), MB::FLAG_SOLO | MB::FLAG_QUEUE2);
  for (const auto& mask : GateMasks(m))
    EXPECT_EQ(mask.buttons & B, 0);
  EXPECT_EQ(WriteHeader(m, Mode::Casual, Ruleset::Brawl, 0, 0, MB::FLAG_SOLO | MB::FLAG_QUEUE2),
            0);
  m.Write8(MB::QUEUE, 1);
  EXPECT_GT(WriteHeader(m, Mode::Casual, Ruleset::Brawl, 1, 0x77, MB::FLAG_QUEUE2), 0);
  h = ReadHeader(m);
  EXPECT_FALSE(h.Solo());
  EXPECT_TRUE(h.Queue2());
  EXPECT_EQ(m.Read8(MB::QUEUE), 0);
  for (const auto& mask : GateMasks(m))
    EXPECT_EQ(mask.buttons & B, B);
  EXPECT_GT(WriteHeader(m, Mode::None, Ruleset::Brawl), 0);
  EXPECT_EQ(m.Read8(SET_RULE + 0x0A), 1);
  EXPECT_EQ(m.Read32(MENU_DATA + 0x08), 0xFFFFFFFFu);
}

TEST(OrcaOnlineRules, TheRoomsHeaderComesFromItsCode)
{
  const Header room = HeaderFor(Mode::Casual, Ruleset::PPlus, "rfkyxp4s", MB::FLAG_QUEUE2);
  EXPECT_TRUE(room.Locked());
  EXPECT_FALSE(room.Solo());
  EXPECT_TRUE(room.Queue2());
  EXPECT_EQ(room.coin, MB::CoinFromCode("rfkyxp4s"));
  EXPECT_EQ(room.room, MB::RoomHash("rfkyxp4s"));
  const Header own = HeaderFor(Mode::Casual, Ruleset::PPlus, "rfkyxp4s",
                               MB::FLAG_SOLO | MB::FLAG_QUEUE2);
  EXPECT_TRUE(own.Solo());
  EXPECT_EQ(own.coin, 0);
  EXPECT_EQ(own.room, 0u);
  EXPECT_FALSE(HeaderFor(Mode::None, Ruleset::PPlus, "rfkyxp4s", MB::FLAG_QUEUE2).present);
  EXPECT_FALSE(HeaderFor(Mode::Casual, Ruleset::None, "rfkyxp4s", 0).present);
  for (const Header& h : {room, own})
  {
    FakeMemory m = Game("scSelctCharacter");
    EXPECT_GT(WriteHeader(m, h.mode, Ruleset::PPlus, h.coin, h.room, h.flags), 0);
    EXPECT_EQ(ReadHeader(m), h);
    EXPECT_TRUE(HeaderIs(ReadHeader(m), h));
  }
}

TEST(OrcaOnlineRules, AJoinerRefusesAHeaderThatIsNotItsRooms)
{
  const std::string code = "rfkyxp4s";
  FakeMemory m = Game("scSelctCharacter");
  WriteHeader(m, Mode::Casual, Ruleset::Brawl, 0, 0, MB::FLAG_SOLO | MB::FLAG_QUEUE2);
  Header h = ReadHeader(m);
  ASSERT_TRUE(h.Locked());
  EXPECT_EQ(h.mode, Mode::Casual);
  EXPECT_EQ(h.ruleset, Ruleset::Brawl);
  EXPECT_FALSE(HeaderFitsRoom(h, Mode::Casual, Ruleset::Brawl, code, MB::FLAG_QUEUE2));
  EXPECT_FALSE(HeaderFitsRoom(h, Mode::Casual, Ruleset::Brawl, code,
                              MB::FLAG_SOLO | MB::FLAG_QUEUE2));

  const Header room = HeaderFor(Mode::Casual, Ruleset::Brawl, code, MB::FLAG_QUEUE2);
  WriteHeader(m, room.mode, Ruleset::Brawl, room.coin, room.room, room.flags);
  h = ReadHeader(m);
  EXPECT_TRUE(HeaderFitsRoom(h, Mode::Casual, Ruleset::Brawl, code, MB::FLAG_QUEUE2));
  EXPECT_FALSE(HeaderFitsRoom(h, Mode::Ranked, Ruleset::Brawl, code, MB::FLAG_QUEUE2));
  EXPECT_FALSE(HeaderFitsRoom(h, Mode::Casual, Ruleset::PPlus, code, MB::FLAG_QUEUE2));
  EXPECT_FALSE(HeaderFitsRoom(h, Mode::Casual, Ruleset::Brawl, "zzzzzzzz", MB::FLAG_QUEUE2));
  EXPECT_FALSE(HeaderFitsRoom(h, Mode::Casual, Ruleset::Brawl, code, 0));
  WriteHeader(m, room.mode, Ruleset::Brawl, room.coin, room.room, 0);
  EXPECT_TRUE(HeaderFitsRoom(ReadHeader(m), Mode::Casual, Ruleset::Brawl, code, 0));
  EXPECT_FALSE(HeaderFitsRoom(ReadHeader(m), Mode::Casual, Ruleset::Brawl, code, MB::FLAG_QUEUE2));
  WriteHeader(m, room.mode, Ruleset::Brawl, static_cast<u8>(room.coin ^ 1), room.room, room.flags);
  EXPECT_FALSE(HeaderFitsRoom(ReadHeader(m), Mode::Casual, Ruleset::Brawl, code, MB::FLAG_QUEUE2));

  EXPECT_FALSE(HeaderFitsRoom(ReadHeader(m), Mode::None, Ruleset::Brawl, code, MB::FLAG_QUEUE2));
  WriteHeader(m, Mode::None, Ruleset::Brawl);
  EXPECT_TRUE(HeaderFitsRoom(ReadHeader(m), Mode::None, Ruleset::Brawl, code, MB::FLAG_QUEUE2));
  EXPECT_TRUE(HeaderFitsRoom(ReadHeader(Game("scSelctCharacter")), Mode::None, Ruleset::Brawl, "",
                             0));
  EXPECT_FALSE(HeaderFitsRoom(ReadHeader(Game("scSelctCharacter")), Mode::Casual, Ruleset::Brawl,
                              code, MB::FLAG_QUEUE2));
}

TEST(OrcaOnlineRules, EveryChangeOfTheHeaderCalledForCounts)
{
  SetWanted(Mode::None);
  const u64 g = WantedGeneration();
  EXPECT_FALSE(SetWanted(Mode::None, MB::FLAG_QUEUE2, "abcdefgh"));
  EXPECT_EQ(WantedGeneration(), g);
  EXPECT_TRUE(SetWanted(Mode::Casual, MB::FLAG_SOLO | MB::FLAG_QUEUE2));
  EXPECT_FALSE(HeaderInPlace());
  EXPECT_FALSE(SetWanted(Mode::Casual, MB::FLAG_SOLO | MB::FLAG_QUEUE2, "abcdefgh"));
  EXPECT_EQ(WantedGeneration(), g + 1);
  EXPECT_TRUE(SetWanted(Mode::Casual, MB::FLAG_QUEUE2, "abcdefgh"));
  EXPECT_FALSE(SetWanted(Mode::Casual, MB::FLAG_QUEUE2, "abcdefgh"));
  EXPECT_TRUE(SetWanted(Mode::Casual, MB::FLAG_QUEUE2, "ijklmnop"));
  EXPECT_TRUE(SetWanted(Mode::Ranked, MB::FLAG_QUEUE2, "ijklmnop"));
  EXPECT_EQ(WantedGeneration(), g + 4);
  EXPECT_EQ(Wanted(), Mode::Ranked);
  EXPECT_EQ(WantedFlags(), MB::FLAG_QUEUE2);
  MemoryReplaced();
  EXPECT_FALSE(HeaderInPlace());
  SetWanted(Mode::None);
  EXPECT_TRUE(HeaderIs(ReadHeader(Game("scSelctCharacter")), Header{}));
  EXPECT_FALSE(HeaderIs(ReadHeader(Locked(Mode::Casual, Ruleset::Brawl)), Header{}));
}

TEST(OrcaOnlineRules, TheRoomsHeaderLandsWithTheOpponentOnTheWay)
{
  using Rollback::OnlineMatch::AloneAt;
  using Rollback::OnlineMatch::HeaderFreeAt;
  using Rollback::OnlineMatch::KEYFRAME_FRESH_FRAMES;
  EXPECT_FALSE(AloneAt(5000, false, true, std::nullopt));
  EXPECT_TRUE(HeaderFreeAt(5000, true, std::nullopt));
  EXPECT_FALSE(HeaderFreeAt(5000, false, std::nullopt));
  EXPECT_FALSE(HeaderFreeAt(5000, true, 4000));
  EXPECT_FALSE(HeaderFreeAt(4000 + KEYFRAME_FRESH_FRAMES, true, 4000));
  EXPECT_TRUE(HeaderFreeAt(4000 + KEYFRAME_FRESH_FRAMES + 1, true, 4000));
}

TEST(OrcaOnlineRules, AQueueRoomNeverMakesAKeyframeWithoutItsHeader)
{
  using Rollback::OnlineMatch::HEADER_FAIL_BOUNDARIES;
  using Rollback::OnlineMatch::HEADER_WAIT_BOUNDARIES;
  using Rollback::OnlineMatch::HeaderWait;
  using Rollback::OnlineMatch::StepHeaderWait;
  int waited = 0;
  EXPECT_EQ(StepHeaderWait(&waited, true, true, true), HeaderWait::Ready);
  for (int i = 0; i < 2 * HEADER_FAIL_BOUNDARIES; ++i)
    EXPECT_EQ(StepHeaderWait(&waited, false, false, true), HeaderWait::Wait);
  EXPECT_EQ(waited, 0);
  int ready = 0, fails = 0, fail_at = -1;
  for (int i = 1; i <= HEADER_FAIL_BOUNDARIES + 1; ++i)
  {
    const HeaderWait w = StepHeaderWait(&waited, false, true, true);
    ready += w == HeaderWait::Ready;
    if (w == HeaderWait::Fail)
    {
      ++fails;
      fail_at = i;
    }
  }
  EXPECT_EQ(ready, 0);
  EXPECT_EQ(fails, 1);
  EXPECT_EQ(fail_at, HEADER_FAIL_BOUNDARIES + 1);
  EXPECT_EQ(waited, 0);
  for (int i = 0; i < 3; ++i)
    EXPECT_EQ(StepHeaderWait(&waited, false, true, true), HeaderWait::Wait);
  EXPECT_EQ(StepHeaderWait(&waited, true, true, true), HeaderWait::Ready);
  EXPECT_EQ(waited, 0);
  for (int i = 1; i <= HEADER_WAIT_BOUNDARIES; ++i)
    EXPECT_EQ(StepHeaderWait(&waited, false, true, false), HeaderWait::Wait);
  EXPECT_EQ(StepHeaderWait(&waited, false, true, false), HeaderWait::Ready);
  for (int i = 0; i < 2 * HEADER_FAIL_BOUNDARIES; ++i)
    EXPECT_NE(StepHeaderWait(&waited, false, true, false), HeaderWait::Fail);
}

namespace
{
constexpr u32 FLIGHT = 0x804E23BC;
constexpr u32 P1_SHIELD = 0x804E0F08;
constexpr u32 P2_ALC = 0x804E1534;
constexpr u32 HITSTUN = 0x804E1E28;
constexpr u32 DEBUG_MODE = 0x804E0D3C;

void LoadCodeMenu(FakeMemory& m, u32 activation = 0)
{
  m.Fill(CODE_MENU, CODE_MENU_END - CODE_MENU);
  m.Write32(CODE_MENU, CODE_MENU_ROOT);
  m.Write32(CODE_MENU + 4, CODE_MENU_ROOT);
  m.Write16(0x804E0C30, 0x0058);
  m.Write32(0x804E0C48, 0x804E0C30);
  m.Write16(0x804E23B4, 0x0030);
  m.Write32(0x804E23CC, 0x804E0F00);
  for (const CodeMenuWord& word : CodeMenuDefaults())
    m.Write32(word.address, word.value);
  m.Write32(CODE_MENU_ACTIVATION, activation);
  for (int port = 0; port < 4; ++port)
    m.Write32(CODE_MENU_CHARACTERS[port], 0x0C);
  m.Fill(CODE_MENU_GAME_PAUSE, 4);
  m.writes = 0;
}

void Cheat(FakeMemory& m)
{
  m.Write32(FLIGHT, 1);
  m.Write32(P1_SHIELD, 1);
  m.Write32(P2_ALC, 1);
  m.Write32(HITSTUN, 0x3F800000);
  m.Write32(DEBUG_MODE, 1);
  m.Write32(CODE_MENU_ACTIVATION, 0);
}

FakeMemory PPlusWithCodeMenu(Mode mode, std::string_view scene, u8 flags = 0)
{
  FakeMemory m = Game(scene);
  LoadCodeMenu(m);
  WriteHeader(m, mode, Ruleset::PPlus, 0, 0, flags);
  m.writes = 0;
  return m;
}

std::map<u32, u8> CodeMenuBytes(const FakeMemory& m)
{
  std::map<u32, u8> out;
  for (u32 a = CODE_MENU; a < CODE_MENU_END; ++a)
    out[a] = m.Read8(a);
  return out;
}
}

TEST(OrcaOnlineRules, TheCodeMenuDefaultsAreTheFilesWords)
{
  const auto defaults = CodeMenuDefaults();
  EXPECT_EQ(defaults.size(), 73u);
  std::map<u32, u32> seen;
  for (const CodeMenuWord& word : defaults)
  {
    EXPECT_GE(word.address, CODE_MENU_ROOT);
    EXPECT_LT(word.address + 3, CODE_MENU_END);
    EXPECT_EQ(word.address % 4, 0u);
    EXPECT_TRUE(seen.emplace(word.address, word.value).second) << std::hex << word.address;
    for (u32 character : CODE_MENU_CHARACTERS)
      EXPECT_NE(word.address, character);
  }
  EXPECT_EQ(seen[CODE_MENU_ACTIVATION], CODE_MENU_OFF);
  EXPECT_EQ(seen[FLIGHT], 0u);
  EXPECT_EQ(seen[0x804E0ECC], 1u);
}

TEST(OrcaOnlineRules, TheCodeMenuIsOffAndAtItsDefaultsUnderAProjectPlusHeader)
{
  for (const u8 flags : {u8{0}, u8{MB::FLAG_QUEUE2}, u8(MB::FLAG_SOLO | MB::FLAG_QUEUE2)})
  {
    for (Mode mode : {Mode::Casual, Mode::Ranked})
    {
      for (const char* scene : {"scSelctCharacter", "scSelStage", "scMemoryChange", "scMelee",
                                "scVsResult"})
      {
        FakeMemory m = PPlusWithCodeMenu(mode, scene, flags);
        Cheat(m);
        m.Write32(CODE_MENU_CHARACTERS[1], 0x2C);
        EXPECT_GT(ApplyLocks(m, NextFrame()), 0) << scene;
        EXPECT_EQ(m.Read32(CODE_MENU_ACTIVATION), CODE_MENU_OFF) << scene;
        EXPECT_EQ(m.Read32(FLIGHT), 0u);
        EXPECT_EQ(m.Read32(P1_SHIELD), 0u);
        EXPECT_EQ(m.Read32(P2_ALC), 0u);
        EXPECT_EQ(m.Read32(DEBUG_MODE), 0u);
        EXPECT_EQ(m.Read32(HITSTUN), 0x3ECCCCCDu);
        for (const CodeMenuWord& word : CodeMenuDefaults())
          EXPECT_EQ(m.Read32(word.address), word.value) << std::hex << word.address;
        EXPECT_EQ(m.Read32(CODE_MENU_CHARACTERS[0]), 0x0Cu);
        EXPECT_EQ(m.Read32(CODE_MENU_CHARACTERS[1]), 0x2Cu);
        m.writes = 0;
        EXPECT_EQ(ApplyLocks(m, NextFrame()), 0) << scene;
        EXPECT_EQ(m.writes, 0);
      }
    }
  }
}

TEST(OrcaOnlineRules, AnOpenCodeMenuClosesAndTheGameRunsAgain)
{
  FakeMemory m = PPlusWithCodeMenu(Mode::Ranked, "scMelee");
  m.Write32(CODE_MENU_STATE, CODE_MENU_OPEN);
  m.Write32(CODE_MENU_SAVED_PAUSE, 0);
  m.Write32(CODE_MENU_GAME_PAUSE, 1);
  m.track_writes = true;
  ApplyLocks(m, NextFrame());
  m.track_writes = false;
  const auto regions = BrawlGame::Regions();
  ASSERT_FALSE(m.recorded.empty());
  for (const u32 address : m.recorded)
    EXPECT_TRUE(std::any_of(regions.begin(), regions.end(),
                           [&](const auto& region) { return region.Contains(address, 1); }))
        << std::hex << address;

  EXPECT_EQ(m.Read32(CODE_MENU_STATE), 0u);
  EXPECT_EQ(m.Read32(CODE_MENU_GAME_PAUSE), 0u);
  m.writes = 0;
  EXPECT_EQ(ApplyLocks(m, NextFrame()), 0);
  for (u32 state : {1u, 2u, 3u})
  {
    m.Write32(CODE_MENU_STATE, state);
    m.Write32(CODE_MENU_GAME_PAUSE, 1);
    ApplyLocks(m, NextFrame());
    EXPECT_EQ(m.Read32(CODE_MENU_STATE), 0u) << state;
    EXPECT_EQ(m.Read32(CODE_MENU_GAME_PAUSE), 1u) << state;
  }
}

TEST(OrcaOnlineRules, TheCodeMenuIsLeftAloneWithoutItsHeaderOrItsLayout)
{
  {
    FakeMemory m = Game("scMelee");
    LoadCodeMenu(m);
    Cheat(m);
    const auto before = CodeMenuBytes(m);
    EXPECT_EQ(ApplyLocks(m, NextFrame()), 0);
    EXPECT_EQ(CodeMenuBytes(m), before);
  }
  {
    FakeMemory m = Game("scMelee");
    LoadCodeMenu(m);
    WriteHeader(m, Mode::Ranked, Ruleset::Brawl);
    Cheat(m);
    const auto before = CodeMenuBytes(m);
    ApplyLocks(m, NextFrame());
    EXPECT_EQ(CodeMenuBytes(m), before);
  }
  const struct
  {
    u32 address;
    u32 value;
  } breaks[] = {
      {CODE_MENU, 0x804E0000},     {CODE_MENU, CODE_MENU_END}, {CODE_MENU + 4, 0x804E0800},
      {0x804E0C30, 0x00540000},   {0x804E0C48, 0x804E0C34},   {0x804E23B4, 0x00340000},
      {0x804E23CC, 0x804E0F40},
  };
  for (const auto& broken : breaks)
  {
    FakeMemory m = PPlusWithCodeMenu(Mode::Ranked, "scMelee");
    m.Write32(broken.address, broken.value);
    EXPECT_FALSE(CodeMenuPresent(m)) << std::hex << broken.address;
    Cheat(m);
    m.Write32(CODE_MENU_STATE, CODE_MENU_OPEN);
    const auto before = CodeMenuBytes(m);
    ApplyLocks(m, NextFrame());
    EXPECT_EQ(CodeMenuBytes(m), before) << std::hex << broken.address;
  }
  FakeMemory empty = Locked(Mode::Ranked, Ruleset::PPlus, "scMelee");
  EXPECT_FALSE(CodeMenuPresent(empty));
}

TEST(OrcaOnlineRules, ClearingGivesBackTheCodeMenusActivation)
{
  FakeMemory m = Game("scSelctCharacter");
  LoadCodeMenu(m, 1);
  m.Write32(FLIGHT, 1);
  WriteHeader(m, Mode::Ranked, Ruleset::PPlus, 0, 0, MB::FLAG_SOLO | MB::FLAG_QUEUE2);
  EXPECT_EQ(m.Read8(MB::SAVED_CODE_MENU), 1);
  EXPECT_EQ(m.Read8(MB::SAVED_FLAGS) & 4, 4);
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read32(CODE_MENU_ACTIVATION), CODE_MENU_OFF);
  WriteHeader(m, Mode::Ranked, Ruleset::PPlus, 1, 0x1234, MB::FLAG_QUEUE2);
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read8(MB::SAVED_CODE_MENU), 1);
  EXPECT_GT(WriteHeader(m, Mode::None, Ruleset::PPlus), 0);
  EXPECT_EQ(m.Read32(CODE_MENU_ACTIVATION), 1u);
  EXPECT_EQ(m.Read32(FLIGHT), 0u);
  FakeMemory late = Game("scSelctCharacter");
  WriteHeader(late, Mode::Casual, Ruleset::PPlus);
  EXPECT_EQ(late.Read8(MB::SAVED_FLAGS) & 4, 0);
  LoadCodeMenu(late, 0);
  ApplyLocks(late, NextFrame());
  WriteHeader(late, Mode::None, Ruleset::PPlus);
  EXPECT_EQ(late.Read32(CODE_MENU_ACTIVATION), CODE_MENU_OFF);
  FakeMemory brawl = Game("scSelctCharacter");
  LoadCodeMenu(brawl, 0);
  WriteHeader(brawl, Mode::Casual, Ruleset::Brawl);
  EXPECT_EQ(brawl.Read8(MB::SAVED_FLAGS) & 4, 0);
}

namespace
{
constexpr u32 RECORD = 0x90180B40;
u32 RecordCharacter(int port)
{
  return RECORD + 0xB8 + static_cast<u32>(port) * 0x5C;
}
u32 RecordSlot(int port)
{
  return RECORD + 0x0A + static_cast<u32>(port) * 4;
}
void BuildRecord(FakeMemory& m)
{
  m.Write32(0x90181300 + 0x10, RECORD);
  m.Fill(RECORD, 0xB8 + 4 * 0x5C);
  for (int port = 0; port < 4; ++port)
  {
    m.Write8(RecordCharacter(port), NONE);
    m.Write8(RecordSlot(port), 0x28);
  }
}
}

TEST(OrcaOnlineRules, ProjectPlusBannedFightersPlayAsTheirBaseFighters)
{
  for (Mode mode : {Mode::Casual, Mode::Ranked})
  {
    for (const char* scene : {"scSelctCharacter", "scSelStage", "scMemoryChange"})
    {
      FakeMemory m = Locked(mode, Ruleset::PPlus, scene);
      BuildRecord(m);
      SetPick(m, 0, HUMAN, CHARACTER_GIGA_BOWSER);
      SetPick(m, 1, HUMAN, CHARACTER_WARIO_MAN);
      SetPick(m, 2, NOBODY, 0x11);
      m.Write8(RecordCharacter(0), CHARACTER_GIGA_BOWSER);
      m.Write8(RecordCharacter(1), CHARACTER_WARIO_MAN);
      m.Write8(RecordCharacter(2), 0x11);
      m.Write8(RecordSlot(0), 0x38);
      m.Write8(RecordSlot(1), 0x36);
      m.Write8(RecordSlot(2), 0x37);
      ApplyLocks(m, NextFrame());
      EXPECT_EQ(ReadPicks(m)[0].character, CHARACTER_BOWSER) << scene;
      EXPECT_EQ(ReadPicks(m)[1].character, CHARACTER_WARIO) << scene;
      EXPECT_EQ(m.Read8(MELEE + 0x98 + 2 * 0x5C), 0x11);
      EXPECT_EQ(m.Read8(RecordCharacter(0)), CHARACTER_BOWSER) << scene;
      EXPECT_EQ(m.Read8(RecordCharacter(1)), CHARACTER_WARIO) << scene;
      EXPECT_EQ(m.Read8(RecordCharacter(2)), 0x11);
      EXPECT_EQ(m.Read8(RecordSlot(0)), 0x0C) << scene;
      EXPECT_EQ(m.Read8(RecordSlot(1)), 0x15) << scene;
      EXPECT_EQ(m.Read8(RecordSlot(2)), 0x37);
      m.writes = 0;
      EXPECT_EQ(ApplyLocks(m, NextFrame()), 0);
    }
  }
  FakeMemory fight = Locked(Mode::Ranked, Ruleset::PPlus, "scMelee");
  BuildRecord(fight);
  SetPick(fight, 0, HUMAN, CHARACTER_GIGA_BOWSER);
  fight.Write8(RecordSlot(0), 0x38);
  ApplyLocks(fight, NextFrame());
  EXPECT_EQ(ReadPicks(fight)[0].character, CHARACTER_GIGA_BOWSER);
  EXPECT_EQ(fight.Read8(RecordSlot(0)), 0x38);
  FakeMemory brawl = Locked(Mode::Ranked, Ruleset::Brawl);
  SetPick(brawl, 0, HUMAN, CHARACTER_GIGA_BOWSER);
  ApplyLocks(brawl, NextFrame());
  EXPECT_EQ(ReadPicks(brawl)[0].character, CHARACTER_GIGA_BOWSER);
  FakeMemory friends = Game("scSelctCharacter");
  SetPick(friends, 0, HUMAN, CHARACTER_WARIO_MAN);
  ApplyLocks(friends, NextFrame());
  EXPECT_EQ(ReadPicks(friends)[0].character, CHARACTER_WARIO_MAN);
}

TEST(OrcaOnlineRules, ProjectPlusNeverSeesTheCodeMenusChord)
{
  for (const char* scene : {"scSelctCharacter", "scSelStage", "scMemoryChange", "scMelee",
                            "scVsResult", "muMenuMain"})
  {
    FakeMemory p = Locked(Mode::Ranked, Ruleset::PPlus, scene);
    for (const auto& mask : GateMasks(p))
      EXPECT_EQ(mask.drop_with_lr, PAD_BUTTON_DOWN) << scene;
    FakeMemory b = Locked(Mode::Ranked, Ruleset::Brawl, scene);
    for (const auto& mask : GateMasks(b))
      EXPECT_EQ(mask.drop_with_lr, 0) << scene;
    FakeMemory friends = Game(scene);
    for (const auto& mask : GateMasks(friends))
      EXPECT_TRUE(mask.Empty()) << scene;
  }
  FakeMemory fight = Locked(Mode::Casual, Ruleset::PPlus, "scMelee");
  for (const auto& mask : GateMasks(fight))
    EXPECT_EQ(mask.buttons, PAD_BUTTON_START);
}

TEST(OrcaOnlineRules, TheCodeMenuLockIsAPureFunctionOfMemory)
{
  FakeMemory a = PPlusWithCodeMenu(Mode::Ranked, "scSelStage");
  Cheat(a);
  a.Write32(CODE_MENU_STATE, CODE_MENU_OPEN);
  a.Write32(CODE_MENU_SAVED_PAUSE, 0);
  a.Write32(CODE_MENU_GAME_PAUSE, 1);
  SetPick(a, 0, HUMAN, CHARACTER_WARIO_MAN);
  FakeMemory b = a;
  EXPECT_EQ(ApplyLocks(a, 700), ApplyLocks(b, 700));
  EXPECT_EQ(a.bytes, b.bytes);
}

TEST(OrcaOnlineRules, RankedBrawlKeepsFinalDestinationAndAddsFrigate)
{
  for (const u8 character : {u8{0}, u8{CHARACTER_ICE_CLIMBERS}})
  {
    FakeMemory m = Locked(Mode::Ranked, Ruleset::Brawl);
    SetPick(m, 0, HUMAN, character);
    ApplyLocks(m, NextFrame());
    EXPECT_EQ(m.Read32(MENU_DATA + 0x24), 0x04005807u);
    EXPECT_EQ(m.Read32(MENU_DATA + 0x20), BRAWL_LEGAL_MELEE_PAGE);
  }
}
