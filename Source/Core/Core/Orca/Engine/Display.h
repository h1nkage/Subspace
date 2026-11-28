// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <span>
#include <string>

#include "Common/CommonTypes.h"

namespace Orca::Engine
{
namespace Cursors
{
struct Rect
{
  s16 x0, y0, x1, y1;
  constexpr bool Contains(int x, int y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
};
struct Tile
{
  s8 stage;
  u8 page;
  Rect rect;
};
struct Point
{
  s16 x, y;
};

struct Layout
{
  u8 game = 0;
  Rect bounds{};
  s16 speed = 16;
  s16 dead_zone = 24;
  std::span<const Tile> tiles;
  bool pages = false;
  Rect page_button{};
  Rect random_button{};
  Rect random_art{};
  std::array<Point, 2> start{};
  float u0 = 0.5f, du = 0.01f, v0 = 0.5f, dv = -0.01f;
};

struct View
{
  bool on = false;
  u8 game = 0;
  u32 page = 0;
  struct Player
  {
    bool present = false;
    s16 x = 0, y = 0;
    s8 proposal = -1;
    bool local = false;
    std::string name;
  };
  std::array<Player, 2> players;
  s8 picked = -1;
  s8 turn = -1;
  u8 kind = 0;
  u8 left = 0;
  bool optional = false;
  bool mk_clause = false;
  bool both = false;
  s16 seconds = -1;
  bool operator==(const View&) const = default;
};

struct TurnCue
{
  enum class Mode : u8
  {
    Idle,
    Acting,
    Waiting,
  };
  Mode mode = Mode::Idle;
  bool local = false;
  std::string title;
  std::string detail;
  int seconds = -1;
  bool operator==(const TurnCue&) const = default;
};
}

struct SetDots
{
  bool on = false;
  std::array<int, 3> games{-1, -1, -1};
  int current = -1;
  bool operator==(const SetDots&) const = default;
};

struct Display
{
  u8 plates = 0;
  bool rules_bar = false;
  bool band_clock = false;
  bool line_relabelled = false;
  bool legend = false;
  std::string tip;
  std::string ranked_set;
  std::string ranked_turn;
  std::string score_line;
  Cursors::View cursors;
  const Cursors::Layout* layout = nullptr;
  std::array<const Cursors::Rect*, 2> proposals{};
  std::array<Cursors::TurnCue, 2> cues{};
  std::string mk_note;
  int picker = -1;
  std::string picker_ribbon;
  SetDots dots;
  u8 hud_ports = 0;
  std::array<u8, 4> hud_fade{};
};
}
