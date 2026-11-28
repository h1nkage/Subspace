// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

namespace Orca::UX::Rules
{
constexpr u32 CSS_HAND_NOTHING = 0;
constexpr u32 CSS_HAND_BUTTON = 1;
constexpr u32 CSS_HAND_GRID = 2;
constexpr u32 CSS_HAND_OWN_TOKEN = 3;
constexpr u32 CSS_HAND_EXIT = 4;
constexpr u32 CSS_HAND_TAKING = 6;
constexpr u32 CSS_HAND_GRID_HOLDING = 7;
constexpr u32 CSS_HAND_PLACING = 8;
constexpr u32 CSS_BUTTON_PLAYER_TYPE = 0x1D;
constexpr u32 CSS_BUTTON_NAME = 0x1C;
constexpr u32 CSS_BUTTON_BACK = 0x02;

constexpr bool CssHandOnBack(u32 target, u32 button)
{
  return target == CSS_HAND_EXIT && button == CSS_BUTTON_BACK;
}

struct CssToken
{
  bool valid = false;
  bool human = false;
  int character = 0x28;
  bool in_hand = true;
  bool flying = false;
  constexpr bool Down() const { return valid && human && character != 0x28 && !in_hand && !flying; }
  bool operator==(const CssToken&) const = default;
};
constexpr bool CssBUnpicks(const CssToken& token, bool b_held)
{
  return token.Down() && !b_held;
}
}
