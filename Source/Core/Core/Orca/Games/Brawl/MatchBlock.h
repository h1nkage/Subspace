// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string_view>

#include "Common/CommonTypes.h"
#include "Core/Orca/Engine/MatchTypes.h"
#include "Core/Orca/Games/Brawl/FreeSpace.h"

namespace Orca::UX
{
class GuestMemory;
}

namespace Orca::UX::MatchBlock
{
constexpr u32 BASE = FreeSpace::kMatchBlock.begin;
constexpr u32 SIZE = FreeSpace::kMatchBlockHead.Size();
constexpr u32 FULL_SIZE = FreeSpace::kMatchBlockLimit - FreeSpace::kMatchBlock.begin;

constexpr u32 MAGIC = BASE + 0x00;
constexpr u32 MAGIC_VALUE = 0x53534D31;
constexpr u32 VERSION = BASE + 0x04;
constexpr u8 VERSION_VALUE = 1;
constexpr u32 MODE = BASE + 0x05;
constexpr u32 RULESET = BASE + 0x06;
constexpr u32 COIN = BASE + 0x07;
constexpr u32 ROOM = BASE + 0x08;
constexpr u32 FLAGS = BASE + 0x0C;
using Orca::Engine::FLAG_QUEUE2;
using Orca::Engine::FLAG_SOLO;
constexpr u32 HEADER_SIZE = 0x10;

using Orca::Engine::Mode;
using Orca::Engine::Ruleset;

constexpr u32 SET = BASE + 0x10;
constexpr u32 SET_SIZE = 0x70;

constexpr u32 STEP = BASE + 0x80;
constexpr u32 STEP_SIZE = 0x40;

constexpr u32 CSS_TIMER = BASE + 0xC0;
constexpr u32 CSS_NO_SHOW = BASE + 0xC2;
constexpr u32 FOUGHT = BASE + 0xC3;
constexpr u32 PLUGGED = BASE + 0xC4;
constexpr u32 CSS_TIMER_START = BASE + 0xC8;
constexpr u32 PHASE1_SIZE = 0x10;

constexpr u32 SAVED_RULES = BASE + 0xD0;
constexpr u32 SAVED_CODE_MENU = BASE + 0xDA;
constexpr u32 SAVED_ITEMS = BASE + 0xE0;
constexpr u32 SAVED_STAGES = BASE + 0xEC;
constexpr u32 SAVED_RSS = BASE + 0x100;
constexpr u32 SAVED_RSS_SIZE = 0xA8;
constexpr u32 ROOM_TEAMS = BASE + 0x1A8;
constexpr u32 ROOM_TEAMS_MAGIC = 0x53535431;
constexpr u32 ROOM_TEAMS_MASK = ROOM_TEAMS + 4;
constexpr u32 ROOM_TEAMS_LOGGED = ROOM_TEAMS + 5;
constexpr u32 SAVED_FLAGS = BASE + 0x1F0;

constexpr u32 STAGE_CURSORS_SHARED = BASE + 0xF4;
constexpr u32 STAGE_CURSORS_SHARED_SIZE = 0x0C;
constexpr u32 STAGE_CURSORS = BASE + 0x1B4;
constexpr u32 STAGE_CURSORS_SIZE = 0x0C;

constexpr u32 QUEUE = BASE + 0x1C0;
constexpr u32 QUEUE_SIZE = 0x30;

constexpr u32 CHAR_ORDER = BASE + 0x200;
constexpr u32 CHAR_ORDER_SIZE = 0x10;
constexpr u32 RANKED_SET = BASE + 0x210;

constexpr u32 FRIENDS = BASE + 0x290;
constexpr u32 FRIENDS_TAG = FRIENDS + 0;
constexpr u8 FRIENDS_TAG_VALUE = 0xF5;
constexpr u32 FRIENDS_SEEN = FRIENDS + 1;
constexpr u32 FRIENDS_PENDING = FRIENDS + 2;
constexpr u32 FRIENDS_MENU_SINCE = FRIENDS + 4;
constexpr u32 FRIENDS_END = FRIENDS + 8;

static_assert(SAVED_RSS + SAVED_RSS_SIZE <= SAVED_FLAGS);
static_assert(SAVED_RULES + 10 <= SAVED_CODE_MENU && SAVED_CODE_MENU < SAVED_ITEMS);
static_assert(SAVED_STAGES + 8 <= STAGE_CURSORS_SHARED &&
              STAGE_CURSORS_SHARED + STAGE_CURSORS_SHARED_SIZE <= SAVED_RSS);
static_assert(SAVED_RSS + SAVED_RSS_SIZE <= STAGE_CURSORS &&
              STAGE_CURSORS + STAGE_CURSORS_SIZE <= QUEUE);
static_assert(SAVED_RSS + SAVED_RSS_SIZE <= ROOM_TEAMS && ROOM_TEAMS + 8 <= STAGE_CURSORS);
static_assert(RANKED_SET + 0x80 <= FRIENDS && FRIENDS_END <= FreeSpace::kMatchBlockLimit);
static_assert(FRIENDS_MENU_SINCE % 4 == 0);
static_assert(SAVED_RSS + SAVED_RSS_SIZE <= QUEUE && QUEUE + QUEUE_SIZE <= SAVED_FLAGS);
static_assert(SAVED_FLAGS < BASE + SIZE);
static_assert(CHAR_ORDER + CHAR_ORDER_SIZE <= RANKED_SET);
static_assert(CSS_TIMER_START + 4 <= BASE + 0xC0 + PHASE1_SIZE);

constexpr u32 STATE_SIZE = 0x90;
static_assert(BASE + STATE_SIZE <= FreeSpace::kMatchBlockHead.end);

constexpr u8 MODE_NONE = 0;
constexpr u8 MODE_CASUAL = 1;
constexpr u8 MODE_RANKED = 2;

constexpr u8 NONE = 0xFF;
constexpr u8 RANDOM = 0xFE;
constexpr u8 AUTO_TIMER = 0;
constexpr u8 AUTO_PICKED = 1;
constexpr u8 AUTO_AGREED = 2;
constexpr u8 AUTO_CASUAL = 3;
constexpr int MAX_GAMES = 8;

struct GameRecord
{
  u8 winner = NONE;
  u8 stage = NONE;
  std::array<u8, 2> characters{NONE, NONE};
  bool operator==(const GameRecord&) const = default;
};

struct State
{
  u8 mode = MODE_NONE;
  u8 ruleset = 0;
  u8 coin = 0;
  u8 games = 0;
  std::array<u8, 2> score{};
  u8 last_winner = NONE;
  std::array<u16, 2> won_on{};
  bool done = false;
  u8 set_winner = NONE;
  bool fight = false;
  u8 stage = NONE;
  std::array<u8, 2> characters{NONE, NONE};
  std::array<GameRecord, MAX_GAMES> records{};
  u8 flow = 0;
  u8 step = 0;
  u8 active = NONE;
  u8 step_done = 0;
  u16 struck = 0;
  u32 deadline = 0;
  bool auto_pick = false;
  u8 auto_stage = NONE;
  bool auto_press = false;
  u8 auto_why = 0;
  std::array<u8, 2> prefs{NONE, NONE};
  bool operator==(const State&) const = default;

  static State Fresh(u8 mode, u8 ruleset, u8 coin);
};

std::optional<State> Read(const GuestMemory& memory);
bool Present(const GuestMemory& memory);
int Write(GuestMemory& memory, const State& state);
int Clear(GuestMemory& memory);

u8 CoinFromCode(std::string_view code);
u32 RoomHash(std::string_view code);
}
