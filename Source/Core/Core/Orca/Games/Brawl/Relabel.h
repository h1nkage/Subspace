// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
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

namespace Orca::UX::Relabel
{
enum class Band : u8
{
  Game = 0,
  LockIn = 1,
  WinnerFirst = 2,
  LoserNow = 3,
};

using Line = u8;
constexpr Line LINE_GAME = 0;
constexpr Line LINE_PICK_A_STAGE = 13;

enum class Title : u8
{
  Game = 0,
  Ranked = 1,
  Casual = 2,
};

struct Slot
{
  enum class Kind : u8
  {
    None,
    PaletteEntry,
    HighNibble,
    Byte,
  };
  Kind kind = Kind::None;
  u32 address = 0;
  u16 game = 0;
  int labels = 0;
  u16 ValueFor(int label, u16 now) const;
  std::optional<int> LabelOf(u16 now) const;
  u16 Read(const GuestMemory& memory) const;
  void Write(GuestMemory& memory, u16 value) const;
};

Slot FindBand(const GuestMemory& memory);
Slot FindLine(const GuestMemory& memory);
Slot FindTitle(const GuestMemory& memory);
std::array<Slot, 2> FindLegend(const GuestMemory& memory);

Band WantedBand(const GuestMemory& memory, const std::vector<Events::PortInfo>& ports);
Line WantedLine(const GuestMemory& memory);
Title WantedTitle(const GuestMemory& memory);
bool WantedLegend(const GuestMemory& memory);

bool BandUp(const GuestMemory& memory, const std::vector<Events::PortInfo>& ports);
bool ClockInBand(const GuestMemory& memory, const std::vector<Events::PortInfo>& ports);

Line LineFor(u8 kind, u8 port, int left);

struct Shown
{
  Band band = Band::Game;
  Line line = LINE_GAME;
  Title title = Title::Game;
  bool legend = false;
  bool band_up = false;
  bool operator==(const Shown&) const = default;
};

int Apply(GuestMemory& memory, const std::vector<Events::PortInfo>& ports, Shown* shown = nullptr);

Shown Frame(const Core::CPUThreadGuard& guard, bool resimulating,
            const std::vector<Events::PortInfo>& ports);
inline bool ClockInBand(const Shown& shown)
{
  return shown.band != Band::Game && shown.band_up;
}
Shown Current();
}
