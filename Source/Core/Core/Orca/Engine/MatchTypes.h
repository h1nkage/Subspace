// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string>

#include "Common/CommonTypes.h"

namespace Orca::Engine
{
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

constexpr u8 FLAG_SOLO = 0x01;
constexpr u8 FLAG_QUEUE2 = 0x02;

struct FreshSeen
{
  bool menu_built = false;
  bool css = false;
  bool operator==(const FreshSeen&) const = default;
};

enum class SceneKind : u8
{
  Other,
  Fight,
  Between,
  Results,
};

struct GameResult
{
  enum class Kind
  {
    Win,
    Draw,
    Void,
  };
  Kind kind = Kind::Void;
  int winner_port = -1;
  std::string why;
  int frame = -1;
  int stage = -1;
  std::array<int, 4> characters{-1, -1, -1, -1};
  std::array<int, 4> stocks{-1, -1, -1, -1};
  unsigned winning_ports = 0;
  int players = 2;
  bool timeout = false;
  int start = -1;
  bool from_set_block = false;
  int number = 0;
  int how = 0;
  bool tiebreak = false;
  std::array<int, 2> ledge{-1, -1};
  int set_done = 0;
  int set_winner = -1;
  bool operator==(const GameResult&) const = default;

  std::string DetailJson() const;
};
}
