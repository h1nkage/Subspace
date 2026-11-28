// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>

#include "Common/CommonTypes.h"
#include "Core/Orca/Engine/Display.h"
#include "Core/Orca/Games/Brawl/MatchBlock.h"
#include "InputCommon/GCPadStatus.h"

namespace Core
{
class CPUThreadGuard;
}

namespace Orca::UX
{
class GuestMemory;
}

namespace Orca::UX::StageCursors
{
constexpr u32 SHARED = MatchBlock::STAGE_CURSORS_SHARED;
constexpr u32 SHARED_SIZE = MatchBlock::STAGE_CURSORS_SHARED_SIZE;
constexpr u32 CURSORS = MatchBlock::STAGE_CURSORS;
constexpr u32 CURSOR_SIZE = 6;
constexpr u32 CURSORS_SIZE = 2 * CURSOR_SIZE;
static_assert(CURSORS_SIZE == MatchBlock::STAGE_CURSORS_SIZE);
constexpr u8 TAG_ON = 0xC2;

enum Button : u8
{
  BTN_A = 0x01,
  BTN_B = 0x02,
  BTN_X = 0x04,
  BTN_Y = 0x08,
  BTN_START = 0x10,
  BTN_Z = 0x20,
};
u8 ButtonsOf(u16 pad_buttons);

constexpr u8 GAME_BRAWL = 1;
constexpr u8 GAME_PPLUS = 2;
constexpr u8 GAME_BRAWL_RANKED = 3;

using Orca::Engine::Cursors::Layout;
using Orca::Engine::Cursors::Point;
using Orca::Engine::Cursors::Rect;
using Orca::Engine::Cursors::Tile;

const Layout& BrawlLayout();
const Layout& BrawlRankedLayout();
const Layout& PPlusLayout();
const Layout* LayoutFor(u8 game);

constexpr int NOTHING = -1;
constexpr int PAGE_BUTTON = -2;
constexpr int RANDOM_BUTTON = -3;
int Hovered(const Layout& layout, u32 page, int x, int y);
const Tile* TileOf(const Layout& layout, int stage, u32 page);
float MidX(const Rect& r);
float MidY(const Rect& r);

struct Cursor
{
  s16 x = 0;
  s16 y = 0;
  u8 now = 0;
  u8 seen = 0;
  bool operator==(const Cursor&) const = default;
};

struct State
{
  u8 game = 0;
  u8 follower = 0;
  u8 page_turn = 0;
  u8 page_frames = 0;
  std::array<Cursor, 2> cursors{};
  bool operator==(const State&) const = default;
};

std::optional<State> Read(const GuestMemory& memory);
int Write(GuestMemory& memory, const State& state);
int Clear(GuestMemory& memory);
constexpr u8 HELD_ALL = 0xFF;
State Start(const Layout& layout);

u8 TakePresses(State* state, int port);

int Latch(GuestMemory& memory, const std::array<std::optional<GCPadStatus>, 4>& raw);
void LatchFrame(const Core::CPUThreadGuard& guard,
                const std::array<std::optional<GCPadStatus>, 4>& raw);

using Orca::Engine::Cursors::View;

using Orca::Engine::Cursors::TurnCue;
TurnCue TurnText(const View& view, int port);
std::string MkClauseNote(const View& view);
void Publish(View view);
View Current();
}
