// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>

#include "Common/CommonTypes.h"
#include "Core/Orca/Games/Brawl/FreeSpace.h"

namespace Orca::UX
{
class GuestMemory;

namespace SetBlock
{
constexpr u32 BASE = FreeSpace::kMatchBlock.begin;
constexpr u32 SIZE = FreeSpace::kMatchBlockLimit - FreeSpace::kMatchBlock.begin;

constexpr u32 MAGIC = 0x53534D31;
constexpr u8 VERSION = 1;

constexpr u32 HDR_MAGIC = 0x00;
constexpr u32 HDR_VERSION = 0x04;
constexpr u32 HDR_MODE = 0x05;
constexpr u32 HDR_RULESET = 0x06;

enum class Mode : u8
{
  None = 0,
  Casual = 1,
  Ranked = 2,
};

enum class Ruleset : u8
{
  None = 0,
  Brawl = 1,
  PPlus = 2,
};

struct Header
{
  Mode mode = Mode::None;
  Ruleset ruleset = Ruleset::None;
  bool operator==(const Header&) const = default;
};

constexpr u32 SET_BEGIN = 0x210;
constexpr u32 SET_GAMES = 0x210;
constexpr u32 SET_WINS = 0x211;
constexpr u32 SET_DONE = 0x213;
constexpr u32 SET_WINNER = 0x214;
constexpr u32 SET_LAST_WINNER = 0x215;
constexpr u32 SET_TIEBREAK = 0x216;
constexpr u32 SET_TB_STREAK = 0x217;
constexpr u32 SET_FIGHT = 0x218;
constexpr u32 SET_FIGHT_FLAGS = 0x219;
constexpr u32 SET_LEDGE = 0x21A;
constexpr u32 SET_FIGHT_START = 0x21C;
constexpr u32 SET_SNAP_STOCKS = 0x220;
constexpr u32 SET_SNAP_PERCENT = 0x222;
constexpr u32 SET_ON_LEDGE = 0x226;
constexpr u32 SET_RECORDS = 0x230;
constexpr u32 RECORD_SIZE = 0x0C;
constexpr int MAX_RECORDS = 8;
constexpr u32 SET_END = SET_RECORDS + RECORD_SIZE * MAX_RECORDS;
static_assert(SET_END <= SIZE && FreeSpace::Detail::Free({BASE + SET_BEGIN, BASE + SET_END}));

constexpr u8 FIGHT_BETWEEN = 0x01;
constexpr u8 FIGHT_SUDDEN = 0x02;
constexpr u8 FIGHT_TIMEUP = 0x04;
constexpr u8 FIGHT_SEEN = 0x08;
constexpr u8 FIGHT_GONE = 0x10;

constexpr u8 NO_PORT = 0xFF;

enum class How : u8
{
  None = 0,
  Results = 1,
  Stocks = 2,
  Percent = 3,
  Ledge = 4,
  Tie = 5,
};

struct GameRecord
{
  u32 start = 0;
  u8 winner = NO_PORT;
  How how = How::None;
  u8 stage = 0xFF;
  bool tiebreak = false;
  bool timeout = false;
  std::array<u8, 2> characters{0xFF, 0xFF};
  std::array<u8, 2> stocks{0xFF, 0xFF};
  std::array<u8, 2> ledge{0, 0};
  bool operator==(const GameRecord&) const = default;
};

struct SetState
{
  u8 games = 0;
  std::array<u8, 2> wins{0, 0};
  u8 done = 0;
  u8 winner = NO_PORT;
  u8 last_winner = NO_PORT;
  bool tiebreak = false;
  u8 tb_streak = 0;
  bool fight = false;
  u8 fight_flags = 0;
  std::array<u8, 2> ledge{0, 0};
  u32 fight_start = 0;
  std::array<u8, 2> snap_stocks{0, 0};
  std::array<u16, 2> snap_percent{0, 0};
  std::array<u8, 2> on_ledge{0, 0};
  std::array<GameRecord, MAX_RECORDS> records{};
  bool operator==(const SetState&) const = default;

  int GameNumber() const { return wins[0] + wins[1] + 1; }
  u64 StagesWonBy(int port) const;
};

std::optional<Header> ReadHeader(const GuestMemory& memory);
int WriteHeader(GuestMemory& memory, const Header& header);
int Clear(GuestMemory& memory);

SetState ReadSet(const GuestMemory& memory);
int WriteSet(GuestMemory& memory, const SetState& state);

bool Mapped(const GuestMemory& memory);
}
}
