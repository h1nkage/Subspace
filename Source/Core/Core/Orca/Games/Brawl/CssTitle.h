// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Events.h"

namespace Core
{
class CPUThreadGuard;
}

namespace Orca::UX
{
class GuestMemory;
}

namespace Orca::UX::CssTitle
{
enum class Title : u8
{
  Game = 0,
  Casual = 1,
  Ranked = 2,
  Friends = 3,
};

std::string_view TitleName(Title title);

u32 ReadPick(const GuestMemory& memory);

Title Wanted(const GuestMemory& memory, int players);

struct Slot
{
  u32 address = 0;
  u16 game = 0;
  int step = 0;
  u16 ValueFor(Title title) const
  {
    return static_cast<u16>(game + step * static_cast<int>(title));
  }
  std::optional<Title> TitleOf(u16 value) const;
};
Slot FindSlot(const GuestMemory& memory);

int Apply(GuestMemory& memory, int players, Title* shown = nullptr);

void Frame(const Core::CPUThreadGuard& guard, bool resimulating,
           const std::vector<Orca::Events::PortInfo>& ports);
}
