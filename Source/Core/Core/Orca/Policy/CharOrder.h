// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include "Common/CommonTypes.h"
#include "Core/Rollback/InputGate.h"

namespace Orca::UX::CharOrder
{
enum class Ruleset : u8
{
  None = 0,
  Brawl = 1,
  PPlus = 2,
};

enum class Order : u8
{
  Free = 0,
  WinnerFirst = 1,
};
constexpr Order kLaterGameOrder = Order::WinnerFirst;

struct SetView
{
  Ruleset ruleset = Ruleset::None;
  int game = 0;
  int last_winner = -1;
  Order order = kLaterGameOrder;
  u16 pick_frames = 0;
  bool operator==(const SetView&) const = default;
};
int FirstPicker(const SetView& set);

constexpr int kNoCharacter = 0x28;

struct CssPort
{
  bool human = false;
  int character = kNoCharacter;
  bool placed = false;
  bool flying = false;
  u16 raw = 0;
  u16 raw_new = 0;
  bool hand = false;
  float hand_x = 0;
  float hand_y = 0;
  bool Down() const { return human && placed && !flying && character != kNoCharacter; }
  bool operator==(const CssPort&) const = default;
};
struct CssView
{
  bool on_css = false;
  std::array<CssPort, 2> ports{};
  bool operator==(const CssView&) const = default;
};

enum class Step : u8
{
  Idle = 0,
  First = 1,
  Second = 2,
  Done = 3,
};

struct State
{
  u8 game = 0;
  Step step = Step::Idle;
  u8 first = 0;
  u8 locked = 0;
  u32 since = 0;
  u16 elapsed = 0;
  u8 held = 0;
  u8 anchored = 0;
  std::array<s16, 2> anchor_x{0, 0};
  std::array<s16, 2> anchor_y{0, 0};
  bool operator==(const State&) const = default;
};

constexpr float kGridMiddleX = 0.0f;
constexpr float kGridMiddleY = 5.0f;

bool Active(const SetView& set, const CssView& css, const State& state);
bool TimedOut(const SetView& set, const State& state);

State Advance(const SetView& set, const CssView& css, const State& state, int frame);

Rollback::InputGate::Masks Gate(const SetView& set, const CssView& css, const State& state);

u8 LockedPorts(const SetView& set, const CssView& css, const State& state);

int NoShow(const SetView& set, const CssView& css, const State& state);

int MkFreePicker(const std::array<int, 2>& characters, int meta_knight);
}
