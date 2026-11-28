// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "Common/CommonTypes.h"
#include "Core/Orca/Games/Brawl/MatchBlock.h"
#include "Core/Orca/Policy/CssRules.h"
#include "Core/Orca/Games/Brawl/SetBlock.h"
#include "Core/Rollback/InputGate.h"

namespace Core
{
class CPUThreadGuard;
}

namespace Orca::UX
{
class GuestMemory;

namespace Rules
{
using MatchBlock::Mode;
using MatchBlock::Ruleset;

struct Header
{
  bool present = false;
  Mode mode = Mode::None;
  Ruleset ruleset = Ruleset::None;
  u8 flags = 0;
  u8 coin = 0;
  u32 room = 0;
  bool Locked() const { return present && mode != Mode::None && ruleset != Ruleset::None; }
  bool Solo() const { return Locked() && (flags & MatchBlock::FLAG_SOLO) != 0; }
  bool Queue2() const { return Locked() && (flags & MatchBlock::FLAG_QUEUE2) != 0; }
  bool operator==(const Header&) const = default;
};
Header ReadHeader(const GuestMemory& memory);

Ruleset ProfileRuleset();

int WriteHeader(GuestMemory& memory, Mode mode, Ruleset ruleset, u8 coin = 0, u32 room = 0,
                u8 flags = 0);

Header HeaderFor(Mode mode, Ruleset ruleset, std::string_view code, u8 flags);
bool HeaderIs(const Header& in_memory, const Header& wanted);

int ApplyLocks(GuestMemory& memory, int frame);

Rollback::InputGate::Masks GateMasks(const GuestMemory& memory);

Rollback::InputGate::Masks SessionMasks(const GuestMemory& memory);

constexpr float CSS_A_TOP = 15.0f;
constexpr float CSS_A_BOTTOM = -3.0f;
constexpr float CSS_A_BOTTOM_BRAWL = -4.2f;

struct CssHand
{
  bool valid = false;
  u32 target = CSS_HAND_NOTHING;
  u32 button = 0;
  float x = 0;
  float y = 0;
  int panel = -1;
  u32 state = 0;
  bool operator==(const CssHand&) const = default;
};
constexpr u32 CSS_HAND_STATE_KEYPAD = 8;
u32 ReadCssArea(const GuestMemory& memory, int port);
CssHand ReadCssHand(const GuestMemory& memory, int port);
bool CssHandMayPressA(const CssHand& hand, float bottom = CSS_A_BOTTOM);

constexpr bool CssBackTakesA(const CssHand& hand, bool a_held)
{
  return hand.valid && CssHandOnBack(hand.target, hand.button) && !a_held;
}

constexpr u32 CSS_AREA_NAME_LIST = 0x200;
constexpr bool CssNameTakesA(const CssHand& hand, int port, bool list_open)
{
  return list_open || (hand.valid && hand.state == CSS_HAND_STATE_KEYPAD) ||
         (hand.valid && hand.target == CSS_HAND_BUTTON && hand.button == CSS_BUTTON_NAME &&
          hand.panel == port);
}
bool ReadCssNameList(const GuestMemory& memory, int port);

CssToken ReadCssToken(const GuestMemory& memory, int port);
bool CssBMayUnpick(const GuestMemory& memory, int port);

constexpr u32 BRAWL_LEGAL_BRAWL_PAGE = 0x04005007;
constexpr u32 BRAWL_RANKED_BRAWL_PAGE = 0x04005807;
constexpr u32 BRAWL_LEGAL_MELEE_PAGE = 0x00020000;
constexpr u32 BRAWL_FINAL_DESTINATION_BIT = 0x00000002;

constexpr int CHARACTER_ICE_CLIMBERS = 0x10;
constexpr int CHARACTER_NONE = 0x3E;
constexpr int CHARACTER_BOWSER = 0x0C;
constexpr int CHARACTER_WARIO = 0x17;
constexpr int CHARACTER_GIGA_BOWSER = 0x2C;
constexpr int CHARACTER_WARIO_MAN = 0x2D;

constexpr u32 CODE_MENU = 0x804E0000;
constexpr u32 CODE_MENU_END = 0x804E2520;
constexpr u32 CODE_MENU_ROOT = 0x804E07D8;
constexpr u32 CODE_MENU_STATE = 0x804E0034;
constexpr u32 CODE_MENU_OPEN = 4;
constexpr u32 CODE_MENU_SAVED_PAUSE = 0x804E006C;
constexpr u32 CODE_MENU_GAME_PAUSE = 0x805B8A08;
constexpr u32 CODE_MENU_ACTIVATION = 0x804E0C38;
constexpr u32 CODE_MENU_OFF = 2;
constexpr std::array<u32, 4> CODE_MENU_CHARACTERS{0x804E0F48, 0x804E1424, 0x804E1688, 0x804E18EC};
struct CodeMenuWord
{
  u32 address;
  u32 value;
};
std::span<const CodeMenuWord> CodeMenuDefaults();
bool CodeMenuPresent(const GuestMemory& memory);

struct PlayerPick
{
  bool human = false;
  int character = -1;
};
std::array<PlayerPick, 4> ReadPicks(const GuestMemory& memory);

bool SetWanted(Mode mode, u8 flags = 0, std::string_view code = {}, u8 teams = 0);
u8 RoomTeamsMask(bool doubles, const std::map<int, int>& teams);
u8 ReadRoomTeams(const GuestMemory& memory);
int WriteRoomTeams(GuestMemory& memory, u8 teams);
int ApplyRoomTeams(GuestMemory& memory, bool* began = nullptr);
Mode Wanted();
u8 WantedFlags();
u64 WantedGeneration();
bool HeaderInPlace();
void MemoryReplaced();
void SetHeaderFree(bool free);
void ExpectHeader(Mode mode, std::string_view code = {}, u8 flags = 0, int known_games = -1);
bool HeaderFitsRoom(const Header& h, Mode mode, Ruleset ruleset, std::string_view code, u8 flags);
bool SetFitsJoin(const SetBlock::SetState& set, int known_games);
std::optional<std::string> TakeMismatch();
std::optional<int> ConfirmNoShow(int confirmed);

void OnFrame(const Core::CPUThreadGuard& guard, int frame, bool resimulating, bool alone,
             u8 plugged);
struct TestQueue
{
  Mode mode = Mode::None;
  u8 coin = 0;
  u8 flags = 0;
  bool operator==(const TestQueue&) const = default;
};
std::optional<TestQueue> ParseTestQueue(const std::string& spec);
std::optional<TestQueue> TestQueueSpec();
std::optional<Mode> TestQueueMode();
}
}
